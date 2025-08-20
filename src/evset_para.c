#include "../include/evset_para.h"
#include "../include/cache_info.h"
#include "../include/helper_thread.h"
#include "../include/utils.h"
#include "../include/bitwise.h"
#include "../include/config.h"
#include "../include/lats.h"
#include "../include/evset.h"
#include "../vm_tools/gpa_hpa.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_uint next_offset_idx = ATOMIC_VAR_INIT(0);
static atomic_uint next_core_id = ATOMIC_VAR_INIT(0);
static atomic_uint n_pair_init_done = ATOMIC_VAR_INIT(0);
static u64 total_runtime_limit = 10;
extern EvBuildConf def_l2_build_conf;
extern EvBuildConf def_l3_build_conf;
EvSet **build_l3_lowerev(void);
extern bool final_l3_evset(EvSet* l3ev);
extern u32 g_evsets_per_offset[NUM_OFFSETS];

#if SANITY_CHECK_ALL_EVS
static void sanity_check_l3_evcomplex(EvSet ****complex, u32 n_offsets,
                                         u32 n_l2_sets, u32 l3_cnt,
                                         u64 total_built)
{
    if (!complex || total_built == 0)
        return;

    bool own_debug = false;
    if (debug == 0) {
        if (start_debug_mod() == -1) {
            fprintf(stderr, ERR "verification failed to open debug module\n");
            return;
        }
        own_debug = true;
    }

    u32 max_sets = l3_info.n_sets_one_slice;
    u32 max_slices = l3_info.n_slices;
    bool *seen = _calloc((u64)max_sets * max_slices, sizeof(bool));
    if (!seen) {
        if (own_debug) stop_debug_mod();
        return;
    }

    u64 valid = 0;
    u64 dups = 0;

    for (u32 off = 0; off < n_offsets; off++) {
        for (u32 set = 0; set < n_l2_sets; set++) {
            for (u32 j = 0; j < l3_cnt; j++) {
                EvSet *ev = complex[off][set][j];
                if (!ev || !ev->addrs || ev->size == 0)
                    continue;

                u64 thpa = va_to_hpa(ev->target_addr);
                u32 sib = cache_get_sib(thpa, &l3_info);
                u32 slice = l3_slice_skx_20(thpa);

                bool ok = true;
                for (u32 k = 0; k < ev->size; k++) {
                    u64 hpa = va_to_hpa(ev->addrs[k]);
                    if (cache_get_sib(hpa, &l3_info) != sib ||
                        l3_slice_skx_20(hpa) != slice) {
                        ok = false;
                        break;
                    }
                }

                if (ok)
                    valid++;

                u64 idx = (u64)slice * max_sets + sib;
                if (seen[idx])
                    dups++;
                else
                    seen[idx] = true;
            }
        }
    }

    free(seen);

    f64 pct = total_built ? (valid * 100.0 / (f64)total_built) : 0.0;
    printf(INFO "Post-build sanity: %lu/%lu valid evsets (%.2f%%). Duplicates: %lu\n",
           valid, total_built, pct, dups);

    if (own_debug)
        stop_debug_mod();
}
#endif

u32 get_next_core_id(u32 max_cores)
{
    u32 core_id = atomic_fetch_add(&next_core_id, 1);
    return core_id % max_cores;
}

void reset_core_id_counter(void)
{
    atomic_store(&next_core_id, 0);
}

void calc_thread_workload(u32 n_pairs, u32 n_offsets, u32 *pair_workload)
{
    if (n_pairs == 0 || n_offsets == 0 || !pair_workload)
        return;
        
    u32 base_load = n_offsets / n_pairs;
    u32 remainder = n_offsets % n_pairs;
    
    for (u32 i = 0; i < n_pairs; i++) {
        pair_workload[i] = base_load + (i < remainder ? 1 : 0);
    }
}

u32 find_optimal_vcpu_pairs(cpu_topology_t *topo, u32 n_requested_pairs, vcpu_pair_assignment_t *pair_assignments)
{
    if (!topo || !pair_assignments || n_requested_pairs == 0)
        return 0;
        
    u32 valid_pairs_found = 0;
    bool *vcpu_used = _calloc(topo->nr_cpus, sizeof(bool));
    
    if (!vcpu_used) {
        fprintf(stderr, ERR "failed to allocate vCPU usage tracking\n");
        return 0;
    }
    
    // Process one socket at a time
    for (i32 socket = 0; socket < topo->nr_sockets && valid_pairs_found < n_requested_pairs; socket++) {
        // First, identify all vCPUs in this socket
        i32 *socket_vcpus = _calloc(topo->nr_cpus, sizeof(i32));
        u32 socket_vcpu_count = 0;
        
        if (!socket_vcpus) {
            fprintf(stderr, ERR "failed to allocate socket vCPUs array\n");
            free(vcpu_used);
            return valid_pairs_found;
        }
        
        for (i32 i = 0; i < topo->nr_cpus; i++) {
            if (topo->cpu_to_socket[i] == socket && !vcpu_used[i]) {
                socket_vcpus[socket_vcpu_count++] = i;
            }
        }
        
        // Form as many pairs as possible from this socket
        for (u32 i = 0; i < socket_vcpu_count && valid_pairs_found < n_requested_pairs; i += 2) {
            if (i + 1 >= socket_vcpu_count) break; // Need at least 2 vCPUs
            
            i32 vcpu1 = socket_vcpus[i];
            i32 vcpu2 = socket_vcpus[i + 1];
            
            // Ensure they're not SMT siblings
            if (topo->relation_matrix[vcpu1][vcpu2] == CPU_RELATION_SMT) {
                // Try to find a non-SMT partner
                bool found_partner = false;
                for (u32 j = i + 2; j < socket_vcpu_count; j++) {
                    i32 vcpu3 = socket_vcpus[j];
                    if (topo->relation_matrix[vcpu1][vcpu3] != CPU_RELATION_SMT) {
                        // Found a non-SMT partner, swap vcpu2 and vcpu3
                        i32 tmp = socket_vcpus[i + 1];
                        vcpu2 = vcpu3;
                        socket_vcpus[i + 1] = vcpu3;
                        //socket_vcpus[j] = socket_vcpus[i + 1];
                        socket_vcpus[j] = tmp;
                        found_partner = true;
                        break;
                    }
                }
                
                if (!found_partner) {
                    continue; // Skip this pair, couldn't find non-SMT partner
                }
            }
            
            // We have a valid same-socket, non-SMT pair
            pair_assignments[valid_pairs_found].main_vcpu = vcpu1;
            pair_assignments[valid_pairs_found].helper_vcpu = vcpu2;
            pair_assignments[valid_pairs_found].assigned = true;
            valid_pairs_found++;
            
            vcpu_used[vcpu1] = true;
            vcpu_used[vcpu2] = true;
            
            if (verbose > 1) {
                printf(V2 "Found pair %u: vCPUs %d and %d on socket %d\n", 
                      valid_pairs_found - 1, vcpu1, vcpu2, socket);
            }
        }
        
        free(socket_vcpus);
    }
    
    free(vcpu_used);
    return valid_pairs_found;
}

gran_pair_assignment_t* calc_gran_assignments(u32 n_pairs, u32 n_offsets, u32 n_l2_sets, u32 *total_assignments)
{
    u32 total_work = n_offsets * n_l2_sets;
    *total_assignments = total_work;

    if (total_work == 0 || n_pairs == 0) return NULL;

    if (n_pairs > total_work) {
        n_pairs = total_work;
    }
    
    gran_pair_assignment_t *pair_assignments = _calloc(n_pairs, sizeof(gran_pair_assignment_t));
    if (!pair_assignments) return NULL;
    
    // calc base work per pair and remainder
    u32 base_work = total_work / n_pairs;
    u32 remainder = total_work % n_pairs;
    
    gran_work_assignment_t *all_work = _calloc(total_work, sizeof(gran_work_assignment_t));
    if (!all_work) {
        free(pair_assignments);
        return NULL;
    }
    
    u32 work_idx = 0;
    for (u32 l2_set = 0; l2_set < n_l2_sets; l2_set++) {
        for (u32 offset = 0; offset < n_offsets; offset++) {
            all_work[work_idx].offset_idx = offset;
            all_work[work_idx].l2_set_idx = l2_set;
            work_idx++;
        }
    }
    
    // distribute work to pairs
    u32 current_work_idx = 0;
    for (u32 pair = 0; pair < n_pairs; pair++) {
        u32 pair_work_count = base_work + (pair < remainder ? 1 : 0);
        
        if (pair_work_count > 0) {
            pair_assignments[pair].assignments = _calloc(pair_work_count, sizeof(gran_work_assignment_t));
            if (!pair_assignments[pair].assignments) {
                // cleanup on failure
                for (u32 i = 0; i < pair; i++) {
                    free(pair_assignments[i].assignments);
                }
                free(pair_assignments);
                free(all_work);
                return NULL;
            }
            
            pair_assignments[pair].n_assignments = pair_work_count;
            pair_assignments[pair].pair_idx = pair;
            
            // copy work assignments to this pair
            for (u32 i = 0; i < pair_work_count && current_work_idx < total_work; i++) {
                pair_assignments[pair].assignments[i] = all_work[current_work_idx++];
            }
        } else {
            pair_assignments[pair].assignments = NULL;
            pair_assignments[pair].n_assignments = 0;
            pair_assignments[pair].pair_idx = pair;
        }
    }
    
    free(all_work);
    return pair_assignments;
}

void free_gran_assignments(gran_pair_assignment_t *assignments, u32 n_pairs)
{
    if (!assignments) return;
    
    for (u32 i = 0; i < n_pairs; i++) {
        free(assignments[i].assignments);
    }
    free(assignments);
}

void *evset_thread_worker_gran(void *arg)
{
    thread_pair *tp = (thread_pair *)arg;
    if (!tp) return NULL;
    
    gran_pair_assignment_t *my_assignment = (gran_pair_assignment_t*)tp->idxs;
    
    if (set_cpu_affinity(tp->core_id_main) != 0) {
        fprintf(stderr, ERR "thread %u failed to set cpu affinity to core %u\n", 
                tp->thread_idx, tp->core_id_main);
    }
    
    if (start_helper_thread_pinned(&tp->helper_ctrl, tp->core_id_helper)) {
        fprintf(stderr, ERR "thread %u failed to start helper thread on core %u\n", 
                tp->thread_idx, tp->core_id_helper);
        return NULL;
    }
    
    printf("├─ Thread pair %u: working on cores %u (main) and %u (helper) - %u assignments (%u evsets)\n",
           tp->thread_idx, tp->core_id_main, tp->core_id_helper,
           my_assignment->n_assignments,
           my_assignment->n_assignments * tp->evsets_per_l2);

    atomic_fetch_add(&n_pair_init_done, 1);
    
    init_def_l3_conf(&tp->l3_conf, NULL, &tp->helper_ctrl);
    tp->total_built = 0;
    
    // process all pre-assigned work using granular build_evsets_at
    for (u32 i = 0; i < my_assignment->n_assignments; i++) {
        u32 offset_idx = my_assignment->assignments[i].offset_idx;
        u32 l2_set_idx = my_assignment->assignments[i].l2_set_idx;
        u32 offset = offset_idx * CL_SIZE;
        
        // setup L3 config for this specific L2 set
        init_def_l3_conf(&tp->l3_conf, tp->l2evsets[offset_idx][l2_set_idx], &tp->helper_ctrl);
        tp->l3_conf.filter_ev = tp->l2evsets[offset_idx][l2_set_idx];
        tp->l3_conf.lower_ev = tp->l2evsets[offset_idx][l2_set_idx];
        
        u64 l3_cnt;
        // build eviction sets for this L2 set
        EvSet **l3_evsets = build_evsets_at(
            offset, &tp->l3_conf, &l3_info, tp->l3_cands[offset_idx][l2_set_idx], &l3_cnt,
            NULL, NULL, tp->l2evsets[offset_idx], 0, tp->evsets_per_l2);

        tp->result_complex[offset_idx][l2_set_idx] = l3_evsets;

        if (l3_evsets && l3_cnt > 0) {
            for (u32 j = 0; j < l3_cnt; j++) {
                EvSet *ev = l3_evsets[j];
                if (!ev || ev->size == 0)
                    continue;

                EvBuildConf *conf_copy = malloc(sizeof(*conf_copy));
                if (conf_copy)
                    *conf_copy = tp->l3_conf;
                ev->build_conf = conf_copy;
                tp->total_built++;
            }

            if (verbose > 1 && l3_evsets[0] && l3_evsets[0]->size > 0) {
                printf(V2 "pair %u: built evset at offset 0x%x, L2 set %u (size: %u)\n",
                       tp->thread_idx, offset, l2_set_idx, l3_evsets[0]->size);
            }
        }
    }
    
    stop_helper_thread(&tp->helper_ctrl);
    
    printf(SUC "Thread pair %u completed: %lu/%u evsets built\n",
           tp->thread_idx, tp->total_built,
           my_assignment->n_assignments * tp->evsets_per_l2);
    
    return NULL;
}

EvSet**** build_l3_evsets_para_gran(u32 n_l2_sets, u32 n_offsets,
                                    EvSet ***pre_l2evsets)
{
    u64 vtop_start = 0, vtop_total = 0;
    i32 n_cores = n_system_cores();
    u32 n_pairs = g_config.num_threads == 0 ? 
                  (n_cores / 2) :
                  (g_config.num_threads / 2);

    if (n_cores <= 1) {
        fprintf(stderr, ERR "Need at least 2 cores to run vevict\n");
        return NULL;
    }

    // adjust for odd n_cores
    if (n_cores % 2 != 0 && g_config.num_threads == 0) {
        printf(NOTE "odd number of cores (%d) detected. using %u thread pairs\n", 
               n_cores, n_pairs);
    }

    if (n_offsets > NUM_OFFSETS) {
        printf(WRN "requested offsets (%u) > max available (%lu). setting to %lu\n",
               n_offsets, NUM_OFFSETS, NUM_OFFSETS);
        n_offsets = NUM_OFFSETS;
    }
    
    n_unc_l2_sets = g_n_uncertain_l2_sets;
    if (n_l2_sets > n_unc_l2_sets) {
        printf(WRN "requested L2 sets (%u) > max uncertain sets (%u). setting to %u\n",
               n_l2_sets, n_unc_l2_sets, n_unc_l2_sets);
        n_l2_sets = n_unc_l2_sets;
    }

    u32 total_work = n_offsets * n_l2_sets;
    if (n_pairs > total_work)
        n_pairs = total_work;

    if (verbose) {
        printf(V1 "granular mode: %u pairs, %u offsets, %u L2 sets per offset\n",
               n_pairs, n_offsets, n_l2_sets);
    }

    // build L2 evsets for selected offsets only if none provided
    EvSet ***l2evsets = pre_l2evsets;
    if (!l2evsets) {
        l2evsets = build_l2_evset(l2_info.n_sets);
        if (!l2evsets) {
            fprintf(stderr, ERR "No L2evset complex returned.\n");
            return NULL;
        }
    }

    init_def_l3_conf(&def_l3_build_conf, NULL, NULL);
    
    // build L3 candidates for selected offsets
    //EvCands ***l3_cands = build_evcands_all(&def_l3_build_conf, l2evsets);
    EvCands ***l3_cands = build_evcands_all_para(&def_l3_build_conf, l2evsets);
    if (!l3_cands) {
        fprintf(stderr, ERR "failed to build L3/LLC candidates\n");
        return NULL;
    }

    // accumulated results by pairs
    EvSet ****l3evset_complex = _calloc(NUM_OFFSETS, sizeof(*l3evset_complex));
    if (!l3evset_complex) {
        fprintf(stderr, ERR "failed to allocate L3 evset complex\n");
        return NULL;
    }
    
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        l3evset_complex[n] = _calloc(n_unc_l2_sets, sizeof(**l3evset_complex));
        if (!l3evset_complex[n]) {
            fprintf(stderr, ERR "failed to allocate L3/LLC sub-complex for offset %u\n", n);
            return NULL;
        }
    }
    
    // --vtop
    cpu_topology_t *topo = NULL;
    vcpu_pair_assignment_t *vcpu_pairs = NULL;
    u32 valid_vcpu_pairs = 0;
    
    if (vtop) {
        vtop_start = time_us();
        topo = get_vcpu_topo();
        vtop_total += time_us() - vtop_start;
        
        if (topo) {
            printf(INFO "vTop: vCPU topology layout:\n");
            print_cpu_topology(topo);
            
            vcpu_pairs = _calloc(n_pairs, sizeof(vcpu_pair_assignment_t));
            if (vcpu_pairs) {
                valid_vcpu_pairs = find_optimal_vcpu_pairs(topo, n_pairs, vcpu_pairs);
                
                if (valid_vcpu_pairs < n_pairs) {
                    printf(WRN "vTop: found only %u valid vCPU pairs; reducing from requested %u pairs.\n", 
                          valid_vcpu_pairs, n_pairs);
                    n_pairs = valid_vcpu_pairs;
                }
            }
        } else {
            fprintf(stderr, WRN "vTop: failed to detect topology, proceeding w/o vTop\n");
            vtop = false;
        }
    }

    u32 total_assignments;
    gran_pair_assignment_t *pair_assignments =
        calc_gran_assignments(n_pairs, n_offsets, n_l2_sets, &total_assignments);
    if (!pair_assignments) {
        fprintf(stderr,
                ERR "failed calculating granular work assignments. Are options (-u and -o N) provided?\n");
        if (vcpu_pairs) free(vcpu_pairs);
        if (topo) free(topo);
        return NULL;
    }

    atomic_store(&n_pair_init_done, 0);
    
    thread_pair *pairs = _calloc(n_pairs, sizeof(thread_pair));
    if (!pairs) {
        fprintf(stderr, ERR "failed to allocate thread pairs\n");
        free_gran_assignments(pair_assignments, n_pairs);
        if (vcpu_pairs) free(vcpu_pairs);
        if (topo) free(topo);
        return NULL;
    }
    
    reset_core_id_counter();
    u64 start_time = time_us();
    
    for (u32 i = 0; i < n_pairs; i++) {
        pairs[i].thread_idx = i;
        pairs[i].result_complex = l3evset_complex;
        pairs[i].l2evsets = l2evsets;
        pairs[i].l3_cands = l3_cands;
        pairs[i].n_uncertain_l2_sets = n_unc_l2_sets;
        pairs[i].evsets_per_l2 = g_config.evsets_per_l2;
        pairs[i].idxs = (u32*)&pair_assignments[i]; // assign specific work

        // pinning based on vtop or default
        if (vtop && vcpu_pairs && i < valid_vcpu_pairs) {
            pairs[i].core_id_main = vcpu_pairs[i].main_vcpu;
            pairs[i].helper_ctrl.core_id = vcpu_pairs[i].helper_vcpu;
        } else {
            pairs[i].core_id_main = get_next_core_id(n_cores);
            pairs[i].helper_ctrl.core_id = get_next_core_id(n_cores);
        }
        pairs[i].core_id_helper = pairs[i].helper_ctrl.core_id;

        pairs[i].helper_ctrl.running = false;
        pairs[i].helper_ctrl.waiting = false;
    }
    
    printf(INFO "Starting %u thread pairs for granular L3 evset construction\n", n_pairs);
    printf(INFO "Expected: %u eviction set(s) per L2 uncertain set (%u sets) across %u offsets\n",
           g_config.evsets_per_l2, n_l2_sets, n_offsets);
    if (vtop && topo) {
        printf(INFO "vTop: using topology-aware vCPU pinning\n");
    }
    
    for (u32 i = 0; i < n_pairs; i++) {
        if (pthread_create(&pairs[i].thread_id, NULL, evset_thread_worker_gran, &pairs[i])) {
            fprintf(stderr, ERR "failed to create thread %u\n", i);
            // cleanup already created threads
            for (u32 j = 0; j < i; j++) {
                pthread_cancel(pairs[j].thread_id);
                pthread_join(pairs[j].thread_id, NULL);
            }
            free(pairs);
            free_gran_assignments(pair_assignments, n_pairs);
            if (vcpu_pairs) free(vcpu_pairs);
            if (topo) free(topo);
            return NULL;
        }
    }

    // wait for all to init
    while(atomic_load(&n_pair_init_done) != n_pairs);
    printf(INFO "Progress:\n");
    
    u64 total_built = 0;
    for (u32 i = 0; i < n_pairs; i++) {
        pthread_join(pairs[i].thread_id, NULL);
        total_built += pairs[i].total_built;
    }
    
    u64 end_time = time_us();

    if (debug > 1) {
        printf(D2 "Granular eviction sets:\n");
        
        u32 display_offsets = (debug > 1) ? n_offsets : 1;
        display_offsets = _min(display_offsets, n_offsets);
        
        for (u32 offset_idx = 0; offset_idx < display_offsets; offset_idx++) {
            u32 offset = offset_idx * CL_SIZE;
            printf(D2 "Offset 0x%x evsets:\n", offset);
            
            u32 built_at_offset = 0;
            for (u32 set = 0; set < n_l2_sets; set++) {
                EvSet **evset_arr = l3evset_complex[offset_idx][set];
                
                EvSet *evset = evset_arr[0];
                if (!evset || !evset->addrs || evset->size == 0) {
                    printf("  Set %2u: [%s]\n\n", set, RED "INVALID" RST);
                    continue;
                }
                
                built_at_offset++;
                
                // show evset info
                u64 target_hpa = va_to_hpa(evset->target_addr);
                u64 target_l3_sib = cache_get_sib(target_hpa, &l3_info);
                u32 target_slice = l3_slice_skx_20(target_hpa);
                
                printf("  Set %2u: size=%u\n", set, evset->size);
                printf("    Target: %p -> HPA 0x%lx (L3 SIB: 0x%lx | Slice: %u)\n",
                       evset->target_addr, target_hpa, target_l3_sib, target_slice);
                
                u32 display_addrs = (debug > 2) ? evset->size : 2;
                u32 width = n_digits(evset->size);
                
                for (u32 i = 0; i < display_addrs; i++) {
                    u64 hpa = va_to_hpa(evset->addrs[i]);
                    u64 hpa_l3_sib = cache_get_sib(hpa, &l3_info);
                    u32 hpa_slice = l3_slice_skx_20(hpa);
                    
                    bool l3_match = (target_l3_sib == hpa_l3_sib);
                    bool slice_match = (target_slice == hpa_slice);
                    
                    printf("    [%*u]: %p -> HPA 0x%lx [%s] [%s]\n",
                           width, i, evset->addrs[i], hpa,
                           l3_match ? GRN "L3 SIB Match" RST : RED "L3 SIB Bad" RST,
                           slice_match ? GRN "Slice Match" RST : RED "Slice Bad" RST);
                }
                
                if (evset->size > display_addrs) {
                    printf("    ... (%u more addresses) | show with -d 3\n", evset->size - display_addrs);
                }
                printf("\n");
            }
            
            printf("  Offset 0x%x summary: %u/%u L2 sets successfully built\n\n", 
                   offset, built_at_offset, n_l2_sets);

            if (debug == 1 && n_offsets > 1) {
                printf("  ... (%u more offsets) | show with -d 2\n\n",
                       n_offsets - 1);
            }
        }
    }


    
    printf(SUC "granular construction completed | %.3fms\n",
          (end_time - start_time) / 1e3);
    u32 total_possible = n_l2_sets * n_offsets * g_config.evsets_per_l2;
    printf(SUC "Total evsets built: %lu/%u (%.2f%%)\n",
           total_built, total_possible, (f64)total_built * 100.0 / (f64)total_possible);

    // calculate minimal eviction set size across built sets
    u32 l3_cnt = g_config.evsets_per_l2;
    i32 *final_min_evsize = _calloc(n_offsets * n_l2_sets * l3_cnt, sizeof(u32));
    u32 i_fme = 0;
    if (final_min_evsize) {
        for (u32 off = 0; off < n_offsets; off++) {
            for (u32 set = 0; set < n_l2_sets; set++) {
                EvSet **arr = l3evset_complex[off][set];
                for (u32 j = 0; j < l3_cnt; j++) {
                    EvSet *ev = arr ? arr[j] : NULL;
                    if (!ev || !ev->addrs || ev->size == 0 || ev->size > l3_info.n_ways) {
                        final_min_evsize[i_fme++] = 0;
                    } else {
                        final_min_evsize[i_fme++] = ev->size;
                    }
                }
            }
        }
        u32 min_evsize =
            calc_min_cluster(final_min_evsize, n_offsets * n_l2_sets * l3_cnt);
        printf(INFO "Minimal eviction set size: %u (Size: %.2f MiB)\n", min_evsize,
              (f64)((f64)min_evsize * l3_info.size / l3_info.n_ways / (1<<20))
              );
        free(final_min_evsize);
    }

#if SANITY_CHECK_ALL_EVS
    {
        printf(INFO "Starting sanity check. Might take a moment.\n");
        u32 l3_cnt_verify = g_config.evsets_per_l2;
        if (l3_cnt_verify == 0)
            l3_cnt_verify = 1;
        sanity_check_l3_evcomplex(l3evset_complex, n_offsets, n_l2_sets,
                                     l3_cnt_verify, total_built);
    }
#endif
    
    if (vtop && topo) {
        printf(INFO "vTop total time: %.3fms\n", vtop_total / 1e3);
    }
    

    // cleanup
    free_gran_assignments(pair_assignments, n_pairs);
    free(pairs);
    if (vcpu_pairs) free(vcpu_pairs);
    if (topo) free(topo);

    return l3evset_complex;
}

void *vtop_para_worker(void *arg) 
{
    vtop_thread_pair_t *tp = (vtop_thread_pair_t *)arg;
    
    if (set_cpu_affinity(tp->main_vcpu) != 0) {
        fprintf(stderr, ERR "Thread %u failed to set affinity to vCPU %d\n", 
                tp->thread_idx, tp->main_vcpu);
    } else if (verbose > 1) {
        printf(V2 "Thread %u pinned to vCPU %d\n", tp->thread_idx, tp->main_vcpu);
    }
    
    if (start_helper_thread_pinned(&tp->helper_ctrl, tp->helper_vcpu)) {
        fprintf(stderr, ERR "Thread %u failed to start helper on vCPU %d\n", 
                tp->thread_idx, tp->helper_vcpu);
        return NULL;
    } else if (verbose > 1) {
        printf(V2 "Thread %u helper pinned to vCPU %d\n", 
                tp->thread_idx, tp->helper_vcpu);
    }
    
    printf("├─ Thread pair %u: working on vCPUs %d (main) and %d (helper)\n", 
            tp->thread_idx, tp->main_vcpu, tp->helper_vcpu);
    
    init_def_l3_conf(&tp->l3_conf, NULL, &tp->helper_ctrl);
    
    // keep taking work as long as offsets are available
    while (1) {
        u32 curr_off_idx = atomic_fetch_add(tp->global_next_offset, 1);
        
        if (curr_off_idx >= tp->max_offsets) {
            break;
        }

        tp->n_offsets_picked += 1;
        
        u32 n = tp->idxs[curr_off_idx];
        u32 offset = n * CL_SIZE;
        
        for (u32 i = 0; i < n_unc_l2_sets; i++) {
            init_def_l3_conf(&tp->l3_conf, tp->l2evsets[n][i], &tp->helper_ctrl);
            tp->l3_conf.filter_ev = tp->l2evsets[n][i];
            tp->l3_conf.lower_ev = tp->l2evsets[n][i];
            
            u64 l3_cnt;
            EvSet **l3_evsets = build_evsets_at(
                offset, &tp->l3_conf, &l3_info, tp->l3_cands[n][i], &l3_cnt,
                NULL, NULL, tp->l2evsets[n], 0, 0);

            tp->result_complex[n][i] = l3_evsets;
            if (l3_evsets) {
                for (u32 j = 0; j < l3_cnt; j++) {
                    EvSet *ev = l3_evsets[j];
                    if (!ev || ev->size == 0)
                        continue;

                    EvBuildConf *conf_copy = malloc(sizeof(*conf_copy));
                    if (conf_copy)
                        *conf_copy = tp->l3_conf;
                    ev->build_conf = conf_copy;
                    tp->total_built++;
                }
            }
        }
        
        printf("├─ Pair %u: offset 0x%x Done\n", tp->thread_idx, offset);
    }
    
    stop_helper_thread(&tp->helper_ctrl);
    
    printf(SUC "Thread pair %u done: %lu/%lu evsets successfully built (%u offsets)\n", 
            tp->thread_idx, tp->total_built, 
            tp->n_offsets_picked * cache_uncertainty(&l3_info),
            tp->n_offsets_picked);
    
    return NULL;
}

void *evset_thread_worker(void *arg)
{
    thread_pair *tp = (thread_pair *)arg;
    if (!tp) return NULL;
    
    if (set_cpu_affinity(tp->core_id_main) != 0) {
        fprintf(stderr, ERR "Thread %u failed to set cpu affinity to core %u\n", 
                tp->thread_idx, tp->core_id_main);
    }
    
    if (start_helper_thread_pinned(&tp->helper_ctrl, tp->core_id_helper)) {
        fprintf(stderr, ERR "Thread %u failed to start helper on core %u\n", 
                tp->thread_idx, tp->core_id_helper);
        return NULL;
    }
    
    printf("├─ Thread pair %u: working on cores %u (main) and %u (helper)\n", 
            tp->thread_idx, tp->core_id_main, tp->core_id_helper);

    atomic_fetch_add(&n_pair_init_done, 1);
    
    init_def_l3_conf(&tp->l3_conf, NULL, &tp->helper_ctrl);
    u64 l3_cnt = 0;
    tp->total_built = 0;
    
    u64 start_time = time_us();
    u32 max_offsets = g_config.num_sets;
    u32 n_offsets_picked = 0;
    
    while (1) {
        // pick next available offset to work on...
        u32 curr_off_idx = atomic_fetch_add(&next_offset_idx, 1);
        
        // (if there is one)
        if (curr_off_idx >= max_offsets) {
            break;
        }

        n_offsets_picked += 1;
        
        u32 n = tp->idxs[curr_off_idx];
        u32 offset = n * CL_SIZE;
        
        for (u32 i = 0; i < n_unc_l2_sets; i++) {
            init_def_l3_conf(&tp->l3_conf, tp->l2evsets[n][i], &tp->helper_ctrl);
            tp->l3_conf.filter_ev = tp->l2evsets[n][i];
            tp->l3_conf.lower_ev = tp->l2evsets[n][i];
            EvSet **l3_evsets = build_evsets_at(
                offset, &tp->l3_conf, &l3_info, tp->l3_cands[n][i], &l3_cnt,
                NULL, NULL, tp->l2evsets[n], 0, 0);

            tp->result_complex[n][i] = l3_evsets;
            if (l3_evsets) {
                for (u32 j = 0; j < l3_cnt; j++) {
                    EvSet *ev = l3_evsets[j];
                    if (!ev || ev->size == 0)
                        continue;

                    EvBuildConf *conf_copy = malloc(sizeof(*conf_copy));
                    if (conf_copy)
                        *conf_copy = tp->l3_conf;
                    ev->build_conf = conf_copy;
                    tp->total_built++;
                }
            }
            
            if (total_runtime_limit &&
                ((time_us() - start_time) / 1e6 >= total_runtime_limit * 60)) {
                printf(WRN "Thread pair %u timeout at offset 0x%x\n", tp->thread_idx, offset);
                goto timeout_break;
            }
        }
        
        printf("├─ Pair %u: offset 0x%x Done\n", tp->thread_idx, offset);
    }
    
timeout_break:
    stop_helper_thread(&tp->helper_ctrl);
    
    u64 s = n_offsets_picked * cache_uncertainty(&l3_info);
    printf(SUC "Thread pair %u done: %lu/%lu evsets successfully built (%lu offsets)\n", 
           tp->thread_idx, tp->total_built, s, s / cache_uncertainty(&l3_info));
    
    return NULL;
}

EvSet**** build_l3_evsets_para(u32 n_offset)
{
    i32 n_cores = n_system_cores();
    u32 n_pairs = g_config.num_threads == 0 ? 
                 (n_cores / 2) : // def: max utilization
                 (g_config.num_threads / 2); // if specified
    
    if (n_cores <= 1) {
        fprintf(stderr, ERR "Need at least 2 cores to run vevict\n");
        return NULL;
    }
    
    // adjust if odd (num of a VM's vCPU based on instance could be odd)
    if (n_cores % 2 != 0 && g_config.num_threads == 0) {
        printf(NOTE "odd cores (%d) detected. using %u thread pairs\n", 
               n_cores, n_pairs);
    }

    if (n_pairs > n_offset && n_offset > 0) {
        n_pairs = n_offset;
    }

    EvSet ***l2evsets = build_l2_evset(l2_info.n_sets); // shared
    if (!l2evsets) {
        fprintf(stderr, ERR "failed to build L2 evset complex\n");
        return NULL;
    }

    n_unc_l2_sets = g_n_uncertain_l2_sets;
    
    u32 idxs[NUM_OFFSETS] = {0};
    for (u32 i = 0; i < NUM_OFFSETS; i++) {
        idxs[i] = i;
    }
    
    init_def_l3_conf(&def_l3_build_conf, NULL, NULL);
    
    //EvCands ***l3_cands = build_evcands_all(&def_l3_build_conf, l2evsets);
    EvCands ***l3_cands = build_evcands_all_para(&def_l3_build_conf, l2evsets);
    if (!l3_cands) {
        fprintf(stderr, ERR "failed to build L3/LLC candidates\n");
        return NULL;
    }
    
    n_offset = _min(n_offset, NUM_OFFSETS);
    if (n_offset == 0) {
        n_offset = NUM_OFFSETS;
    }
    
    atomic_store(&next_offset_idx, 0);
    atomic_store(&n_pair_init_done, 0);
    
    EvSet ****l3evset_complex = _calloc(NUM_OFFSETS, sizeof(*l3evset_complex));
    if (!l3evset_complex) {
        fprintf(stderr, ERR "failed to alloc L3 evset complex\n");
        return NULL;
    }
    
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        l3evset_complex[n] = _calloc(n_unc_l2_sets, sizeof(**l3evset_complex));
        if (!l3evset_complex[n]) {
            fprintf(stderr, ERR "failed to alloc L3/LLC sub-complex\n");
            return NULL;
        }
    }
    
    thread_pair *pairs = _calloc(n_pairs, sizeof(thread_pair));
    if (!pairs) {
        fprintf(stderr, ERR "failed to alloc thread pairs\n");
        return NULL;
    }
    
    reset_core_id_counter();
    u64 start_time = time_us();
    
    for (u32 i = 0; i < n_pairs; i++) {
        pairs[i].thread_idx = i;
        pairs[i].result_complex = l3evset_complex;
        pairs[i].l2evsets = l2evsets;
        pairs[i].l3_cands = l3_cands;
        pairs[i].n_uncertain_l2_sets = n_unc_l2_sets;
        pairs[i].evsets_per_l2 = g_config.evsets_per_l2;
        pairs[i].idxs = idxs;

        // pinning
        pairs[i].core_id_main = get_next_core_id(n_cores);
        pairs[i].core_id_helper = get_next_core_id(n_cores);

        pairs[i].helper_ctrl.running = false;
        pairs[i].helper_ctrl.waiting = false;
    }
    
    printf(INFO "Starting %u thread pairs for L3 evset construction\n", n_pairs);
    for (u32 i = 0; i < n_pairs; i++) {
        if (pthread_create(&pairs[i].thread_id, NULL, evset_thread_worker, &pairs[i])) {
            fprintf(stderr, ERR "failed to create thread %u\n", i);
            // cleanup already created threads
            for (u32 j = 0; j < i; j++) {
                pthread_cancel(pairs[j].thread_id);
                pthread_join(pairs[j].thread_id, NULL);
            }
            free(pairs);
            return NULL;
        }
    }

    // wait for all to init
    while(n_pair_init_done != n_pairs);
    printf(INFO "Progress:\n");
    
    u64 total_built = 0;
    for (u32 i = 0; i < n_pairs; i++) {
        pthread_join(pairs[i].thread_id, NULL);
        total_built += pairs[i].total_built;
    }
    
    u64 end_time = time_us();
    
    printf(SUC "Parallel evset construction completed | %.3fms\n", 
          (end_time - start_time) / 1e3);
    u32 tot_possible = n_offset * cache_uncertainty(&l3_info);
    printf(SUC "Total built: %lu/%u L3 evsets (%.2f%%)\n",
           total_built, tot_possible, total_built * 100.0 / (f64)tot_possible);

    free(pairs);

    u64 l3_cnt = (l3_info.n_set_idx_bits - l2_info.n_set_idx_bits) * l3_info.n_slices;
    i32 *final_min_evsize = _calloc(n_offset * n_unc_l2_sets * l3_cnt, sizeof(u32));
    i32 i_fme = 0; // for manipulating final_min_evsize indices
    if (!final_min_evsize) {
        fprintf(stderr, ERR "failed to allocate arr for final min evsize\n");
        return NULL;
    }

    for (u32 c = 0; c < n_offset; c++) {
        u32 n = idxs[c];
        for (u32 i = 0; i < n_unc_l2_sets; i++) {
            if (!l3evset_complex[n][i]) {
                for (u32 f = 0; f < l3_cnt; f++) {
                    final_min_evsize[i_fme] = 0;
                    i_fme++;
                }
                continue;
            }

            for (u32 j = 0; j < l3_cnt; j++) {
                EvSet *l3_evset = l3evset_complex[n][i][j];
                if (!l3_evset || !l3_evset->addrs || 
                    l3_evset->size == 0 || l3_evset->size > l3_info.n_ways) {
                    final_min_evsize[i_fme] = 0;
                    i_fme++;
                    continue;
                }

                final_min_evsize[i_fme] = l3_evset->size; 
                i_fme++;
            }
        }
    }

    printf(INFO "Minimal eviction set size: %u\n", calc_min_cluster(final_min_evsize, n_offset * n_unc_l2_sets * l3_cnt));
    free(final_min_evsize);

#if SANITY_CHECK_ALL_EVS
    {
        printf(INFO "Starting sanity check. Might take a moment.\n");
        u64 l3_cnt_verify = g_l3_cnt;
        if (l3_cnt_verify == 0)
            l3_cnt_verify = 1;
        sanity_check_l3_evcomplex(l3evset_complex, n_offset, n_unc_l2_sets,
                                  l3_cnt_verify, total_built);
    }
#endif

    return l3evset_complex;
}

EvSet**** build_l3_evsets_para_vtop(u32 n_offset)
{
    u64 vtop_start = 0, vtop_total = 0;

    EvSet ***l2evsets = build_l2_evset(l2_info.n_sets);

    n_unc_l2_sets = g_n_uncertain_l2_sets;
    
    if (!l2evsets) {
        fprintf(stderr, ERR "failed to build L2 evset complex\n");
        return NULL;
    }
    
    init_def_l3_conf(&def_l3_build_conf, NULL, NULL);
    
    //EvCands ***l3_cands = build_evcands_all(&def_l3_build_conf, l2evsets);
    EvCands ***l3_cands = build_evcands_all_para(&def_l3_build_conf, l2evsets);
    if (!l3_cands) {
        //already printed within evcands_populate
        //fprintf(stderr, ERR "failed to allocate or filter L3/LLC candidates\n");
        return NULL;
    }

    EvSet ****l3evset_complex = _calloc(NUM_OFFSETS, sizeof(*l3evset_complex));
    if (!l3evset_complex) {
        fprintf(stderr, ERR "failed to allocate L3 evset complex\n");
        return NULL;
    }
    
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        l3evset_complex[n] = _calloc(n_unc_l2_sets, sizeof(**l3evset_complex));
        if (!l3evset_complex[n]) {
            fprintf(stderr, ERR "failed to allocate L3/LLC sub-complex\n");
            return NULL;
        }
    }

    u32 idxs[NUM_OFFSETS] = {0};
    for (u32 i = 0; i < NUM_OFFSETS; i++) {
        idxs[i] = i;
    }
    
    n_offset = _min(n_offset, NUM_OFFSETS);
    if (n_offset == 0) {
        n_offset = NUM_OFFSETS;
    }
    
    vtop_start = time_us();
    cpu_topology_t *curr_topo = get_vcpu_topo();
    vtop_total += time_us() - vtop_start;
    
    if (!curr_topo) {
        fprintf(stderr, ERR "failed to detect initial vCPU topology. Falling back to non-vTop version.\n");
        return build_l3_evsets_para(n_offset);
    }
    
    printf(INFO "vTop: vCPU topology layout:\n");
    print_cpu_topology(curr_topo);
    
    // num of thread pairs to create
    i32 n_cores = n_system_cores();
    u32 n_pairs = g_config.num_threads == 0 ? 
                 (n_cores / 2) : 
                 (g_config.num_threads / 2);
    
    if (n_cores <= 1) {
        fprintf(stderr, ERR "Need at least 2 cores to run parallel eviction set construction\n");
        free(curr_topo);
        return NULL;
    }
    
    // adjust if odd cores
    if (n_cores % 2 != 0 && g_config.num_threads == 0) {
        printf(NOTE "odd number of cores (%d) detected. using %u thread pairs\n", 
               n_cores, n_pairs);
    }
    
    // to test on a very large server one day?
    /*
    if (n_pairs > n_offset) {
        printf(NOTE "Reducing thread pairs from %u to %u to match number of offsets\n", 
               n_pairs, n_offset);
        n_pairs = n_offset;
    }
    */

    vcpu_pair_assignment_t *pair_assignments = _calloc(n_pairs, sizeof(vcpu_pair_assignment_t));
    if (!pair_assignments) {
        fprintf(stderr, ERR "failed to allocate pair assignments\n");
        free(curr_topo);
        return NULL;
    }

    u32 valid_pairs_found = find_optimal_vcpu_pairs(curr_topo, n_pairs, pair_assignments);

    if (valid_pairs_found == 0) {
        fprintf(stderr, ERR "No valid vCPU pairs found. Falling back to non-vTop version.\n");
        free(pair_assignments);
        free(curr_topo);
        return build_l3_evsets_para(n_offset);
    }

    if (valid_pairs_found < n_pairs) {
        printf(WRN "Found only %u valid vCPU pairs; reducing from requested %u pairs.\n", 
              valid_pairs_found, n_pairs);
        n_pairs = valid_pairs_found;
    }
    
    atomic_uint next_offset_idx = ATOMIC_VAR_INIT(0);
    
    vtop_thread_pair_t *pairs = _calloc(n_pairs, sizeof(vtop_thread_pair_t));
    if (!pairs) {
        fprintf(stderr, ERR "failed to allocate thread pairs\n");
        free(pair_assignments);
        free(curr_topo);
        return NULL;
    }
    
    for (u32 i = 0; i < n_pairs; i++) {
        pairs[i].thread_idx = i;
        pairs[i].result_complex = l3evset_complex;
        pairs[i].l2evsets = l2evsets;
        pairs[i].l3_cands = l3_cands;
        pairs[i].idxs = idxs;
        pairs[i].max_offsets = n_offset;
        pairs[i].n_offsets_picked = 0;
        pairs[i].total_built = 0;
        
        pairs[i].main_vcpu = pair_assignments[i].main_vcpu;
        pairs[i].helper_vcpu = pair_assignments[i].helper_vcpu;
        
        pairs[i].global_next_offset = &next_offset_idx;
    }
    
    free(pair_assignments);
    
    printf(INFO "Starting %u pairs for topology-aware parallel L3 evset construction\n", n_pairs);
    for (u32 i = 0; i < n_pairs; i++) {
        if (pthread_create(&pairs[i].thread_id, NULL, vtop_para_worker, &pairs[i])) {
            fprintf(stderr, ERR "failed to create thread %u\n", i);
            for (u32 j = 0; j < i; j++) {
                pthread_cancel(pairs[j].thread_id);
                pthread_join(pairs[j].thread_id, NULL);
            }
            
            free(pairs);
            free(curr_topo);
            return NULL;
        }
    }
    
    u64 last_topo_check = time_us();
    const u64 topo_check_interval = g_config.vtop_freq;
    u64 start_time = time_us();
    bool all_offsets_claimed = false;
    
    while (1) {
        if (!all_offsets_claimed && atomic_load(&next_offset_idx) >= n_offset) {
            all_offsets_claimed = true;
            //printf(INFO "All offsets have been claimed, waiting for pairs to complete.\n");
            break;
        }
        
        // topology changes?
        u64 current_time = time_us();
        if (current_time - last_topo_check >= topo_check_interval) {
            last_topo_check = current_time;
            
            if (verbose) {
                printf(V1 "Checking for topology changes\n");
            }

            vtop_start = time_us();
            cpu_topology_t *new_topo = get_vcpu_topo();
            u64 vtop_end = time_us();
            vtop_total += (vtop_end - vtop_start);
            
            if (!new_topo) {
                if (verbose) {
                    printf(V1 WRN "Failed to get updated topology. Continuing with existing assignments.\n");
                }
                continue;
            }
            
            bool harmful_change = false;
            for (u32 i = 0; i < n_pairs; i++) {
                if (!is_topo_change_harmless(curr_topo, new_topo, 
                                             pairs[i].main_vcpu, 
                                             pairs[i].helper_vcpu)) {
                    harmful_change = true;
                    printf(WRN "Detected harmful vCPU topology change. Repinning.\n");
                    break;
                    // it is true that if the topology has harmed more than one pair,
                    // it will not be shows/detected here; but, in the new pair pinning,
                    // the other damaged ones would also be taken into account for new
                    // vCPU pinning.
                }
            }
            
            if (harmful_change) {
                if (verbose) {
                    print_cpu_topology(new_topo);
                }
                
                // new valid pairs
                vcpu_pair_assignment_t *new_assignments = _calloc(n_pairs, sizeof(vcpu_pair_assignment_t));
                if (!new_assignments) {
                    fprintf(stderr, ERR "failed to allocate new pair assignments\n");
                    free(new_topo);
                    continue;
                }
                
                u32 new_valid_pairs = find_optimal_vcpu_pairs(new_topo, n_pairs, new_assignments);
                
                if (verbose && new_valid_pairs < n_pairs) {
                    printf(V1 WRN "After topology change, can only find %u valid vCPU pairs.\n", 
                          new_valid_pairs);
                    printf(V1 "Some thread pairs will continue with their current assignments.\n");
                }
                
                // repins don't happen by notifying pairs to repin, rather done by main process
                for (u32 i = 0; i < n_pairs; i++) {
                    if (i < new_valid_pairs && new_assignments[i].assigned) {
                        i32 old_main = pairs[i].main_vcpu;
                        i32 old_helper = pairs[i].helper_vcpu;
                        i32 new_main = new_assignments[i].main_vcpu;
                        i32 new_helper = new_assignments[i].helper_vcpu;
                        
                        if (old_main != new_main || old_helper != new_helper) {
                            if (verbose)
                                printf(V1 "Re-pinning thread pair %u: main %d->%d, helper %d->%d\n",
                                    i, old_main, new_main, old_helper, new_helper);
                            
                            // stored vCPU assignments
                            pairs[i].main_vcpu = new_main;
                            pairs[i].helper_vcpu = new_helper;
                            
                            // repin
                            pin_thread_by_pid(pairs[i].thread_id, new_main);
                            pin_helper_by_ctrl(&pairs[i].helper_ctrl, new_helper);
                        }
                    }
                }
                
                free(new_assignments);
            }
            
            free(curr_topo);
            curr_topo = new_topo;
        }
        
        usleep(10000); // to avoid busy waiting a bit
    }
    
    for (u32 i = 0; i < n_pairs; i++) {
        pthread_join(pairs[i].thread_id, NULL);
    }
    
    u64 end_time = time_us();
    u64 total_built = 0;
    
    for (u32 i = 0; i < n_pairs; i++) {
        total_built += pairs[i].total_built;
    }
    
    printf(SUC "Parallel topology-aware construction completed | %.3fs\n",
          (end_time - start_time) / 1e6);
    printf(SUC "Total built: %lu/%lu L3 evsets\n",
           total_built, n_offset * cache_uncertainty(&l3_info));
    printf(INFO "vTop total time: %.3fms\n", vtop_total / 1e3);

#if SANITY_CHECK_ALL_EVS
    {
        printf(INFO "Starting sanity check. Might take a moment.\n");
        u64 l3_cnt = g_l3_cnt;
        u32 l3_cnt_verify = l3_cnt;
        if (l3_cnt_verify == 0)
            l3_cnt_verify = 1;
        sanity_check_l3_evcomplex(l3evset_complex, n_offset, n_unc_l2_sets,
                                     l3_cnt_verify, total_built);
    }
#endif

    u64 l3_cnt = g_l3_cnt;
    i32 *final_min_evsize = _calloc(n_offset * n_unc_l2_sets * l3_cnt, sizeof(u32));
    i32 i_fme = 0;
    
    if (!final_min_evsize) {
        fprintf(stderr, ERR "failed to allocate arr for final min evsize\n");
        free(curr_topo);
        free(pairs);
        return l3evset_complex;
    }

    for (u32 c = 0; c < n_offset; c++) {
        u32 n = idxs[c];
        for (u32 i = 0; i < n_unc_l2_sets; i++) {
            if (!l3evset_complex[n][i]) {
                for (u32 f = 0; f < l3_cnt; f++) {
                    final_min_evsize[i_fme] = 0;
                    i_fme++;
                }
                continue;
            }

            for (u32 j = 0; j < l3_cnt; j++) {
                EvSet *l3_evset = l3evset_complex[n][i][j];
                if (!l3_evset || !l3_evset->addrs || 
                    l3_evset->size == 0 || l3_evset->size > l3_info.n_ways) {
                    final_min_evsize[i_fme] = 0;
                    i_fme++;
                    continue;
                }

                final_min_evsize[i_fme] = l3_evset->size; 
                i_fme++;
            }
        }
    }

    printf(INFO "Minimal eviction set size: %u\n", 
          calc_min_cluster(final_min_evsize, n_offset * n_unc_l2_sets * l3_cnt));
    free(final_min_evsize);
    
    // Cleanup
    free(curr_topo);
    free(pairs);

    return l3evset_complex;
}
