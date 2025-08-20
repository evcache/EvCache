#include "../include/common.h"
#include "../include/config.h"
#include "../include/utils.h"
#include "../include/cache_info.h"
#include "../include/lats.h"
#include "../include/vset_ops.h"
#include "../include/evset_para.h"
#include "../include/cache_ops.h"
#include "../include/mem.h"
#include "../vm_tools/gpa_hpa.h"

#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>

extern i32 verbose;
extern i32 debug;

typedef struct {
    u32 tid;
    u32 wait_us;
    u64 cycles_per_us;
    EvSet ***color_sets;
    u32 *color_counts;
    u32 start_color;
    u32 num_colors;
    i32 core_main;
    i32 core_helper;
} poisoner_arg;

enum {
    OPT_USE_GPA = 1000,
    OPT_USE_HPA,
    OPT_DIST_ONLY,
};

static void write_dist_file(u32 *counts, u32 n_colors, u32 total)
{
    ensure_data_dir();
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "data/llc-dist-%04d-%02d-%02d-%02d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append, sizeof(filename) - strlen(filename) - 1);
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create distribution data file: %s\n", filename);
        return;
    }

    fprintf(fp, "# host LLC color distribution for guest color 0\n");
    fprintf(fp, "# host_colors: %u\n", n_colors);
    fprintf(fp, "# columns: host_color count pct\n");
    for (u32 c = 0; c < n_colors; c++) {
        f64 pct = total ? (100.0 * counts[c] / total) : 0.0;
        fprintf(fp, "%u %u %.2f\n", c, counts[c], pct);
    }
    fclose(fp);
    printf(INFO "LLC color distribution info written to: %s\n", filename);
}

typedef struct {
    u8 **pages;
    u32 count;
    i32 core;
} thrash_arg;

static void *thrash_worker(void *arg)
{
    thrash_arg *a = arg;
    set_cpu_affinity(a->core);
    while (1) {
        //access_array(a->pages, a->count);
        for (u32 i = 0; i < a->count; i++) {
            u8 val = (u8)rand();
            memset(a->pages[i], val, PAGE_SIZE);
        }
    }
    return NULL;
}

static void *poisoner_worker(void *arg)
{
    poisoner_arg *w = arg;
    set_cpu_affinity(w->core_main);
    helper_thread_ctrl hctrl = {0};
    start_helper_thread_pinned(&hctrl, w->core_helper);

    // Attach helper thread to all eviction sets this worker will operate on.
    attach_helper_to_evsets(w->color_sets, w->color_counts, w->start_color,
                            w->num_colors, &hctrl);

    if (verbose) {
        printf(V1 "thread pair %u working on colors [%u:%u]\n",
               w->tid, w->start_color, w->start_color + w->num_colors - 1);
    }

    while (1) {
        u64 p_start = _rdtsc();
        for (u32 idx = 0; idx < w->num_colors; idx++) {
            u32 color = w->start_color + idx;
            u32 set_cnt = w->color_counts[color];
            for (u32 s = 0; s < set_cnt; s++) {
                EvSet *ev = w->color_sets[color][s];
                // thrash
                access_array(ev->addrs, ev->size);
                /*
                for (u32 i = 0; i < ev->size; i++)
                    memset(ev->addrs[i], 0xF, PAGE_SIZE);
                */
            }
        }
        u64 p_end = _rdtsc();
        u32 prime_us = (u32)((p_end - p_start) / w->cycles_per_us);

        // pass -t 1 if you want continuous thrashing back to back
        // otherwise some waiting period would be triggered here
        // which reduces intensity of the poisoner
        if (w->wait_us > prime_us) {
            u32 remaining_us = w->wait_us - prime_us;
            u64 wait_cycles = (u64)remaining_us * w->cycles_per_us;
            u64 end_time = _rdtsc() + wait_cycles;
            while (_rdtsc() < end_time)
                ;
        }

        // keep thrash
        for (u32 idx = 0; idx < w->num_colors; idx++) {
            u32 color = w->start_color + idx;
            u32 set_cnt = w->color_counts[color];
            for (u32 s = 0; s < set_cnt; s++) {
                EvSet *ev = w->color_sets[color][s];
                access_array(ev->addrs, ev->size);
            }
        }
    }
    // ctrl-c...
    return NULL;
}

static void start_poisoning(EvSet ****complex)
{
    u32 n_colors = g_config.num_l2_sets;
    u32 max_sets_per_color = g_config.num_offsets * g_config.evsets_per_l2;

    EvSet ***color_sets = _calloc(n_colors, sizeof(EvSet**));
    u32 *color_counts = _calloc(n_colors, sizeof(u32));
    for (u32 c = 0; c < n_colors; c++)
        color_sets[c] = _calloc(max_sets_per_color, sizeof(EvSet*));

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < n_colors; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][c][e];
                if (ev && ev->size > 0) {
                    u32 idx = color_counts[c]++;
                    if (idx < max_sets_per_color)
                        color_sets[c][idx] = ev;
                }
            }
        }
    }

    u64 cpu_freq_hz = get_cpu_freq_hz();
    u64 cycles_per_us = cpu_freq_hz ? cpu_freq_hz / 1000000ULL : 2000ULL;

    i32 n_cores = n_system_cores();
    u32 req_threads = g_config.num_threads ? g_config.num_threads : n_cores;
    if (req_threads > (u32)n_cores)
        req_threads = n_cores;
    u32 n_pairs = req_threads / 2;
    if (n_pairs == 0)
        n_pairs = 1;
    if (n_pairs > n_colors)
        n_pairs = n_colors;

    pthread_t *threads = _calloc(n_pairs, sizeof(pthread_t));
    poisoner_arg *wargs = _calloc(n_pairs, sizeof(poisoner_arg));

    u32 base = n_colors / n_pairs;
    u32 extra = n_colors % n_pairs;
    u32 next_start = 0;

    for (u32 i = 0; i < n_pairs; i++) {
        u32 count = base + (i < extra ? 1 : 0);
        wargs[i].tid = i;
        wargs[i].wait_us = wait_time_us;
        wargs[i].cycles_per_us = cycles_per_us;
        wargs[i].color_sets = color_sets;
        wargs[i].color_counts = color_counts;
        wargs[i].start_color = next_start;
        wargs[i].num_colors = count;
        wargs[i].core_main = i * 2;
        wargs[i].core_helper = i * 2 + 1;
        next_start += count;
        pthread_create(&threads[i], NULL, poisoner_worker, &wargs[i]);
    }

    for (u32 i = 0; i < n_pairs; i++)
        pthread_join(threads[i], NULL);
}

i32 main(i32 argc, char *argv[])
{
    setup_segv_handler();

    init_def_args_conf();
    i32 opt;
    opterr = 0;
    bool use_gpa = false, use_hpa = false, dist_only = false;
    u32 z_scale = 1;
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {"debug", required_argument, 0, 'd'},
        {"num-core", required_argument, 0, 'c'},
        {"uncertain-sets", required_argument, 0, 'u'},
        {"evsets-per-l2", required_argument, 0, 'f'},
        {"num-offsets", required_argument, 0, 'o'},
        {"wait-time", required_argument, 0, 't'},
        {"append", required_argument, 0, 'a'},
        {"scale", required_argument, 0, 'z'},
        {"use-gpa", no_argument, 0, OPT_USE_GPA},
        {"use-hpa", no_argument, 0, OPT_USE_HPA},
        {"dist-only", no_argument, 0, OPT_DIST_ONLY},
        {0,0,0,0}
    };

    while ((opt = getopt_long(argc, argv, "hHv:d:c:u:f:o:t:a:z:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            return print_usage_vpo(argv[0]);
        case 'v':
            verbose = optarg ? atoi(optarg) : 1;
            if (verbose < 1 || verbose > 3) verbose = 0;
            break;
        case 'd':
            debug = optarg ? atoi(optarg) : 1;
            if (debug < 1 || debug > 3) {
                 debug = 0;
            } else if (start_debug_mod() == -1) {
                return EXIT_FAILURE;
            }
            g_config.debug_level = debug;
            break;
        case 'c':
            if (optarg) g_config.num_threads = atoi(optarg);
            break;
        case 'u':
            g_config.num_l2_sets = atoi(optarg);
            break;
        case 'f':
            g_config.evsets_per_l2 = atoi(optarg);
            break;
        case 'o':
            g_config.num_offsets = atoi(optarg);
            break;
        case 't':
            wait_time_us = atoi(optarg);
            break;
        case 'a':
            if (optarg && strlen(optarg) > 0)
                data_append = strdup(optarg);
            break;
        case 'z':
            z_scale = optarg ? atoi(optarg) : 1;
            if (z_scale == 0)
                z_scale = 1;
            break;
        case OPT_USE_GPA:
            use_gpa = true;
            break;
        case OPT_USE_HPA:
            use_hpa = true;
            break;
        case OPT_DIST_ONLY:
            dist_only = true;
            break;
        default:
            return 0;
        }
    }

    if (use_gpa && use_hpa) {
        fprintf(stderr, ERR "Cannot use --use-gpa and --use-hpa together\n");
        return EXIT_FAILURE;
    }
    if (dist_only && !use_gpa) {
        fprintf(stderr, ERR "--dist-only requires --use-gpa\n");
        return EXIT_FAILURE;
    }
    if ((z_scale != 1) && !use_gpa) {
        fprintf(stderr, ERR "-z/--scale only valid with --use-gpa\n");
        return EXIT_FAILURE;
    }

    if (use_hpa && debug == 0) {
        if (start_debug_mod() == -1)
            return EXIT_FAILURE;
    }

    if (use_gpa) {
        if (dist_only && debug == 0) {
            if (start_debug_mod() == -1)
                return EXIT_FAILURE;
        }

        init_cache_info();

        u64 region_size = 10ULL * 1024 * 1024 * z_scale;
        u32 total_pages = region_size / PAGE_SIZE;
        u8 *buf = mmap_shared_init_para(NULL, region_size, 0);
        if (!buf) {
            fprintf(stderr, ERR "Failed to allocate memory region\n");
            return EXIT_FAILURE;
        }

        printf(INFO "Allocated %.2f MiB (%u pages)\n", region_size / (1024.0 * 1024.0), total_pages);
        printf(INFO "Filtering pages for GPA LLC color 0...\n");

        u8 **filtered = _calloc(total_pages, sizeof(u8*));
        if (!filtered) {
            fprintf(stderr, ERR "Failed to allocate filtered array\n");
            munmap(buf, region_size);
            return EXIT_FAILURE;
        }

        u32 filtered_cnt = 0;
        for (u32 i = 0; i < total_pages; i++) {
            u8 *page = buf + i * PAGE_SIZE;
            u64 phys = va_to_pa(page);
            u32 color = cache_get_color(phys, &l3_info);
            if (color == 0) {
                filtered[filtered_cnt++] = page;
            }
        }

        printf(INFO "Filtered %u/%u pages to GPA LLC color 0 (%.2f MiB)\n",
               filtered_cnt, total_pages,
               filtered_cnt * PAGE_SIZE / (1024.0 * 1024.0));

        if (dist_only) {
            u32 host_colors = 1u << l3_info.unknown_sib;
            u32 *dist = _calloc(host_colors, sizeof(u32));
            if (!dist) {
                fprintf(stderr, ERR "Failed to allocate distribution array\n");
                free(filtered);
                munmap(buf, region_size);
                return EXIT_FAILURE;
            }
            for (u32 i = 0; i < filtered_cnt; i++) {
                u64 hpa = va_to_hpa(filtered[i]);
                u32 hc = cache_get_color(hpa, &l3_info);
                dist[hc]++;
            }
            for (u32 c = 0; c < host_colors; c++) {
                f64 pct = filtered_cnt ? (100.0 * dist[c] / filtered_cnt) : 0.0;
                printf("Host color %2u: %.2f%%\n", c, pct);
            }
            write_dist_file(dist, host_colors, filtered_cnt);
            free(dist);
            free(filtered);
            munmap(buf, region_size);
            return 0;
        }

        srand(time(NULL));

        u32 n_threads = g_config.num_threads ? g_config.num_threads : 1;
        pthread_t *threads = _calloc(n_threads, sizeof(pthread_t));
        thrash_arg *targs = _calloc(n_threads, sizeof(thrash_arg));
        u32 base = filtered_cnt / n_threads;
        u32 extra = filtered_cnt % n_threads;
        u32 next = 0;
        printf(SUC "Thrashing LLC with %u pages across %u threads. Ctrl-C to cancel.\n",
               filtered_cnt, n_threads);
        for (u32 t = 0; t < n_threads; t++) {
            u32 cnt = base + (t < extra ? 1 : 0);
            targs[t].pages = &filtered[next];
            targs[t].count = cnt;
            targs[t].core = t;
            next += cnt;
            pthread_create(&threads[t], NULL, thrash_worker, &targs[t]);
        }
        for (u32 t = 0; t < n_threads; t++)
            pthread_join(threads[t], NULL);

        free(targs);
        free(threads);
        free(filtered);
        munmap(buf, region_size);
    }

    if (g_config.num_offsets == 0)
        g_config.num_offsets = 1;
    if (g_config.evsets_per_l2 == 0)
        g_config.evsets_per_l2 = 1;

    init_cache_info();
    init_cache_lats_thresh(DEF_LAT_REPS);
    print_cache_lats();

    EvSet ****complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                  g_config.num_offsets, NULL);
    if (!complex) {
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return EXIT_FAILURE;
    }

    if (use_hpa) {
        EvSet *first = NULL;
        for (u32 o = 0; o < g_config.num_offsets && !first; o++) {
            for (u32 c = 0; c < g_config.num_l2_sets && !first; c++) {
                for (u32 e = 0; e < g_config.evsets_per_l2 && !first; e++) {
                    EvSet *ev = complex[o][c][e];
                    if (ev && ev->size > 0)
                        first = ev;
                }
            }
        }
        if (first) {
            // pick the first addr's addr to determine LLC color
            u32 host_color = cache_get_color(va_to_hpa(first->addrs[0]), &l3_info);
            printf(INFO "Host LLC color picked: 0x%x\n", host_color);
            u32 total = 0, kept = 0;
            u64 pages = 0;
            for (u32 o = 0; o < g_config.num_offsets; o++) {
                for (u32 c = 0; c < g_config.num_l2_sets; c++) {
                    for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                        EvSet *ev = complex[o][c][e];
                        if (ev && ev->size > 0) {
                            total++;
                            u32 hc = cache_get_color(va_to_hpa(ev->addrs[0]), &l3_info);
                            if (hc == host_color) {
                                kept++;
                                pages += ev->size;
                            } else {
                                ev->size = 0; // exclude it from being accessed
                            }
                        }
                    }
                }
            }
            printf(INFO "Filtered %u/%u eviction sets to host LLC color 0x%x (%.2f MiB)\n",
                   kept, total, host_color,
                   kept * PAGE_SIZE / (1024.0 * 1024.0));
        }
    }

    if (g_config.debug_level > 0) {
        printf("Host colors:\n");
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            EvSet *ev = complex[0][c][0];
            if (ev && ev->size > 0) {
                u64 hpa = va_to_hpa(ev->addrs[0]);
                u32 hc = cache_get_color(hpa, &l2_info);
                printf("color %2u -> host 0x%x\n", c, hc);
            }
        }
        printf("-------------------\n");
    }

    // any L2 color is also an L3 color
    printf(SUC "Started poisoning on %u colors. Ctrl-C to cancel.\n", g_config.num_l2_sets);
    start_poisoning(complex);
    return 0;
}

