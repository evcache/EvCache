#include "../include/common.h"
#include "../include/config.h"
#include "../include/utils.h"
#include "../include/cache_info.h"
#include "../include/lats.h"
#include "../include/evset.h"
#include "../include/evset_para.h"
#include "../vm_tools/gpa_hpa.h"
#include <bits/getopt_ext.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void* evsets = NULL;

i32 main(i32 argc, char* argv[])
{
    setup_segv_handler();

    u64 start_full = 0, end_full = 0,
        start_lat_detect = 0, end_lat_detect = 0;

    start_full = time_us();

    i32 opt;
    opterr = 0;
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {"debug", required_argument, 0, 'd'},
        {"cands-scaling", required_argument, 0, 's'},
        {"num-core", required_argument, 0, 'c'},
        {"num-sets", required_argument, 0, 'n'},
        {"uncertain-sets", required_argument, 0, 'u'},
        {"evsets-per-l2", required_argument, 0, 'f'},
        {"num-offsets", required_argument, 0, 'o'},
        {"vtop", no_argument, 0, 0},
        {0, 0, 0, 0}
    };

    init_def_args_conf();
    bool cache_level_valid = false;
    
    srand(time(NULL));

    if (optind < argc) {
        const char* cache_level = argv[optind];
        
        if (strcasecmp(cache_level, "L2") == 0) {
            g_config.cache_level = L2;
            cache_level_valid = true;
        } else if (strcasecmp(cache_level, "L3") == 0 || strcasecmp(cache_level, "LLC") == 0) {
            g_config.cache_level = L3;
            cache_level_valid = true;
        } else {
            return print_usage_vev(argv[0]);
        }
    }

    if (!cache_level_valid) {
        fprintf(stderr, ERR "cache level must be explicitly specified (L2 or L3/LLC).\n");
        return print_usage_vev(argv[0]);
    }

    i32 option_index = 0;
    while ((opt = getopt_long(argc, argv, "hv:d:s:tc:n:u:f:o:", long_options, &option_index)) != -1) {
        switch(opt) {
            case 'h': 
                return print_usage_vev(argv[0]);
                break;
            case 'n':
                g_config.num_sets = atoi(optarg);
                if (g_config.num_sets < 0) {
                    fprintf(stderr, ERR "-n cannot be less than or equal to 0\n");
                    return EXIT_FAILURE;
                }
                break;
            case 'o':
                g_config.num_offsets = atoi(optarg);
                if (g_config.num_offsets < 1 || g_config.num_offsets > NUM_OFFSETS) {
                    fprintf(stderr, ERR "-o must be between 1 and %lu\n", NUM_OFFSETS);
                    return EXIT_FAILURE;
                }
                break;
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
                    printf(WRN "Invalid debug level provided. Disabling.\n");
                }
                i32 debug_started = start_debug_mod();
                if (debug_started == -1) {
                    fprintf(stderr, ERR "Failed to open/start debugging module. Exiting.\n");
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
            case 'c':
                if (g_config.cache_level != L3) {
                    fprintf(stderr, ERR "Parallel evset construction is only available for L3/LLC.\n");
                    return EXIT_FAILURE;
                }
                if (optarg) {
                    g_config.num_threads = atoi(optarg);
                    if (g_config.num_threads % 2 != 0) {
                        fprintf(stderr, ERR "Specified core count to leverage for parallel mode must be even.\n");
                        puts("    One for main and one for helper.");
                        return EXIT_FAILURE;
                    }
                } else {
                    g_config.num_threads = 0;
                }
                break;
            case 'u':
                granular = true;
                g_config.num_l2_sets = atoi(optarg);
                if ((i32)g_config.num_l2_sets < 0) {
                    fprintf(stderr, ERR "-u cannot be negative\n");
                    return EXIT_FAILURE;
                }
                break;
            case 'f':
                g_config.evsets_per_l2 = atoi(optarg);
                if (g_config.evsets_per_l2 == 0) {
                    fprintf(stderr, ERR "-f must be greater than 0\n");
                    return EXIT_FAILURE;
                }
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "vtop") == 0) {
                    vtop = true;
                }
                break;
            default: return print_usage_vev(argv[0]);
        }
    }

    if (g_config.num_offsets > 1 && !granular) {
        fprintf(stderr, ERR "-o requires -u (granular mode)\n");
        return EXIT_FAILURE;
    }

    init_cache_info();

    start_lat_detect = time_us();
    init_cache_lats_thresh(DEF_LAT_REPS);
    end_lat_detect = time_us();

    print_cache_lats();

    if (verbose) {
        printf(V1 "Detected cache latency | %.3fms\n\n", 
              (end_lat_detect - start_lat_detect) / 1e3);
    }

    i32 result = EXIT_SUCCESS;

    // L2
    if (g_config.cache_level == L2) {
        if (g_config.num_sets == 0) g_config.num_sets = 1;
        evsets = (void*) build_l2_evset(g_config.num_sets);
        goto check_result;
    }

    // L3/LLC

    if (!granular && g_config.num_sets == 0) {
        evsets = (void*) build_single_l3_evset();
        goto check_result;
    }

    if (g_config.num_sets > NUM_OFFSETS) {
        printf(WRN "-n > NUM_OFFSETS. Setting it to %lu\n", NUM_OFFSETS);
        g_config.num_sets = NUM_OFFSETS;
    }
    // L3 with base pages and n > 0
    if (granular) {
        u32 max_l2_unc_sets = g_n_uncertain_l2_sets;
        if (g_config.num_l2_sets == 0) {
            g_config.num_l2_sets = max_l2_unc_sets;  // default: all L2 uncertain sets
        } else if (g_config.num_l2_sets > max_l2_unc_sets) {
            printf(WRN "-u (%u) > max L2 uncertain sets (%u). Setting to %u.\n",
                   g_config.num_l2_sets, max_l2_unc_sets, max_l2_unc_sets);
            g_config.num_l2_sets = max_l2_unc_sets;
        }

        evsets =
            (void *)build_l3_evsets_para_gran(g_config.num_l2_sets,
                                             g_config.num_offsets, NULL);
    } else if (vtop) {
        evsets = (void*) build_l3_evsets_para_vtop(g_config.num_sets);
    } else {
        evsets = (void*) build_l3_evsets_para(g_config.num_sets);
    }

check_result:
    if (verbose > 2) {
        print_arg_conf();
    }

    stop_debug_mod();

    if (!evsets) {
        result = EXIT_FAILURE;
        fprintf(stderr, ERR "No evset returned!\n");
    }

    cleanup_mem(NULL, NULL, 0);
    end_full = time_us();
    
    printf(INFO "Program completed | %.3fs\n", 
          (end_full - start_full) / 1e6);
    
    return result;
}
