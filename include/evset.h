#ifndef EVSET_H
#define EVSET_H

#include "cache_info.h"
#include "common.h"
#include "helper_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STRAW_ZHAO = 0
} EvAlgoType;

typedef struct {
    void *buf;
    u64 n_pages,
        ref_cnt;
} EvBuffer;

typedef struct {
    u32 cand_scale; // scaling factor for candidate set size (-c arg)

    EvBuffer *evb;

    u8 **addrs;
    u64 count,
        ref_cnt;
    CacheInfo* cache;
} EvCands;
 
typedef struct _ev_build_conf {
    /* CANDS CONF */
    struct _evset *filter_ev;
    u32 cand_scale;
    /* CANDS CONF */

    /* TESTING CONF */
    i64 lat_thresh;            // latency threshold for miss
    u32 trials;                // number of test iterations 
    u32 low_bnd;               // otc lower bound
    u32 upp_bnd;               // otc upper bound
    u32 test_scale;            // scaling factor for bounds
    u32 ev_repeat;
    u32 access_cnt;
    u32 n_retries; // for L2 checking evset->size == n_ways
    u32 max_whole_ret;
    u32 block;
    u32 stride;
    
    struct _evset *lower_ev;   // for L3
    
    helper_thread_ctrl *hctrl;
    bool need_helper;
    bool flush_cands;
    bool foreign_evictor;
    
    // func ptrs
    void (*cand_traverse)(u8 **cands, u64 cnt, struct _ev_build_conf *c);

    EvRes (*test)(u8 *target, u8 **cands, u64 cnt, struct _ev_build_conf *tconf);
    /* TESTING CONF */

    /* PRUNING ALGO CONF */
    EvAlgoType algo;   // pruning algorithms
    u32 cap_scaling;   // capacity scaling factor
    u32 verify_retry;
    u32 retry_timeout; // timeout in ms
    u32 max_backtrack;
    u32 slack;
    u32 extra_cong;    // extra congruent lines
    bool ret_partial;
    bool prelim_test;  // preliminary test before building
    /* PRUNING ALGO CONF */
} EvBuildConf;

typedef struct _evset {
    /* EVSET CONF */
    u8 **addrs;      // evset addresses
    u32 size; // num of addresses in evset
    u32 ev_cap; // cap_scaling * n_ways
    u8* target_addr;
    /* EVSET CONF */

    /* CANDIDATE CONF */
    EvCands* cands;
    struct _evset *filter_ev;  // eviction set used for filtering candidates
    /* CANDIDATE CONF */

    EvBuildConf *build_conf;
    CacheInfo *target_cache; // e.g. l2_info, l3_info
} EvSet;

typedef struct {
    pthread_t main_thread;
    helper_thread_ctrl helper_ctrl;
    
    i32 main_vcpu;                  // vCPU ID for main thread
    i32 helper_vcpu;                // vCPU ID for helper thread
    bool running;                   // if threads are running
    
    EvSet ****result_complex;
    EvSet ***l2evsets;
    EvCands ***l3_cands;
    u32 *idxs;                     // offset indices
    u32 n_uncertain_l2_sets; // will replace later with global var
    EvBuildConf l3_conf;
    
    volatile u32 current_offset_idx;
    u32 max_offset_idx;
    
    u64 *offset_success;           // success count per offset
    u64 total_built;               // counter for successful builds
    u64 last_topo_check;           // timestamp of last topology check
} vtop_thread_pair;

typedef struct eviction_chain {
    struct eviction_chain *next;
    struct eviction_chain *prev;
} evchain;

/*
  to be iteratively called by build_l2_evset to construct one evset
  for all uncertain sets in L2. further shifts for each offset to get all evsets
*/
EvSet* build_single_l2_evset(EvCands* cands, u8* target, u32 set_idx, 
                             u8** all_addrs, u64 n_all_addrs);

// [shifted_offset][uncertain_l2_set]
EvSet*** build_l2_evset(u32 num_sets);

EvSet** build_single_l3_evset(void);

void *vtop_main_thread_worker(void *arg);

EvCands ***build_evcands_all(EvBuildConf *conf, EvSet ***l2evsets);

EvCands ***build_evcands_all_para(EvBuildConf *conf, EvSet ***l2evsets);

/*
 * modified: https://github.com/zzrcxb/LLCFeasible/
 * Supports early termination via 
 * @p max_evsets specifying how many eviction sets to build at most
 */
EvSet **build_evsets_at(u32 offset, EvBuildConf *conf, CacheInfo *cache,
                        EvCands *_cands, u64 *ev_cnt, CacheInfo *lower_c,
                        EvBuildConf *lower_conf, EvSet **lower_evsets,
                        u64 n_lower_evsets, u32 max_evsets);

void calc_evsets_per_offset(u32 n_sets, u32 n_pairs);

void init_def_l2_conf(EvBuildConf* config);

void init_def_l3_conf(EvBuildConf* conf, EvSet* l2ev, helper_thread_ctrl *hctrl);

EvRes test_eviction(u8 *target, u8 **cands, u64 cnt, EvBuildConf *tconf);

bool build_evset_zhao(u8 *target, EvSet *evset);

void addrs_traverse(u8 **cands, u64 cnt, EvBuildConf *tconf);

EvRes verify_evset(EvSet* evset, u8* target);

void evcands_filter_batch(u8** addrs, u64 total_cands, u64* filtered_count,
                          EvSet* filter_ev, EvBuildConf* conf);

u64 prune_evcands(u8 *target, u8 **cands, u64 cnt, EvBuildConf *tconf);

EvSet* evset_shift(EvSet* from, u32 offset);

EvCands* evcands_shift(EvCands* from, u32 offset);

EvBuffer *evbuffer_new(CacheInfo *cache, EvBuildConf *cand_conf);

EvCands *evcands_new(CacheInfo *cache, EvBuildConf *cands_config, EvBuffer *evb);

EvCands *evcands_new(CacheInfo *cache, EvBuildConf *cands_config, EvBuffer *evb);

// bool evcands_populate(u32 offset, EvCands *cands, EvBuildConf *config);
bool evcands_populate(u32 offset, EvCands *cands, EvBuildConf *config,
                      i32 thread_id, u32 filter_offset);

void evcands_free(EvCands *cands);

void evbuffer_free(EvBuffer *evb);

void free_evset_complex(EvSet ****complex, u32 num_offsets,
                        u32 num_l2_sets, u32 evsets_per_l2);

#ifdef __cplusplus
}
#endif
#endif // EVSET_H
