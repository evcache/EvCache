#include "../include/common.h"
#include "../include/vset_ops.h"
#include "../include/utils.h"
#include "../include/cache_info.h"
#include "../include/lats.h"
#include "../include/config.h"
#include "../include/evset.h"
#include "../include/evset_para.h"
#include "../vm_tools/gpa_hpa.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <getopt.h>

extern i32 verbose;
extern i32 debug;
extern bool remap;
extern bool graph_mode;
extern bool vtop;
extern bool lcas_mode;
extern u32 lcas_period_ms;
extern bool fix_wait;
extern bool vset;
extern char *data_append;
extern EvBuildConf def_l2_build_conf;
extern EvBuildConf def_l3_build_conf;

typedef struct {
    EvSet ****complex;
    EvSet ***color_sets;
    EvSet **filters;
    EvSet ***filters_complex;
    u32 *color_counts;
    u32 n_colors;
    u32 n_ways;
    u32 wait_us;
    u32 n_pairs;
    u64 cycles_per_us;
    f64 *ewma;
    f64 alpha_rise;
    f64 alpha_fall;
    f64 **tot_avg;
    pthread_t *threads;
    l2c_occ_worker_arg *wargs;
} scan_ctx_t;

static bool init_live_scan_ctx(scan_ctx_t *ctx, EvSet ***filters, u32 n_colors)
{
    if (!ctx) return false;

    memset(ctx, 0, sizeof(*ctx));

    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = n_colors;
    if (g_config.evsets_per_l2 == 0)
        g_config.evsets_per_l2 = 2;
    if (g_config.num_offsets == 0)
        g_config.num_offsets = 64;

    ctx->wait_us = 7000;
    ctx->alpha_rise = 0.85;
    ctx->alpha_fall = 0.85;

    ctx->filters_complex = filters;

    ctx->complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                             g_config.num_offsets, filters);
    if (!ctx->complex)
        return false;

    ctx->n_colors = g_config.num_l2_sets;
    u32 max_sets = g_config.num_offsets * g_config.evsets_per_l2;
    ctx->color_sets = _calloc(ctx->n_colors, sizeof(EvSet**));
    ctx->filters = _calloc(ctx->n_colors, sizeof(EvSet*));
    ctx->color_counts = _calloc(ctx->n_colors, sizeof(u32));
    if (!ctx->color_sets || !ctx->color_counts || !ctx->filters)
        return false;

    for (u32 c = 0; c < ctx->n_colors; c++) {
        ctx->color_sets[c] = _calloc(max_sets, sizeof(EvSet*));
        if (filters)
            ctx->filters[c] = filters[c][c];
    }

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < ctx->n_colors; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = ctx->complex[off][c][e];
                if (ev && ev->size > 0) {
                    u32 idx = ctx->color_counts[c]++;
                    ctx->color_sets[c][idx] = ev;
                    if (ctx->n_ways == 0) {
                        ctx->n_ways = ev->size;
                    }
                }
            }
        }
    }

    u64 freq = get_cpu_freq_hz();
    ctx->cycles_per_us = freq ? freq / 1000000ULL : 2000ULL;

    i32 n_cores = n_system_cores();
    u32 req = g_config.num_threads ? g_config.num_threads : n_cores;
    if (req > (u32)n_cores) req = n_cores;
    ctx->n_pairs = req / 2;
    if (ctx->n_pairs == 0) ctx->n_pairs = 1;
    if (ctx->n_pairs > ctx->n_colors) ctx->n_pairs = ctx->n_colors;

    ctx->ewma = _calloc(ctx->n_colors, sizeof(f64));
    ctx->tot_avg = _calloc(ctx->n_colors, sizeof(f64*));
    ctx->threads = _calloc(ctx->n_pairs, sizeof(pthread_t));
    ctx->wargs = _calloc(ctx->n_pairs, sizeof(l2c_occ_worker_arg));
    if (!ctx->ewma || !ctx->tot_avg || !ctx->threads || !ctx->wargs)
        return false;

    for (u32 c = 0; c < ctx->n_colors; c++) {
        ctx->tot_avg[c] = _calloc(1, sizeof(f64));
        if (!ctx->tot_avg[c])
            return false;
    }

    u32 base = ctx->n_colors / ctx->n_pairs;
    u32 extra = ctx->n_colors % ctx->n_pairs;
    u32 next = 0;
    for (u32 i = 0; i < ctx->n_pairs; i++) {
        u32 count = base + (i < extra ? 1 : 0);
        ctx->wargs[i].wait_us = ctx->wait_us;
        ctx->wargs[i].cycles_per_us = ctx->cycles_per_us;
        ctx->wargs[i].iterations = 1;
        ctx->wargs[i].color_sets = ctx->color_sets;
        ctx->wargs[i].color_counts = ctx->color_counts;
        ctx->wargs[i].tot_avg = ctx->tot_avg;
        ctx->wargs[i].start_color = next;
        ctx->wargs[i].num_colors = count;
        ctx->wargs[i].core_main = i * 2;
        ctx->wargs[i].core_helper = i * 2 + 1;
        next += count;
    }

    return true;
}

static void free_live_scan_ctx(scan_ctx_t *ctx)
{
    if (!ctx) return;

    free_evset_complex(ctx->complex, g_config.num_offsets, ctx->n_colors,
                       g_config.evsets_per_l2);

    if (ctx->color_sets) {
        for (u32 c = 0; c < ctx->n_colors; c++)
            if (ctx->color_sets[c])
                free(ctx->color_sets[c]);
        free(ctx->color_sets);
    }

    if (ctx->color_counts) free(ctx->color_counts);
    if (ctx->filters) free(ctx->filters);
    if (ctx->ewma) free(ctx->ewma);
    if (ctx->threads) free(ctx->threads);
    if (ctx->wargs) free(ctx->wargs);
    if (ctx->tot_avg) {
        for (u32 c = 0; c < ctx->n_colors; c++)
            if (ctx->tot_avg[c])
                free(ctx->tot_avg[c]);
        free(ctx->tot_avg);
    }
}

static bool scan_iteration(scan_ctx_t *ctx, f64 *rates)
{
    if (!ctx) return false;

    for (u32 c = 0; c < ctx->n_colors; c++)
        ctx->tot_avg[c][0] = 0.0;

    for (u32 i = 0; i < ctx->n_pairs; i++) {
        ctx->wargs[i].wait_us = ctx->wait_us;
        if (pthread_create(&ctx->threads[i], NULL, l2c_occ_worker, &ctx->wargs[i])) {
            ctx->n_pairs = i;
            break;
        }
    }

    for (u32 i = 0; i < ctx->n_pairs; i++)
        pthread_join(ctx->threads[i], NULL);

    for (u32 c = 0; c < ctx->n_colors; c++) {
        f64 total = (f64)ctx->n_ways * ctx->color_counts[c];
        rates[c] = ctx->tot_avg[c][0] / total;
    }

    return true;
}

static void update_ewma(scan_ctx_t *ctx, f64 *rates)
{
    for (u32 c = 0; c < ctx->n_colors; c++) {
        f64 old = ctx->ewma[c];
        f64 alpha = rates[c] > old ? ctx->alpha_rise : ctx->alpha_fall;
        ctx->ewma[c] = alpha * old + (1.0 - alpha) * rates[c];
    }
}

static void print_hotness(scan_ctx_t *ctx, u32 *host_colors)
{
    for (u32 c = 0; c < ctx->n_colors; c++) {
        if (host_colors)
            printf("Color %2u: %6.2f%% (host color 0x%x)\n", c,
                   ctx->ewma[c] * 100.0, host_colors[c]);
        else
            printf("Color %2u: %6.2f%%\n", c, ctx->ewma[c] * 100.0);
    }
}

i32 result = EXIT_SUCCESS;

i32 main(i32 argc, char* argv[])
{
    setup_segv_handler();
    vset = true;

    u64 start_full = 0, end_full = 0,
        start_lat_detect = 0, end_lat_detect = 0;

    start_full = time_us();

    init_def_args_conf();

    bool live_mode = false;
    bool perf_mode = false;
    bool fraction_mode = false;
    bool freq_mode = false;
    bool freq_plot = false;
    bool u_provided = false, f_provided = false, o_provided = false;
    u32 scan_wait_us = 7000;
    i32 t_arg = -1;
    f64 alpha_r = 0.85, alpha_f = 0.85; // higher less reactive, lower more

    i32 opt;
    opterr = 0;
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {"debug", required_argument, 0, 'd'},
        {"num-core", required_argument, 0, 'c'},
        {"max-records", required_argument, 0, 'm'},
        {"activity-freq", no_argument, 0, 3},
        {"graph", required_argument, 0, 'G'},
        {"append", required_argument, 0, 'a'},
        {"uncertain-sets", required_argument, 0, 'u'},
        {"evsets-per-l2", required_argument, 0, 'f'},
        {"num-offsets", required_argument, 0, 'o'},
        {"remap", no_argument, 0, 'r'},
        {"wait-time", required_argument, 0, 'w'},
        {"max-time", required_argument, 0, 'M'},
        {"time-step", required_argument, 0, 't'},
        {"vtop", no_argument, 0, 0},
        {"live", no_argument, 0, 0},
        {"lcas", no_argument, 0, 0},
        {"perf", no_argument, 0, 0},
        {"fraction-check", no_argument, 0, 0},
        {"fix-wait", no_argument, 0, 0},
        {"alpha-rise", required_argument, 0, 1},
        {"alpha-fall", required_argument, 0, 2},
        {0, 0, 0, 0}
    };

    i32 option_index = 0;
    while ((opt = getopt_long(argc, argv, "hrv:d:c:m:G:a:u:f:o:w:M:t:", long_options, &option_index)) != -1) {
        switch(opt) {
            case 'h': 
                return print_usage_vset(argv[0]);
            case 'v':
                verbose = optarg ? atoi(optarg) : 1;
                if (verbose < 1 || verbose > 3) {
                    verbose = 0; // disable if invalid
                }
                break;
            case 'd': 
                debug = optarg ? atoi(optarg) : 1;
                if (debug < 1 || debug > 3) {
                    debug = 0; // disable
                    printf(WRN "Invalid debug level provided. Disabling.\n");
                } else {
                    // start debug module if debug is enabled
                    i32 debug_started = start_debug_mod();
                    if (debug_started == -1) {
                        fprintf(stderr, ERR "Failed to open/start debugging module. Exiting.\n");
                        return EXIT_FAILURE;
                    }
                }
                break;
            case 'r':
                if (!debug) {
                    printf(ERR "r, --remap requires debug (-d, --debug) to be enabled.\n");
                    return EXIT_FAILURE;
                }
                remap = true;
                break;
            case 'c':
                if (optarg) {
                    g_config.num_threads = atoi(optarg);
                    if (g_config.num_threads % 2 != 0) {
                        fprintf(stderr, ERR "-c requires an even number of cores\n");
                        return EXIT_FAILURE;
                    }
                } else {
                    g_config.num_threads = 0;
                }
                break;
            case 'm':
                if (optarg)
                    max_num_recs = strtoull(optarg, NULL, 10);
                break;
            case 'u':
                granular = true;
                g_config.num_l2_sets = atoi(optarg);
                u_provided = true;
                break;
            case 'f':
                granular = true;
                g_config.evsets_per_l2 = atoi(optarg);
                f_provided = true;
                break;
            case 'o':
                granular = true;
                g_config.num_offsets = atoi(optarg);
                o_provided = true;
                break;
            case 'G':
                graph_mode = true;
                if (strcmp(optarg, "0") == 0 || strcmp(optarg, "eviction-freq") == 0) {
                    graph_type = GRAPH_EVICTION_FREQ;
                    freq_mode = true;
                } else if (strcmp(optarg, "1") == 0 || strcmp(optarg, "evrate-wait") == 0) {
                    graph_type = GRAPH_EVRATE_WAIT;
                } else if (strcmp(optarg, "2") == 0 || strcmp(optarg, "occ-heatmap-l2color") == 0) {
                    graph_type = GRAPH_OCC_HEATMAP_L2COLOR;
                }
                else if (strcmp(optarg, "3") == 0 || strcmp(optarg, "evict-single") == 0)
                    graph_type = GRAPH_EVRATE_TIME;
                else if (strcmp(optarg, "4") == 0 || strcmp(optarg, "l2color-dist") == 0)
                    graph_type = GRAPH_L2COLOR_DIST;
                else {
                    fprintf(stderr, ERR "Unknown graph type '%s'\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'a':
                if (optarg && strlen(optarg) > 0)
                    data_append = strdup(optarg);
                break;
            case 'w':
                {
                    u64 wait_time = atoi(optarg);
                    if (wait_time <= 0) {
                        fprintf(stderr, ERR "Wait time must be a positive integer (microseconds)\n");
                        return EXIT_FAILURE;
                    }
                    wait_time_us = wait_time;
                    og_wait_time_us = wait_time;
                    scan_wait_us = wait_time;
                }
                break;
            case 'M':
                heatmap_max_time_us = atoi(optarg);
                break;
            case 't':
                t_arg = atoi(optarg);
                break;
            case 3:
                freq_plot = true;
                break;
            case 1:
                alpha_r = atof(optarg);
                break;
            case 2:
                alpha_f = atof(optarg);
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "vtop") == 0) {
                    vtop = true;
                } else if (strcmp(long_options[option_index].name, "live") == 0) {
                    live_mode = true;
                } else if (strcmp(long_options[option_index].name, "lcas") == 0) {
                    lcas_mode = true;
                } else if (strcmp(long_options[option_index].name, "perf") == 0) {
                    perf_mode = true;
                } else if (strcmp(long_options[option_index].name, "fraction-check") == 0) {
                    fraction_mode = true;
                } else if (strcmp(long_options[option_index].name, "fix-wait") == 0) {
                    fix_wait = true;
                }
                break;
            default:
                return print_usage_vset(argv[0]);
        }
    }

    lcas_alpha_rise = alpha_r;
    lcas_alpha_fall = alpha_f;

    if (fraction_mode) {
        granular = true;
        if (!u_provided) g_config.num_l2_sets = 16;
        if (!f_provided) g_config.evsets_per_l2 = 2;
        if (!o_provided) g_config.num_offsets = 64;
        if (!debug) {
            fprintf(stderr, ERR "--fraction-check requires -d/--debug\n");
            return EXIT_FAILURE;
        }
    }

    if (graph_mode && graph_type == GRAPH_EVRATE_TIME) {
        lcas_mode = true;
        vtop = true;
        if (!granular) {
            g_config.num_l2_sets = 1;
            g_config.evsets_per_l2 = 1;
            g_config.num_offsets = 1;
        }
    }

    u32 evict_iters = 0;
    if (t_arg != -1) {
        if (graph_mode && graph_type == GRAPH_EVRATE_TIME) {
            evict_iters = (u32)t_arg;
        } else if (live_mode || lcas_mode ||
                   (graph_mode && graph_type == GRAPH_OCC_HEATMAP_L2COLOR)) {
            scan_period_ms = (u32)t_arg;
        } else {
            heatmap_time_step_us = (u32)t_arg;
        }
    } else if (graph_mode && graph_type == GRAPH_EVRATE_TIME) {
        evict_iters = 100;
    }

    if (graph_mode || freq_mode) {
        if (ensure_data_dir() == -1)
            return EXIT_FAILURE;
    }

    if (!live_mode && !perf_mode && !fraction_mode && !graph_mode &&
        !remap && !lcas_mode && !freq_mode) {
        return print_usage_vset(argv[0]);
    }

    init_cache_info();
    
    start_lat_detect = time_us();
    init_cache_lats_thresh(DEF_LAT_REPS);
    end_lat_detect = time_us();

    if (verbose) {
        printf(V1 "Detected cache latency | %.3fms\n\n", (end_lat_detect - start_lat_detect) / 1e3);
    }

    print_cache_lats();

    if (remap) {
        u32 n_remap = check_mem_remap_cheat();
        if (n_remap < 0) { // err
            printf(ERR "Memory remap check failed.\n");
            result = EXIT_FAILURE;
        } else { // successful
            printf(SUC "%u remaps occured in %u seconds\n", n_remap, check_remap);
            result = EXIT_SUCCESS;
        }

        end_full = time_us();
        printf(INFO "Program completed | %.3fs\n",
            (end_full - start_full) / 1e6);

        return result;
    }

    if (fraction_mode) {
        if (fraction_check() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    if (live_mode) {
        u32 n_colors = g_config.num_l2_sets ? g_config.num_l2_sets : g_n_uncertain_l2_sets;
        scan_ctx_t ctx;
        if (!init_live_scan_ctx(&ctx, NULL, n_colors)) {
            return EXIT_FAILURE;
        }
        ctx.wait_us = scan_wait_us;
        ctx.alpha_rise = alpha_r;
        ctx.alpha_fall = alpha_f;
        f64 *rates = _calloc(ctx.n_colors, sizeof(f64));
        if (!rates) {
            free_live_scan_ctx(&ctx);
            return EXIT_FAILURE;
        }
        u32 *host_colors = NULL;
        if (debug > 0) {
            host_colors = _calloc(ctx.n_colors, sizeof(u32));
            if (!host_colors) {
                free(rates);
                free_live_scan_ctx(&ctx);
                return EXIT_FAILURE;
            }
            for (u32 c = 0; c < ctx.n_colors; c++) {
                if (ctx.color_counts[c]) {
                    u64 hpa = va_to_hpa(ctx.color_sets[c][0]->addrs[0]);
                    host_colors[c] = cache_get_color(hpa, &l2_info);
                }
            }
        }

        if (move_cgroup_hi() == -1) {
            fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                                "1) Running as root?\n"
                                "2) Ran scripts/setup_vset.sh ?\n");
            return EXIT_FAILURE;
        }

        scan_iteration(&ctx, rates);
        for (u32 c = 0; c < ctx.n_colors; c++)
            ctx.ewma[c] = rates[c];
        printf("LLC color hotness\n");
        printf("Wait: %u ms\n", ctx.wait_us / 1000);
        print_hotness(&ctx, host_colors);

        u32 consec_shrink_high = 0;
        u32 consec_expand_low = 0;
        while (1) {
            scan_iteration(&ctx, rates);
            update_ewma(&ctx, rates);
            u32 n_colors_hot = 0;
            u32 n_colors_cold = 0;
            for (u32 c = 0; c < ctx.n_colors; c++)
                if (ctx.ewma[c] >= 0.95)
                    n_colors_hot++;
                else if (ctx.ewma[c] <= 0.20)
                    n_colors_cold++;
            if (!fix_wait && ctx.wait_us > 1000) {

                if (n_colors_hot >= 2) // at least two of the colors need to hit the threshold
                    consec_shrink_high++;
                else
                    consec_shrink_high = 0;

                if (n_colors_cold == n_colors) // when all are idle
                    consec_expand_low++;
                else
                    consec_expand_low = 0;

                if (consec_shrink_high >= 2) { // 2 consecutive measurements
                    ctx.wait_us -= 1000;
                    consec_shrink_high = 0;
                }

                else if (consec_shrink_high) // once is enough to trigger jumping back to org val
                    ctx.wait_us = wait_time_us;
            }
            printf("\033[%uA", ctx.n_colors + 1);
            printf("\33[2K\rWait: %u ms\n", ctx.wait_us / 1000);
            print_hotness(&ctx, host_colors);
            fflush(stdout);
            usleep(scan_period_ms * 1000);
        }

        free(rates);
        if (host_colors) free(host_colors);
        free_live_scan_ctx(&ctx);
        return EXIT_SUCCESS;
    }

    if (perf_mode) {
        perf_prime_probe();
        return EXIT_SUCCESS;
    }

    u32 rate_cycles = 0;

    if (lcas_mode) {
        lcas_period_ms = scan_period_ms;
        if (g_config.num_l2_sets == 1 && g_config.evsets_per_l2 == 1 &&
            g_config.num_offsets == 1) {
            monitor_eviction_pct_single(evict_iters);
        } else if (graph_mode && graph_type == GRAPH_EVRATE_TIME) {
            monitor_eviction_rate_multi(evict_iters);
        } else {
            monitor_l3_occupancy_lcas();
        }
        return EXIT_SUCCESS;
    } else if (graph_mode) {
        if (graph_type == GRAPH_OCC_HEATMAP_L2COLOR) {
            rate_cycles = monitor_l3_occupancy_l2color();
        } else if (graph_type == GRAPH_EVRATE_WAIT) {
            rate_cycles = monitor_eviction_rate_wait();
        } else if (graph_type == GRAPH_EVRATE_TIME) {
            monitor_eviction_rate_multi(evict_iters);
            return EXIT_SUCCESS;
        } else if (graph_type == GRAPH_L2COLOR_DIST) {
            graph_l2color_distribution();
            return EXIT_SUCCESS;
        }
    }
    if (freq_mode) {
        rate_cycles = monitor_l3_activity_freq(freq_plot);
    }

    u64 cpu_freq_hz = get_cpu_freq_hz();

    if (debug > 0) {
        stop_debug_mod();
    }

    if (rate_cycles > 0) {
        if (cpu_freq_hz == 0) {
            printf(WRN "Couldn't retrieve CPU freq. Using retrieved cycles directly.\n");
            printf("LLC access rate: %u cycles\n", rate_cycles);
        } else {
            f64 rate_us = (f64)rate_cycles / ((f64)cpu_freq_hz / 1000000.0);
            printf(SUC "Access rate to LLC: %.3f microseconds (CPU @ %.2f GHz)\n",
                   rate_us, (f64)cpu_freq_hz / 1000000000.0);
        }
    }

    end_full = time_us();
    printf(INFO "Program completed | %.3fs\n", 
          (end_full - start_full) / 1e6);

    return EXIT_SUCCESS;
}
