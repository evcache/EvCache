#ifndef EVSET_PARA_H
#define EVSET_PARA_H

#include "evset.h"
#include "common.h"
#include "helper_thread.h"
#include "../vm_tools/vtop.h"
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

extern u32 n_unc_l2_sets;

typedef struct {
    pthread_t thread_id;   // pthread id
    helper_thread_ctrl helper_ctrl;
    u32 thread_idx;
    u32 core_id_main;      // core ID to pin to for main thread
    u32 core_id_helper;

    EvSet**** result_complex;  // result storage

    EvSet*** l2evsets;            // shared
    EvCands*** l3_cands;

    u32 *idxs;                     // offset indices
    u32 n_uncertain_l2_sets;
    u32 evsets_per_l2;
    EvBuildConf l3_conf;
    u64 total_built;
    u64 runtime_limit;             // in mins
    u64 start_time;                // in us
} thread_pair;

typedef struct {
    i32 main_vcpu;
    i32 helper_vcpu;
    bool assigned;
} vcpu_pair_assignment_t;

typedef struct {
    pthread_t thread_id;
    helper_thread_ctrl helper_ctrl;
    u32 thread_idx;
    
    i32 main_vcpu;
    i32 helper_vcpu;
    
    u32 n_offsets_picked;
    u64 total_built;
    
    EvSet **** result_complex;
    EvSet *** l2evsets;
    EvCands *** l3_cands;
    
    u32 *idxs;
    EvBuildConf l3_conf;
    
    volatile atomic_uint *global_next_offset;
    u32 max_offsets;
} vtop_thread_pair_t;

typedef struct {
    u32 offset_idx;
    u32 l2_set_idx;
} gran_work_assignment_t;

// thread pair assignment
typedef struct {
    gran_work_assignment_t *assignments;
    u32 n_assignments;
    u32 pair_idx;
} gran_pair_assignment_t;

typedef struct {
    EvSet ****sets;
    EvSet **all_sets;
    u32 total_sets;
    u32 min_evsize;
} L3EvsetComplex;

void *vtop_para_worker(void *arg);

i32 n_system_cores(void);

void calc_thread_workload(u32 n_pairs, u32 n_offsets, u32 *pair_workload);

void *evset_thread_worker_gran(void *arg);

// sib = set index bit
// [page_offset][l2_color][(n_l3_sib - n_l2_sib)^2 * n_slices]->addrs
EvSet**** build_l3_evsets_para(u32 n_offset);

gran_pair_assignment_t* calc_gran_assignments(u32 n_pairs, u32 n_offsets, u32 n_l2_sets, u32 *total_assignments);

void free_gran_assignments(gran_pair_assignment_t *assignments, u32 n_pairs);

// granular distribution among L2 uncertain sets at each offset
// number of eviction sets per L2 set controlled via g_config.evsets_per_l2
EvSet**** build_l3_evsets_para_gran(u32 n_l2_sets, u32 n_offsets,
                                    EvSet ***pre_l2evsets);

u32 find_optimal_vcpu_pairs(cpu_topology_t *topo, u32 n_requested_pairs, vcpu_pair_assignment_t *pair_assignments);

// main worker function for topology-aware parallel threads
void *vtop_para_thread_worker(void *arg);

//  parallel, topology-aware approach with dynamic workload assignment
EvSet **** build_l3_evsets_para_vtop(u32 n_offset);

void* evset_thread_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif // EVSET_PARA_H
