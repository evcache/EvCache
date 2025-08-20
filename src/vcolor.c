#include "../include/common.h"
#include "../include/config.h"
#include "../include/utils.h"
#include "../include/cache_info.h"
#include "../include/evset.h"
#include "../include/evset_para.h"
#include "../include/lats.h"
#include "../vm_tools/gpa_hpa.h"
#include "../include/vset_ops.h"
#include <bits/getopt_ext.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define VCOLOR_MOD_PATH "/proc/vcolor_km"
#define VCOLOR_MAX_COLORS 16

extern EvBuildConf def_l2_build_conf;
extern EvBuildConf def_l3_build_conf;

static i32 mod_fd = -1;

static i32 start_vcolor_mod(void)
{
    mod_fd = open(VCOLOR_MOD_PATH, O_RDWR);
    if (mod_fd < 0) {
        perror("open vcolor_km");
        return -1;
    }
    return 0;
}

static void stop_vcolor_mod(void)
{
    if (mod_fd >= 0)
        close(mod_fd);
    mod_fd = -1;
}

typedef struct {
    u32 start_idx;
    u32 end_idx;
    u32 thread_idx;
    EvCands **cands;
} write_arg_t;

static void *write_worker(void *arg)
{
    write_arg_t *w = (write_arg_t *)arg;
    pin_thread_by_pid(pthread_self(), w->thread_idx);

    i32 fd = open(VCOLOR_MOD_PATH, O_RDWR);
    if (fd < 0) {
        perror("open vcolor_km");
        return NULL;
    }

    for (u32 c = w->start_idx; c < w->end_idx; c++) {
        EvCands *cand = w->cands[c];
        if (!cand)
            continue;
        for (u32 i = 0; i < cand->count; i++) {
            u64 pfn = va_to_pa(cand->addrs[i]) >> PAGE_SHIFT;
            char buf[64];
            i32 len = snprintf(buf, sizeof(buf), "%lx %u\n", pfn, c);
            if (write(fd, buf, len) != len)
                perror("write proc");
        }
    }
    fsync(fd);
    close(fd);
    return NULL;
}

static bool write_colors(EvCands **cands, u32 n_colors, bool do_clear)
{
    if (do_clear) {
        if (start_vcolor_mod() == -1)
            return false;
        if (write(mod_fd, "clear\n", 6) != 6) {
            perror("clear module");
            stop_vcolor_mod();
            return false;
        }
        stop_vcolor_mod();
    }

    u32 n_threads = g_config.num_threads ? g_config.num_threads : n_system_cores();
    if (n_threads > n_colors)
        n_threads = n_colors;

    pthread_t tids[n_threads];
    write_arg_t args[n_threads];
    u32 base_load = n_colors / n_threads;
    u32 rem = n_colors % n_threads;
    u32 curr = 0;

    for (u32 t = 0; t < n_threads; t++) {
        u32 cnt = base_load + (t < rem ? 1 : 0);
        args[t].start_idx = curr;
        args[t].end_idx = curr + cnt;
        args[t].thread_idx = t;
        args[t].cands = cands;
        curr += cnt;
        if (pthread_create(&tids[t], NULL, write_worker, &args[t])) {
            perror("thread create");
            n_threads = t;
            break;
        }
    }

    for (u32 t = 0; t < n_threads; t++)
        pthread_join(tids[t], NULL);


    for (u32 c = 0; c < n_colors; c++) {
        EvCands *cand = cands[c];
        if (!cand)
            continue;
        cand->evb->ref_cnt = 0; // noone else is going to use it anymore
        evcands_free(cands[c]);
        break; // no double frees
    }

    if (start_vcolor_mod() != -1) {
        ssize_t __w = write(mod_fd, "enable\n", 7);
        (void)__w;
        stop_vcolor_mod();
    }

    return true;
}

static bool write_order(u32 *order, u32 n_colors)
{
    if (start_vcolor_mod() == -1)
        return false;

    char buf[128];
    i32 len = snprintf(buf, sizeof(buf), "order");
    for (u32 i = 0; i < n_colors; i++)
        len += snprintf(buf + len, sizeof(buf) - len, " %u", order[i]);
    buf[len++] = '\n';
    bool ok = (write(mod_fd, buf, len) == len);
    fsync(mod_fd);
    stop_vcolor_mod();
    return ok;
}

static bool write_hot(u32 color)
{
    if (start_vcolor_mod() == -1)
        return false;

    char buf[32];
    i32 len = snprintf(buf, sizeof(buf), "hot %u\n", color);
    bool ok = (write(mod_fd, buf, len) == len);
    fsync(mod_fd);
    stop_vcolor_mod();
    return ok;
}

static void drop_caches(void)
{
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) {
        fputs("3\n", f);
        fclose(f);
    }
}

typedef struct {
    u32 start_idx;
    u32 end_idx;
    u32 thread_idx;
    EvCands *base_cands;
    EvCands **results;
    EvSet ***l2evsets;
    EvBuildConf *conf;
} color_arg_t;

static bool build_single_color(u32 idx, u32 thread_idx, EvCands *base,
                               EvCands **results, EvSet ***l2evsets,
                               EvBuildConf *conf)
{
    u32 offset = idx * CL_SIZE;
    EvBuildConf c = *conf;
    c.filter_ev = l2evsets[idx][idx];
    c.filter_ev->build_conf = &def_l2_build_conf;

    EvCands *cand = evcands_new(&l3_info, &c, base->evb);
    if (!cand)
        return false;

    if (evcands_populate(offset, cand, &c, thread_idx, offset)) {
        free(cand->addrs);
        free(cand);
        return false;
    }

    results[idx] = cand;
    return true;
}

static void *color_worker(void *arg)
{
    color_arg_t *ctx = (color_arg_t *)arg;
    pin_thread_by_pid(pthread_self(), ctx->thread_idx);
    for (u32 i = ctx->start_idx; i < ctx->end_idx; i++)
        build_single_color(i, ctx->thread_idx, ctx->base_cands,
                           ctx->results, ctx->l2evsets, ctx->conf);
    return NULL;
}

static EvCands **build_color_cands_para(EvBuildConf *conf, EvSet ***l2evsets)
{
    u64 start_alloc = time_us();
    EvCands *base_cands = evcands_new(&l3_info, conf, NULL);
    if (!base_cands) {
        fprintf(stderr, ERR "Failed to allocate EvBuffer\n");
        return NULL;
    }
    u64 end_alloc = time_us();
    if (verbose)
        printf(V1 "Completed EvCands allocation | %.3fms\n",
               (end_alloc - start_alloc) / 1e3);

    u32 n_colors = g_n_uncertain_l2_sets;
    EvCands **results = _calloc(n_colors, sizeof(EvCands*));
    if (!results)
        return NULL;

    u32 n_threads = g_config.num_threads ? g_config.num_threads : n_system_cores();
    if (n_threads > n_colors)
        n_threads = n_colors;

    pthread_t tids[n_threads];
    color_arg_t args[n_threads];
    u32 base_load = n_colors / n_threads;
    u32 rem = n_colors % n_threads;
    u32 curr = 0;

    u64 start_filter = time_us();

    for (u32 t = 0; t < n_threads; t++) {
        u32 cnt = base_load + (t < rem ? 1 : 0);
        args[t].start_idx = curr;
        args[t].end_idx = curr + cnt;
        args[t].thread_idx = t;
        args[t].base_cands = base_cands;
        args[t].results = results;
        args[t].l2evsets = l2evsets;
        args[t].conf = conf;
        curr += cnt;
        if (pthread_create(&tids[t], NULL, color_worker, &args[t])) {
            fprintf(stderr, ERR "thread create failed\n");
            n_threads = t;
            break;
        }
    }

    for (u32 t = 0; t < n_threads; t++)
        pthread_join(tids[t], NULL);

    u64 end_filter = time_us();
    if (verbose)
        printf(V1 "Completed filtering | %.3fms\n",
               (end_filter - start_filter) / 1e3);

    for (u32 i = 0; i < n_colors; i++)
        if (!results[i]) {
            fprintf(stderr, ERR "Failed to populate candidates for color %u\n", i);
            return NULL;
        }

    return results;
}

typedef struct {
    EvSet ****complex;
    EvSet ***color_sets;
    EvSet **filters;
    EvSet ***filters_complex; /* original L2 filters */
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

static bool init_scan_ctx(scan_ctx_t *ctx, EvSet ***filters, u32 n_colors)
{
    if (!ctx) return false;

    memset(ctx, 0, sizeof(*ctx));

    /* defaults for initial scan */
    g_config.num_l2_sets = n_colors;
    g_config.evsets_per_l2 = 2;
    g_config.num_offsets = 64;

    ctx->wait_us = 7000; // 7ms
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
                    if (ctx->n_ways == 0)
                        ctx->n_ways = ev->size;
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

static void free_scan_ctx(scan_ctx_t *ctx)
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

// rangs are:
// [0-40)-[40-65)-[65-85)-[85-100)
// e.g. if hottest color is currently at 25% eviction rate,
// and another color's rate rises to 30%, we won't bother
// changing allocation priority, because the new higher rate
// is not a level or more above the current range (got to be 40+%)
// furthermore, when observing a color's % in a range above our cur
// highest, we would have to observe it stays at that % domain
// for at least 3 rounds of measurements, before officialy setting
// that color as our new hottest.
static i32 hot_level(f64 h)
{
    f64 pct = h * 100.0;
    if (pct < 40.0)
        return 0;
    if (pct < 65.0)
        return 1;
    if (pct < 85.0)
        return 2;
    return 3;
}

typedef struct {
    u32 color;
    f64 hot;
} color_hot_t;

static i32 cmp_hot(const void *a, const void *b)
{
    const color_hot_t *aa = a, *bb = b;
    if (aa->hot < bb->hot)
        return 1;
    if (aa->hot > bb->hot)
        return -1;
    return 0;
}

static void build_sorted_order(color_hot_t *sorted, u32 n_colors,
                               u32 hottest, u32 *order)
{
    order[0] = hottest;
    u32 idx = 1;
    for (u32 i = 0; i < n_colors && idx < n_colors; i++) {
        if (sorted[i].color == hottest)
            continue;
        order[idx++] = sorted[i].color;
    }
}


i32 main(i32 argc, char *argv[])
{
    setup_segv_handler();
    u32 start_full = time_us(), end_full = 0;

    init_def_args_conf();

    i32 opt;
    opterr = 0;
    i32 sleep_time = 0;
    bool use_vset = false;
    u32 scan_period_ms = 1000;
    u32 scan_wait_us = 7000;
    u32 scan_cand_scale = 3;
    f64 alpha_r = 0.85, alpha_f = 0.85;
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {"debug", required_argument, 0, 'd'},
        {"cands-scaling", required_argument, 0, 's'},
        {"sleep-time", required_argument, 0, 't'},
        {"num-cores", required_argument, 0, 'c'},
        {"vset-scale", required_argument, 0, 'C'},
        {"vset", no_argument, 0, 0},
        {"scan-period", required_argument, 0, 1},
        {"alpha-rise", required_argument, 0, 2},
        {"alpha-fall", required_argument, 0, 3},
        {"scan-wait", required_argument, 0, 4},
        {0,0,0,0}
    };

    while ((opt = getopt_long(argc, argv, "hHv:d:s:t:c:C:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            return print_usage_vcolor(argv[0]);
        case 'v':
            g_config.verbose_level = optarg ? atoi(optarg) : 0;
            if (g_config.verbose_level < 1 || g_config.verbose_level > 3)
                g_config.verbose_level = 0;
            verbose = g_config.verbose_level;
            break;
        case 'd':
            g_config.debug_level = optarg ? atoi(optarg) : 1;
            if (g_config.debug_level < 1 || g_config.debug_level > 3) {
                g_config.debug_level = 0;
            } else if (start_debug_mod() == -1) {
                return EXIT_FAILURE;
            }
            debug = g_config.debug_level;
            break;
        case 's':
            g_config.cand_scaling = atoi(optarg);
            if (g_config.cand_scaling <= 0) {
                fprintf(stderr, ERR "invalid scaling factor: %u\n", g_config.cand_scaling);
                return EXIT_FAILURE;
            }
            break;
        case 't':
            sleep_time = atoi(optarg);
            if (sleep_time < 0)
                sleep_time = 0;
            break;
        case 'c':
            g_config.num_threads = atoi(optarg);
            if (g_config.num_threads <= 0)
                g_config.num_threads = n_system_cores();
            break;
        case 'C':
            scan_cand_scale = atoi(optarg);
            if (scan_cand_scale == 0)
                scan_cand_scale = 3;
            break;
        default:
            if (opt == 0) {
                use_vset = true;
            } else if (opt == 1) {
                scan_period_ms = atoi(optarg);
            } else if (opt == 2) {
                alpha_r = atof(optarg);
            } else if (opt == 3) {
                alpha_f = atof(optarg);
            } else if (opt == 4) {
                scan_wait_us = atoi(optarg);
            } else {
                return 0;
            }
            break;
        }
    }

    init_cache_info();
    init_cache_lats_thresh(DEF_LAT_REPS);
    print_cache_lats();

    u32 old_scale = g_config.cand_scaling;
    g_config.cand_scaling = 3;
    EvSet ***l2evsets = build_l2_evset(l2_info.n_sets);
    g_config.cand_scaling = old_scale;
    if (!l2evsets) {
        fprintf(stderr, ERR "failed to build L2 evsets\n");
        return EXIT_FAILURE;
    }

    init_def_l3_conf(&def_l3_build_conf, NULL, NULL);
    u32 n_colors = g_n_uncertain_l2_sets;
    if (!use_vset) {
        bool first = true;
        do {
            EvCands **cands = build_color_cands_para(&def_l3_build_conf, l2evsets);
            if (!cands)
                return EXIT_FAILURE;


            u32 start_insertion = time_us();

            if (!write_colors(cands, n_colors, first))
                return EXIT_FAILURE;

            u32 end_insertion = time_us();
            if (verbose)
                printf(V1 "Insertion to list completed | %.2fms\n",
                       (end_insertion - start_insertion)/1e3);

            first = false;
            if (sleep_time)
                sleep(sleep_time);
            else
                break;
        } while (sleep_time);

        end_full = time_us();
        printf(INFO "Program completed | %.3fs\n", 
            (end_full - start_full) / 1e6);

        return 0;
    } else {
        EvCands **cands = build_color_cands_para(&def_l3_build_conf, l2evsets);
        /* keep cand_scaling at scan_cand_scale for building scan sets */

        if (move_cgroup_hi() == -1) {
            fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                                "1) Running as root?\n"
                                "2) Ran scripts/setup_vset.sh ?\n");
            return EXIT_FAILURE;
        }

        if (!cands)
            return EXIT_FAILURE;

        u32 *host_colors = NULL;
        if (g_config.debug_level > 0) {
            host_colors = _calloc(n_colors, sizeof(u32));
            if (!host_colors)
                return EXIT_FAILURE;
            for (u32 c = 0; c < n_colors; c++) {
                if (cands[c] && cands[c]->count) {
                    u64 hpa = va_to_hpa(cands[c]->addrs[0]);
                    host_colors[c] = cache_get_color(hpa, &l2_info);
                }
            }
        }

        if (!write_colors(cands, n_colors, true)) {
            if (host_colors) free(host_colors);
            return EXIT_FAILURE;
        }

        printf("-------------------------\n");
        printf("Starting vSet Monitoring\n");
        printf("-------------------------\n");

        scan_ctx_t ctx;
        g_config.cand_scaling = scan_cand_scale; // -C
        def_l3_build_conf.cand_scale = scan_cand_scale;
        if (!init_scan_ctx(&ctx, l2evsets, n_colors)) {
            if (host_colors) free(host_colors);
            return EXIT_FAILURE;
        }
        ctx.wait_us = scan_wait_us;
        ctx.alpha_rise = alpha_r;
        ctx.alpha_fall = alpha_f;

        f64 *rates = _calloc(ctx.n_colors, sizeof(f64));
        if (!rates) {
            if (host_colors) free(host_colors);
            free_scan_ctx(&ctx);
            return EXIT_FAILURE;
        }

        scan_iteration(&ctx, rates);
        for (u32 c = 0; c < ctx.n_colors; c++)
            ctx.ewma[c] = rates[c];

        u32 order[VCOLOR_MAX_COLORS];
        color_hot_t tmp[VCOLOR_MAX_COLORS];
        for (u32 i = 0; i < ctx.n_colors; i++) {
            tmp[i].color = i;
            tmp[i].hot = ctx.ewma[i];
        }
        qsort(tmp, ctx.n_colors, sizeof(color_hot_t), cmp_hot);
        u32 hottest = tmp[0].color;
        i32 change_cnt = 0;
        u32 prev_cand = hottest;
        build_sorted_order(tmp, ctx.n_colors, hottest, order);
        write_order(order, ctx.n_colors);
        write_hot(hottest);
        drop_caches();

        printf("LLC color hotness\n");
        print_hotness(&ctx, host_colors);
        printf("Hottest color: %2u\n", hottest);

        u32 last_order[VCOLOR_MAX_COLORS];
        memcpy(last_order, order, sizeof(u32) * ctx.n_colors);

        while (1) {
            scan_iteration(&ctx, rates);
            update_ewma(&ctx, rates);

            for (u32 i = 0; i < ctx.n_colors; i++) {
                tmp[i].color = i;
                tmp[i].hot = ctx.ewma[i];
            }
            qsort(tmp, ctx.n_colors, sizeof(color_hot_t), cmp_hot);

            u32 cand = tmp[0].color;
            i32 cand_lvl = hot_level(tmp[0].hot);
            i32 hot_lvl_now = hot_level(ctx.ewma[hottest]);

            if (cand != hottest && cand_lvl - hot_lvl_now >= 1) {
                if (cand == prev_cand)
                    change_cnt++;
                else {
                    prev_cand = cand;
                    change_cnt = 1;
                }
            } else {
                change_cnt = 0;
                prev_cand = cand;
            }

            if (change_cnt >= 3) {
                hottest = cand;
                change_cnt = 0;
                prev_cand = cand;
                build_sorted_order(tmp, ctx.n_colors, hottest, order);
                write_order(order, ctx.n_colors);
                write_hot(hottest);
                drop_caches();
                memcpy(last_order, order, sizeof(u32) * ctx.n_colors);
            } else {
                build_sorted_order(tmp, ctx.n_colors, hottest, order);
                if (memcmp(order, last_order, sizeof(u32) * ctx.n_colors)) {
                    write_order(order, ctx.n_colors);
                    memcpy(last_order, order, sizeof(u32) * ctx.n_colors);
                }
            }

            printf("\033[%uA", ctx.n_colors + 1);
            print_hotness(&ctx, host_colors);
            printf("Hottest color: %2u\n", hottest);
            fflush(stdout);
            usleep(scan_period_ms * 1000);
        }

        free(rates);
        if (host_colors) free(host_colors);
        free_scan_ctx(&ctx);
        return 0;
    }
}
