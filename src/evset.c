#include "../include/evset.h"
#include "../include/cache_info.h"
#include "../include/cache_ops.h"
#include "../include/lats.h"
#include "../include/helper_thread.h"
#include "../include/utils.h"
#include "../include/bitwise.h"
#include "../include/evset_para.h"
#include "../include/config.h"
#include "../vm_tools/gpa_hpa.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>

#define FILTER_BATCH 1

// for cand filtering
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

EvBuildConf def_l2_build_conf;
EvBuildConf def_l3_build_conf;
static helper_thread_ctrl hctrl;
u32 n_unc_l2_sets;
u32 g_evsets_per_offset[NUM_OFFSETS] = {0};

static void print_cand_accuracy(EvCands ***cands_complex, EvSet ***l2evsets)
{
    if (debug <= 0)
        return;

    printf(D1 "Filtered candidate accuracy per uncertain L2 set:\n");
    for (u32 i = 0; i < n_unc_l2_sets; i++) {
        EvCands *cands = cands_complex[0][i];
        EvSet *filter_ev = l2evsets[0][i];
        if (!cands || !filter_ev) {
            printf("  [%u] N/A\n", i);
            continue;
        }

        u64 n_filtered = cands->count;
        u64 t_hpa = va_to_hpa(filter_ev->target_addr);
        u64 t_hpa_l2 = cache_get_sib(t_hpa, &l2_info);
        u64 n_healthy = 0; // healthy if filter cand's L2 SIB matches filterev's target L2 SIB
        for (u32 k = 0; k < n_filtered; k++) {
            u64 cand_hpa = va_to_hpa(cands->addrs[k]);
            u64 cand_l2 = cache_get_sib(cand_hpa, &l2_info);
            if (cand_l2 == t_hpa_l2)
                n_healthy++;
        }

        printf("  [%2u] %u/%lu filtered candidate lines are healthy (%.2f%%)\n",
               i, (u32)n_healthy, n_filtered,
               (f32)n_healthy / (f32)n_filtered * 100.0f);
    }
}

void addrs_traverse(u8 **addrs, u64 cnt, EvBuildConf *tconf)
{
    for (u64 i = 0; i < tconf->ev_repeat ; i++) {
        access_array_bwd(addrs, cnt);
    }
}

static ALWAYS_INLINE
void helper_thread_traverse_cands(u8 **cands, u64 cnt, EvBuildConf *tconf)
{
    struct helper_thread_traverse_cands *tcands = malloc(sizeof(*tcands));
    assert(tcands);
    
    *tcands = (struct helper_thread_traverse_cands)
    {
        .traverse = tconf->cand_traverse,
        .cands = cands,
        .cnt = cnt,
        .tconfig = tconf
    };
    
    tconf->hctrl->action = TRAVERSE_CANDS;
    tconf->hctrl->payload = tcands;
    compiler_barrier();
    tconf->hctrl->waiting = false;
    wait_helper_thread(tconf->hctrl);
    free(tcands);
}

EvBuffer *evbuffer_new(CacheInfo *cache, EvBuildConf *cand_conf)
{
    u64 uncertainty = cache_uncertainty(cache);
    u64 n_pages, buf_size;

    n_pages = uncertainty * cache->n_ways * cand_conf->cand_scale;
    buf_size = n_pages * PAGE_SIZE;

    EvBuffer *evb = _calloc(1, sizeof(*evb));
    if (!evb) {
        fprintf(stderr, ERR "Failed to allocate EvBuffer\n");
        return NULL;
    }

    void *pages = NULL;
    //pages = mmap_shared_init(NULL, buf_size, 0);
    pages = mmap_shared_init_para(NULL, buf_size, 0);

    if (!pages) {
        fprintf(stderr, ERR "Failed to mmap %lu bytes for eviction buffer\n", buf_size);
        goto err;
    }
    assert(_ALIGNED(pages, PAGE_SHIFT));

    evb->buf = pages;
    evb->n_pages = n_pages;
    evb->ref_cnt = 0;
    return evb;

err:
    free(evb);
    return NULL;
}

EvCands *evcands_new(CacheInfo *cache, EvBuildConf *cands_config, EvBuffer *evb)
{
    EvCands *cands = _calloc(1, sizeof(*cands));
    if (!cands) {
        fprintf(stderr, ERR "Failed to allocate EvCands");
        return NULL;
    }

    cands->evb = evb;
    if (!cands->evb) {
        cands->evb = evbuffer_new(cache, cands_config);
        if (!cands->evb) {
            free(cands);
            return NULL;
        }
    }

    cands->evb->ref_cnt += 1;
    cands->ref_cnt = 0;
    cands->cache = cache;
    return cands;
}

bool evcands_populate(u32 offset, EvCands *cands, EvBuildConf *config,
                      i32 thread_id, u32 filter_offset)
{
    u64 n_cands_init = cands->evb->n_pages;
    u64 stride = PAGE_SIZE;

    u8 **addrs = _calloc(n_cands_init, sizeof(*addrs));
    if (!addrs) {
        fprintf(stderr, ERR "Failed to allocate the candidate array | n_pages: %lu\n",
               n_cands_init);
        goto err;
    }

    for (u64 n = 0; n < n_cands_init; n++) {
        addrs[n] = cands->evb->buf + n * stride + offset;
        *addrs[n] = n;
    }

    if (!config->filter_ev || cache_uncertainty(config->filter_ev->target_cache) == 1) {
        printf(WRN "no filter ev, or not worth filtering\n");
        cands->addrs = addrs;
        cands->count = n_cands_init;
        return false;
    }

    u64 start = time_us();
    u64 n_cands_filtered = 0;
#if FILTER_BATCH // works best on e.g. SKX, Cascade
    evcands_filter_batch(addrs, cands->evb->n_pages, &n_cands_filtered,
                         config->filter_ev, &def_l2_build_conf);
#else // sequential filtering. some uarchs like icelake and larger LLCs on Intel Xeon platinum aren't reliable with batch filtering
    for (u32 i = 0; i < cands->evb->n_pages; i++) {
        if (test_eviction(addrs[i], config->filter_ev->addrs, config->filter_ev->size, &def_l2_build_conf) == OK) {
            _swap(addrs[n_cands_filtered], addrs[i]);
                n_cands_filtered += 1;
        }
    }
#endif

    if (n_cands_filtered <= 0) {
        fprintf(stderr, ERR "Failed to filter out candidates!\n");
        goto err;
    }
    u64 end = time_us();

    u32 w = n_digits(n_cands_init); // width

    pthread_mutex_lock(&print_mutex);
    printf("├─ Thread %3u: Filtered %lu candidate lines to %*ld | %.3fms",
           thread_id, n_cands_init, w, n_cands_filtered, (end - start) / 1e3);

    if (verbose)
        printf(" (V1: page offset: 0x%x)", offset);

    printf("\n");
    pthread_mutex_unlock(&print_mutex);

    u8 **tmp = realloc(addrs, n_cands_filtered * sizeof(*addrs));
    if (!tmp) {
        fprintf(stderr, ERR "realloc failed: candidate array\n");
        exit(EXIT_FAILURE);
    }

    cands->addrs = tmp;
    cands->count = n_cands_filtered;
    return false;

err:
    free(addrs);
    return true;
}

EvCands *evcands_shift(EvCands *from, u32 offset)
{
    EvCands *cands = _calloc(1, sizeof(*cands));
    if (!cands) {
        fprintf(stderr, ERR "failed to alloc EvCands");
        return NULL;
    }

    cands->addrs = _calloc(from->count, sizeof(*from->addrs));
    cands->evb = from->evb;
    cands->evb->ref_cnt += 1;
    cands->count = from->count;
    cands->ref_cnt = 0;
    if (!cands->addrs) {
        fprintf(stderr, ERR "failed to alloc the cands array\n");
        goto err;
    }

    for (u32 i = 0; i < cands->count; i++) {
        cands->addrs[i] = _ALIGN_DOWN(from->addrs[i], PAGE_SHIFT) + offset;
    }

    return cands;

err:
    free(cands);
    return NULL;
}

EvCands ***build_evcands_all(EvBuildConf *conf, EvSet ***l2evsets)
{
    u64 start, end;
    // L2 is already inited prior when building l2evsets

    start = time_us();

    EvCands *base_cands = evcands_new(&l3_info, conf, NULL);
    if (!base_cands) {
        fprintf(stderr, ERR "Failed to allocate EvBuffer\n");
        return NULL;
    }
    end = time_us();

    if (verbose) 
        printf(V1 "Allocated EvCands complex | %.3fms\n",
              (end - start) / 1e3);

    n_unc_l2_sets = g_n_uncertain_l2_sets;

    start = time_us();

    EvCands ***cands_complex = _calloc(NUM_OFFSETS, sizeof(*cands_complex));
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        u32 offset = n * CL_SIZE;
        cands_complex[n] = _calloc(n_unc_l2_sets, sizeof(EvCands*));
        for (u32 i = 0; i < n_unc_l2_sets; i++) {
            if (n == 0) {
                conf->filter_ev = l2evsets[n][i];
                conf->filter_ev->build_conf = &def_l2_build_conf;
                EvCands *cands = evcands_new(&l3_info, conf, base_cands->evb);
                if (!cands) {
                    return NULL;
                }

                if (evcands_populate(offset, cands, conf,
                                     -1, offset)) {
                    //inside, the err would be reported.
                    return NULL;
                }
                cands_complex[n][i] = cands;
            } else {
                cands_complex[n][i] = evcands_shift(cands_complex[0][i], offset);
                if (debug > 2) {
                    EvCands* cc = cands_complex[n][i];
                    u16 n_display = _min(2, cc->count);
                    sep();
                    printf(D3 "There is %lu candidates; displaying %u\n", cc->count, n_display);
                    for (u16 c = 0; c < n_display; c++) {
                        printf("cands_complex[%u][%u]->addrs[%u]: HPA 0x%lx\n", 
                                n, i, c, va_to_hpa(cc->addrs[c]));
                    }
                }
            }
        }
    }
    end = time_us();
    printf(INFO "Built EvCands complex | %.3fms\n", (end - start) / 1e3);

    print_cand_accuracy(cands_complex, l2evsets);
    return cands_complex;
}

typedef struct {
    u32 start_idx;
    u32 end_idx;
    u32 thread_idx;
    EvCands *base_cands;
    EvCands ***cands_complex;
    EvSet ***l2evsets;
    EvBuildConf *conf;
} cand_para_arg_t;

static bool build_single_cand(u32 idx, u32 thread_idx, EvCands *base_cands,
                              EvCands ***cands_complex, EvSet ***l2evsets,
                              EvBuildConf *conf)
{
    u32 offset = idx * CL_SIZE;
    EvBuildConf c = *conf;
    c.filter_ev = l2evsets[idx][idx];
    c.filter_ev->build_conf = &def_l2_build_conf;

    EvCands *cand = evcands_new(&l3_info, &c, base_cands->evb);
    if (!cand)
        return false;

    if (evcands_populate(offset, cand, &c, thread_idx, offset)) {
        free(cand->addrs);
        free(cand);
        return false;
    }

    cands_complex[idx][idx] = cand;
    return true;
}

static void *cand_para_worker(void *arg)
{
    cand_para_arg_t *ctx = (cand_para_arg_t *)arg;
    pin_thread_by_pid(pthread_self(), ctx->thread_idx);
    for (u32 idx = ctx->start_idx; idx < ctx->end_idx; idx++) {
        build_single_cand(idx, ctx->thread_idx, ctx->base_cands,
                          ctx->cands_complex, ctx->l2evsets, ctx->conf);
    }

    return NULL;
}

EvCands ***build_evcands_all_para(EvBuildConf *conf, EvSet ***l2evsets)
{
    u64 start, end;

    start = time_us();
    EvCands *base_cands = evcands_new(&l3_info, conf, NULL);
    if (!base_cands) {
        fprintf(stderr, ERR "Failed to allocate EvBuffer\n");
        return NULL;
    }
    end = time_us();

    if (verbose)
        printf(V1 "Allocated EvCands complex | %.3fms\n",
              (end - start) / 1e3);

    n_unc_l2_sets = g_n_uncertain_l2_sets;

    start = time_us();

    EvCands ***cands_complex = _calloc(NUM_OFFSETS, sizeof(*cands_complex));
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        cands_complex[n] = _calloc(n_unc_l2_sets, sizeof(EvCands*));
    }


    u32 n_threads = g_config.num_threads ? g_config.num_threads : n_system_cores();
    if (n_threads > n_unc_l2_sets)
        n_threads = n_unc_l2_sets;

    printf(INFO "Using %u cores to construct EvCands complex in parallel\n", n_threads);

    pthread_t tids[n_threads];
    cand_para_arg_t targs[n_threads];

    u32 base_load = n_unc_l2_sets / n_threads;
    u32 remainder = n_unc_l2_sets % n_threads;
    u32 curr_idx = 0;

    for (u32 t = 0; t < n_threads; t++) {
        u32 cnt = base_load + (t < remainder ? 1 : 0);
        targs[t].start_idx = curr_idx;
        targs[t].end_idx = curr_idx + cnt;
        targs[t].thread_idx = t;
        targs[t].base_cands = base_cands;
        targs[t].cands_complex = cands_complex;
        targs[t].l2evsets = l2evsets;
        targs[t].conf = conf;
        curr_idx += cnt;

        if (pthread_create(&tids[t], NULL, cand_para_worker, &targs[t])) {
            fprintf(stderr, ERR "failed to create candidate thread %u\n", t);
            n_threads = t;
            break;
        }
    }

    for (u32 t = 0; t < n_threads; t++) {
        pthread_join(tids[t], NULL);
    }

    bool complete = true;
    for (u32 i = 0; i < n_unc_l2_sets; i++) {
        if (!cands_complex[i][i]) {
            complete = false;
            break;
        }
    }

    if (!complete) {
        fprintf(stderr, ERR "Failed to populate evcands\n");
        return NULL;
    }

    for (u32 i = 0; i < n_unc_l2_sets; i++) {
        if (!cands_complex[i][i])
            continue;
        for (u32 n = 0; n < NUM_OFFSETS; n++) {
            if (n == i)
                continue;
            u32 offset = n * CL_SIZE;
            cands_complex[n][i] = evcands_shift(cands_complex[i][i], offset);
            if (debug > 2 && cands_complex[n][i]) {
                EvCands *cc = cands_complex[n][i];
                u16 n_display = _min(2, cc->count);
                sep();
                printf(D3 "There is %lu candidates; displaying %u\n", cc->count, n_display);
                for (u16 c = 0; c < n_display; c++) {
                    printf("cands_complex[%u][%u]->addrs[%u]: HPA 0x%lx\n",
                           n, i, c, va_to_hpa(cc->addrs[c]));
                }
            }
        }
    }

    end = time_us();
    printf(INFO "Built EvCands Complex in Parallel | %.3fms\n", (end - start) / 1e3);

    print_cand_accuracy(cands_complex, l2evsets);
    return cands_complex;
}

EvRes test_eviction(u8 *target, u8 **cands, u64 cnt, EvBuildConf *tconf)
{
    //u8 *tlb_target = (u8 *)((((u64)target) & ~(PAGE_SIZE - 1)) + (PAGE_SIZE / 2));
    u32 otc = 0; // over-threshold count
    u32 trials = tconf->trials;
    u32 low_bnd = tconf->low_bnd;
    u32 upp_bnd = tconf->upp_bnd;
    
    if (tconf->test_scale > 1) {
        trials *= tconf->test_scale;
        low_bnd *= tconf->test_scale;
        upp_bnd *= tconf->test_scale;
    }

    otc = 0;
    for (u32 i = 0; i < trials; i++) {
        _clflushopt(target); // insertion age

        if (tconf->flush_cands) {
            flush_array(cands, cnt);
        }

        _lfence();
        _mfence();
        
        // load target line
        for (u32 j = 0; j < tconf->access_cnt; j++) {
            if (tconf->lower_ev) { // for L3/LLC
                addrs_traverse(tconf->lower_ev->addrs, tconf->lower_ev->size, tconf);
            }

            _lfence();
            maccess(target);
            _lfence();

            if (tconf->need_helper) {
                helper_thread_read_single(target, tconf->hctrl);
                maccess(target);
            }
        }

        _lfence();

        if (tconf->foreign_evictor) {
            helper_thread_traverse_cands(cands, cnt, tconf);
        } else {
            tconf->cand_traverse(cands, cnt, tconf);
        }

        _lfence();

        u64 lat = _time_maccess(target);
        //printf(V1 "Trial [%u] - Lat: %lu\n", i, lat);
        
        if (lat >= tconf->lat_thresh && lat <= g_lats.interrupt_thresh)
            otc++;

        if (otc >= upp_bnd)
            return OK;
    }

    // if we get here we did not surpass otc >= upp_bnd
    return FAIL;
}

EvRes verify_evset(EvSet* evset, u8* target)
{
    if (!evset || !target || !evset->build_conf) {
        fprintf(stderr, ERR "uninitialized address in verify_evset.\n");
        return FAIL;
    }
 
    return test_eviction(target, evset->addrs, evset->size, evset->build_conf);
}

// modified: https://github.com/zzrcxb/LLCFeasible/
void evcands_filter_batch(u8** addrs, u64 total_cands, u64* filtered_count, 
                          EvSet* filter_ev, EvBuildConf* conf)
{
    if (!filter_ev) {
        //*filtered_count = total_cands;
        fprintf(stderr, ERR "No filter ev in evcands_filter_batch");
        return;
    }
    
    u64 batch_sz = filter_ev->target_cache->n_ways;
    if (batch_sz > 2) batch_sz -= 1;
    
    u32* otcs = _calloc(batch_sz, sizeof(u32));
    if (!otcs) {
        fprintf(stderr, ERR "Failed to allocate OTC buffer\n");
        *filtered_count = 0;
        return;
    }
    
    u64 n_pos = 0;
    
    for (u64 s = 0; s < total_cands; s += batch_sz) {
        u64 cur_batch_sz = _min(batch_sz, total_cands - s);
        memset(otcs, 0, sizeof(*otcs) * cur_batch_sz);
        
        for (u32 t = 0; t < conf->trials; t++) {
            access_array(&addrs[0], cur_batch_sz);
            _lfence();
            addrs_traverse(filter_ev->addrs, filter_ev->size, filter_ev->build_conf);
            _lfence();
            for (u32 i = 0; i < cur_batch_sz; i++) {
                u64 lat = _time_maccess(addrs[s + i]);
                otcs[i] += lat > conf->lat_thresh;
            }
        }
        
        for (u32 i = 0; i < cur_batch_sz; i++) {
            if (otcs[i] > conf->upp_bnd) {
                _swap(addrs[n_pos], addrs[s + i]);
                n_pos += 1;
            }
        }
    }
    
    *filtered_count = n_pos;
    free(otcs);
}

u64 prune_evcands(u8 *target, u8 **cands, u64 cnt, EvBuildConf *tconf)
{
    u64 start_ns = time_us();
    for (u64 i = 0; i < cnt;) {
        _swap(target, cands[i]);
        EvRes tres = tconf->test(target, cands, cnt, tconf);
        _swap(target, cands[i]);
        if (tres == FAIL) {
            cnt -= 1;
            _swap(cands[i], cands[cnt]);
        } else {
            i += 1;
        }
    }
    u64 end_ns = time_us();
    
    if (verbose > 1)
        printf(V2 "Pruning took %luus, reduced size from %lu to %lu\n", 
               end_ns - start_ns, cnt + 1, cnt);
    
    return cnt;
}

EvSet* evset_shift(EvSet* from, u32 offset)
{
    if (!from) return NULL;
    
    EvSet* evset = _calloc(1, sizeof(EvSet));
    if (!evset) {
        fprintf(stderr, ERR "Cannot allocate shifted eviction set\n");
        return NULL;
    }
    
    // same metadata
    evset->ev_cap = from->ev_cap;
    evset->size = from->size;
    evset->target_cache = from->target_cache;
    evset->build_conf = from->build_conf;
    evset->cands = from->cands;
    evset->target_addr =
    ((u8 *)((u64)from->target_addr & ~(PAGE_SIZE - 1))) + offset;
    
    evset->addrs = _calloc(evset->ev_cap, sizeof(u8*));
    if (!evset->addrs) {
        fprintf(stderr, ERR "Cannot allocate evset addresses buffer\n");
        free(evset);
        return NULL;
    }
    
    for (u32 i = 0; i < from->size; i++) {
        u8* page_base = (u8*)((u64)from->addrs[i] & ~(PAGE_SIZE - 1));
        evset->addrs[i] = page_base + offset;
    }
    
    return evset;
}

void calc_evsets_per_offset(u32 n_sets, u32 n_pairs)
{
    memset(g_evsets_per_offset, 0, sizeof(g_evsets_per_offset));
    
    u32 evsets_per_offset_max = g_n_uncertain_l2_sets * g_l3_cnt;
    
    if (verbose) {
        printf(V1 "calculating evset distribution for %u requested sets using %u pairs\n", 
               n_sets, n_pairs);
    }
    
    u32 evsets_per_pair = n_sets / n_pairs;
    u32 remainder = n_sets % n_pairs;
    
    if (verbose) {
        printf(V1 "target distribution: %u evsets per pair; %u pairs get +1 extra\n", 
               evsets_per_pair, remainder);
    }
    
    if (evsets_per_pair > evsets_per_offset_max) {
        if (verbose) {
            printf(V1 "evsets per pair (%u) exceeds max per offset (%u), falling back to offset-filling\n",
                   evsets_per_pair, evsets_per_offset_max);
        }
        
        // when pairs need more than one offset each
        u32 remaining_sets = n_sets;
        u32 offset_idx = 0;
        
        while (remaining_sets > 0 && offset_idx < NUM_OFFSETS) {
            if (remaining_sets >= evsets_per_offset_max) {
                g_evsets_per_offset[offset_idx] = evsets_per_offset_max;
                remaining_sets -= evsets_per_offset_max;
            } else {
                g_evsets_per_offset[offset_idx] = remaining_sets;
                remaining_sets = 0;
            }
            offset_idx++;
        }
    } else {
        // optimal case: distribute evenly across pairs
        for (u32 i = 0; i < n_pairs && i < NUM_OFFSETS; i++) {
            g_evsets_per_offset[i] = evsets_per_pair;
            if (i < remainder) {
                g_evsets_per_offset[i]++; // distribute remainder
            }
            
            if (verbose > 1) {
                printf(V2 "offset %u: %u evsets assigned (for pair %u)\n", 
                       i, g_evsets_per_offset[i], i);
            }
        }
    }
    
    // total assigned and offsets with work
    u32 total_assigned = 0;
    u32 offsets_with_work = 0;
    for (u32 i = 0; i < NUM_OFFSETS; i++) {
        if (g_evsets_per_offset[i] > 0) {
            total_assigned += g_evsets_per_offset[i];
            offsets_with_work++;
        }
    }
    
    if (total_assigned != n_sets) {
        printf(ERR "mismatch in evset distribution: assigned %u, requested %u\n", 
                total_assigned, n_sets);
    }

    if (verbose) {
        printf(V1 "evset distribution calculated across %u offsets\n", offsets_with_work);
        printf(V1 "total evsets assigned: %u/%u\n", total_assigned, n_sets);
    }
}

// default L2 config
void init_def_l2_conf(EvBuildConf* conf)
{
    if (!conf) {
        fprintf(stderr, ERR "No configuration foudn in init_def_l2_conf!\n");
        return;
    }
    
    /* CAND CONF */
    conf->cand_scale = (g_config.cand_scaling == 0) ? // user didnt change (pass in -c)?
                        3 // default value for L2 if not changed
                        : g_config.cand_scaling;
    g_config.cand_scaling = conf->cand_scale;
    conf->filter_ev = NULL;
    /* CAND CONF */
    
    /* TESTING CONF */
    conf->lat_thresh = g_lats.l2_thresh;
    conf->trials = 9;
    conf->low_bnd = 3;
    conf->upp_bnd = 6;
    conf->test_scale = 1;
    conf->ev_repeat = 4;
    conf->access_cnt = 3;
    conf->lower_ev = NULL; // for L3/LLC conf
    conf->need_helper = false;
    conf->flush_cands = false;
    conf->foreign_evictor = false;
    conf->cand_traverse = addrs_traverse;
    conf->test = test_eviction;
    conf->n_retries = 10;
    conf->max_whole_ret = 10;
    /* TESTING CONF */
    
    /* ALGO CONF */
    conf->algo = STRAW_ZHAO;
    conf->cap_scaling = 2;
    conf->verify_retry = 5;
    conf->retry_timeout = 20;
    conf->max_backtrack = 20;
    conf->slack = 0;
    conf->extra_cong = 0;
    conf->ret_partial = false;
    conf->prelim_test = false;
    /* ALGO CONF */
}

// default L3/LLC config
void init_def_l3_conf(EvBuildConf* conf, EvSet* l2ev, helper_thread_ctrl *hctrl)
{
    if (!conf) {
        fprintf(stderr, ERR "No configuration found in init_def_l3_conf!\n");
        return;
    }    
    /* CANDIDATE CONF */
    conf->cand_scale = (g_config.cand_scaling == 0) ? // user didn't change?
                       3 // def value for L3/LLC
                       : g_config.cand_scaling;
    conf->filter_ev = l2ev;
    /* CANDIDATE CONF */
    
    // TEST CONF (tconf)
    conf->lat_thresh = g_lats.l3_thresh;
    conf->trials = 4;
    conf->low_bnd = 2;
    conf->upp_bnd = 2;
    conf->test_scale = 1;
    conf->ev_repeat = 1;
    conf->access_cnt = 1;
    conf->block = 24,
    conf->stride = 12,
    conf->lower_ev = l2ev;
    conf->need_helper = true;
    conf->flush_cands = false;
    conf->foreign_evictor = false;
    conf->cand_traverse = traverse_cands_mt;
    conf->test = test_eviction;
    conf->max_whole_ret = 5;
    /* TEST CONF */
    
    /* ALGORITHM CONF */
    conf->algo = STRAW_ZHAO;
    conf->cap_scaling = 2;
    conf->verify_retry = 10;
    conf->retry_timeout = 1000;
    conf->max_backtrack = 20;
    conf->slack = 2;
    conf->extra_cong = 0;
    conf->ret_partial = false;
    conf->prelim_test = false;
    /* ALGORITHM CONF */

    conf->hctrl = hctrl;
}

/*
  Binary Search evset construction by Neil Zhao
  Paper: Last Level Cache Side Channel Attacks are Feasible in the
  Modern Public Cloud
*/
bool build_evset_zhao(u8 *target, EvSet *evset)
{
    u8 **cands = evset->cands->addrs;
    EvBuildConf *build_conf = evset->build_conf;
    u64 n_cands = evset->cands->count,
        evsz = 0;
    
    if (n_cands <= 1) {
        return true; // has err
    }

    CacheInfo *target_cache = evset->target_cache;
    u64 n_ways = target_cache->n_ways;
    u64 max_bctr = build_conf->max_backtrack;
    u64 num_carried_cong = n_ways - build_conf->slack;
    i64 lower = 0, upper = n_cands, cnt, n_bctr = 0, iters = 0;
    bool is_reset = false;
    u64 migrated = n_cands - 1;
    u32 exp_evsz = n_ways + build_conf->extra_cong;
    if (verbose > 2) {
        printf(V1 "exp_evsz: %u, target_cache: %p, n_ways: %lu, max_bctr: %lu, num_carried_cong: %lu, \n\
            lower: %ld, upper: %ld, n_bctr: %ld, iters: %ld, is_reset: %d, migrated: %lu\n",
            exp_evsz, (void*)target_cache, n_ways, max_bctr, num_carried_cong, 
            lower, upper, n_bctr, iters, is_reset, migrated);
    }
    
    // until we have enough addrs or reach max_bctr
    while (evsz < evset->ev_cap && n_bctr < max_bctr) {
        u32 offset = 0;
        if (build_conf->slack && evsz > num_carried_cong) {
            offset = evsz - num_carried_cong;
        }

        if (evsz > 0 && !is_reset && evsz < n_ways) {
            u32 rem = n_ways - evsz; // rem > 0
            cnt = (upper * rem + lower) / (rem + 1);
            if (cnt == upper) {
                cnt -= 1;
            }
        } else {
            cnt = (upper + lower) / 2;
        }

        is_reset = false;
        u8 **cands_o = cands + offset;
        bool has_pos = false;
        
        while (upper - lower > 1) {
            if (verbose > 2)
                printf(V3 "BS: lower=%ld upper=%ld cnt=%ld\n", lower, upper, cnt);
            EvRes res = build_conf->test(target, cands_o, cnt - offset, build_conf);
            
            if (res == OK) {
                upper = cnt;
                has_pos = true;
            } else {
                lower = cnt;
                //has_neg = true;
            }
            cnt = (upper + lower) / 2;
        }
        
        if (!has_pos && 
            build_conf->test(target, cands_o, upper - offset, build_conf) == FAIL) {
            n_bctr += 1;
            is_reset = true;
        } else {
            // found a cong line, add to evset
            _swap(cands[evsz], cands[upper - 1]);
            evsz += 1;
            if (verbose > 2)
                printf(V3 "Added congruent VA %p, size now: %lu\n", 
                       cands[evsz-1], evsz);
        }

        if (evsz >= exp_evsz && build_conf->test(target, cands, evsz, build_conf) == OK) {
            evsz = prune_evcands(target, cands, evsz, build_conf);
            if (evsz >= exp_evsz) {
                break; // only if we still have enough after pruning
            }
            n_bctr += 1; // otherwise count as backtrack
        }

        lower = evsz;
        
        if (is_reset || (build_conf->slack && evsz > num_carried_cong)) {
            if (upper >= migrated) {
                migrated = n_cands - 1;
            }

            u64 step = 3 * (1 << target_cache->unknown_sib) / 2;
            for (u64 i = 0; i < step && upper < migrated; upper++, migrated--, i++) {
                _swap(cands[upper], cands[migrated]);
            }
        }

        if (upper <= lower) {
            upper = lower + 1;
            if (upper > n_cands) {
                fprintf(stderr, ERR "Upper > n_cands in evset build\n");
                break;
            }
        }
        iters++;
    }

    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);

    if (verbose > 1)
        printf("     Built evset. size: %lu | iterations: %ld | backtracks: %ld\n", 
               evsz, iters, n_bctr);
    
    return false;
}

/*
  single L2 evset with retries if needed
  @return eviction set on success, NULL on failure after n_retries
*/
EvSet* build_single_l2_evset(EvCands* cands, u8* target, u32 set_idx, 
                             u8** all_addrs, u64 n_all_addrs) 
{
    init_def_l2_conf(&def_l2_build_conf);
    EvBuildConf* conf = &def_l2_build_conf;

    if (!target || !cands) {
        u32 unknown_sib = l2_info.unknown_sib;
        cands = _calloc(1, sizeof(EvCands));
        cands->count = l2_info.n_ways * (1 << unknown_sib) * def_l2_build_conf.cand_scale;
        cands->cache = &l2_info;
        
        u64 region_size = 0;
        u64 base_addr_int = init_base_cands(cands, &region_size);
        if (base_addr_int == -1) {
            fprintf(stderr, ERR "Failed to allocate memory for candidates\n");
            return NULL;
        }
        u8* base_addr = (u8*)base_addr_int;
    
        // first addr as target
        target = base_addr;
    }
    
    EvSet* evset = _calloc(1, sizeof(EvSet));
    evset->target_addr = target;
    if (!evset) {
        fprintf(stderr, ERR "failed to allocate eviction set for L2 set %u\n", set_idx);
        return NULL;
    }
    
    evset->size = 0;
    evset->ev_cap = def_l2_build_conf.cap_scaling * l2_info.n_ways;
    evset->addrs = _calloc(evset->ev_cap, sizeof(u8*));
    if (!evset->addrs) {
        fprintf(stderr, ERR "failed to allocate evset addresses\n");
        free(evset);
        return NULL;
    }     

    evset->target_cache = &l2_info;
    evset->build_conf = conf;
    evset->cands = cands;
    
    u32 n_ret = conf->n_retries;
    u8* curr_target = target;
    bool success = false;
    
    while (n_ret > 0 && !success) {
        if (verbose > 1)
            printf(V2 "attempt for set %u, tries left: %u\n", set_idx, n_ret);
        
        // reset
        evset->size = 0;
        
        if (build_evset_zhao(curr_target, evset)) {
            fprintf(stderr, WRN "failed to build eviction set %u\n", set_idx);
            n_ret--;
            
            // find a new target in cands for retry, if any left
            bool found_new_target = false;
            for (u64 j = 0; j < cands->count; j++) {
                // don't reuse cur target
                if (cands->addrs[j] != curr_target) {
                    bool is_in_prev_evsets = false;
                    
                    // in all_addrs (any previous eviction set)?
                    for (u64 a = 0; a < n_all_addrs; a++) {
                        if (all_addrs[a] == cands->addrs[j]) {
                            is_in_prev_evsets = true;
                            break;
                        }
                    }
                    
                    if (!is_in_prev_evsets) {
                        curr_target = cands->addrs[j];
                        
                        // remove this target from cands pool
                        _swap(cands->addrs[j], cands->addrs[cands->count-1]);
                        cands->count--;
                        
                        found_new_target = true;
                        break;
                    }
                }
            }
            
            if (!found_new_target) {
                fprintf(stderr, ERR "no more suitable candidates available for retry\n");
                break;
            }
            
            continue;
        }
        
        // check if evset size matches expected n_ways and verify if it works
        if (evset->size != l2_info.n_ways ||
            verify_evset(evset, curr_target) == FAIL) {
            if (evset->size != l2_info.n_ways) {
                if (verbose > 1)
                    printf(V2 WRN "built evset size %u doesn't match L2 n_ways %u, retrying %u/%u\n", 
                            evset->size, l2_info.n_ways, 
                            evset->build_conf->n_retries - n_ret + 1, evset->build_conf->n_retries);
            } else {
                if (verbose > 1)
                    printf(V2 WRN "evset verification failed for set %u, retrying %u/%u\n", 
                            set_idx, evset->build_conf->n_retries - n_ret + 1, 
                            evset->build_conf->n_retries);
            }
            
            n_ret--;
            
            // find a new target in cands for retry
            bool found_new_target = false;
            for (u64 j = 0; j < cands->count; j++) {
                // don't reuse the current target
                if (cands->addrs[j] != curr_target) {
                    bool is_in_prev_evsets = false;
                    
                    // check if this candidate is in all_addrs (previous eviction sets)
                    for (u64 a = 0; a < n_all_addrs; a++) {
                        if (all_addrs[a] == cands->addrs[j]) {
                            is_in_prev_evsets = true;
                            break;
                        }
                    }
                    
                    if (!is_in_prev_evsets) {
                        curr_target = cands->addrs[j];
                        
                        // remove this target from candidates pool
                        _swap(cands->addrs[j], cands->addrs[cands->count-1]);
                        cands->count--;
                        
                        found_new_target = true;
                        break;
                    }
                }
            }
            
            if (!found_new_target) {
                fprintf(stderr, ERR "no more suitable candidates available for retry\n");
                break;
            }
        } else {
            success = true;
        }
    }
    
    if (!success) {
        free(evset->addrs);
        free(evset);
        return NULL;
    }
    
    return evset;
}

EvSet ***build_l2_evset(u32 num_sets)
{
    u64 all_start = 0,
        all_end = 0;
    all_start = time_us();

    u32 unknown_sib = l2_info.unknown_sib;
    u32 n_uncertain_sets = 1 << unknown_sib;
    u32 n_offsets = PAGE_SIZE / l2_info.cl_size; // within a page

    u32 requested_sets;
    requested_sets = num_sets;

    u32 sets_per_uncertain = n_offsets;
    u32 uncertain_sets_to_build = (requested_sets + sets_per_uncertain - 1)
                                  / sets_per_uncertain;
    
    // cap at max possible uncertain sets
    if (uncertain_sets_to_build > n_uncertain_sets)
        uncertain_sets_to_build = n_uncertain_sets;
    
    u32 total_sets_possible = uncertain_sets_to_build * sets_per_uncertain;
    
    printf(NOTE "Requested sets: %u; this would build a total of %u sets after shifting\n", 
            requested_sets, total_sets_possible);
    
    init_def_l2_conf(&def_l2_build_conf);

    u32 max_whole_ret = def_l2_build_conf.max_whole_ret;
    u32 whole_ret = 0, success_count = 0;
    EvSet*** l2_evset_complex = NULL;
    // track addresses from all successfully built evsets
    u8 **built_evset_addrs = NULL;
    u64 n_built_evset_addrs = 0;

    while (whole_ret <= max_whole_ret) {
        if (l2_evset_complex != NULL) {
            if (verbose)
                printf(V1 "L2 evset build retry %u/%u.\n", whole_ret, max_whole_ret);
                
            // free prev failing complex
            for (u32 n = 0; n < n_offsets; n++) {
                if (l2_evset_complex[n]) {
                    for (u32 i = 0; i < uncertain_sets_to_build; i++) {
                        if (l2_evset_complex[n][i]) {
                            free(l2_evset_complex[n][i]->addrs);
                            free(l2_evset_complex[n][i]);
                        }
                    }
                    free(l2_evset_complex[n]);
                }
            }
            free(l2_evset_complex);
            l2_evset_complex = NULL;
        }
        
        l2_evset_complex = _calloc(n_offsets, sizeof(EvSet**));
        if (!l2_evset_complex) {
            fprintf(stderr, ERR "Failed to allocate evset complex\n");
            return NULL;
        }
    
        u64 region_size = 0;
        EvCands* cands = _calloc(1, sizeof(EvCands));
        if (!cands) {
            fprintf(stderr, ERR "Failed to allocate eviction candidates\n");
            free(l2_evset_complex);
            return NULL;
        }

        cands->count = l2_info.n_ways * (1 << unknown_sib) * def_l2_build_conf.cand_scale;
        if (verbose > 1)
            printf(V2 "n_cands for L2 evset construction: %lu\n", cands->count);
        cands->cache = &l2_info;
        
        u64 base_addr_int = init_base_cands(cands, &region_size);
        if (base_addr_int == -1) {
            fprintf(stderr, ERR "Failed to allocate memory for candidates\n");
            return NULL;
        }
        u8* base_addr = (u8*)base_addr_int;
        
        l2_evset_complex[0] = _calloc(uncertain_sets_to_build, sizeof(EvSet*));
        if (!l2_evset_complex[0]) {
            fprintf(stderr, ERR "Failed to allocate evset array\n");
            cleanup_mem(base_addr, cands, region_size);
            free(l2_evset_complex);
            return NULL;
        }
        
        // first addr as target
        u8* target = base_addr;
        
        // addrs used in all evsets
        u8** all_addrs = NULL;
        u64 n_all_addrs = 0;

        
        for (u32 i = 0; i < uncertain_sets_to_build; i++) {
            if (verbose > 1)
                printf(V1 "Building eviction set %u/%u\n", i+1, uncertain_sets_to_build);
            
            // for sets after the first
            if (i > 0) {
                bool found_target = false;

                /*
                  test each cand until we find one that
                  does not get evicted by the combined addresses
                  of all previously built evsets (unique L2 color)
                */
                for (u64 j = 0; j < cands->count && !found_target; j++) {
                    bool is_non_conflicting = true;

                    if (n_built_evset_addrs > 0) {
                        EvRes res = test_eviction(cands->addrs[j],
                                                  built_evset_addrs,
                                                  n_built_evset_addrs,
                                                  &def_l2_build_conf);

                        if (res == OK) {
                            is_non_conflicting = false;

                            if (debug > 1) {
                                u64 target_hpa = va_to_hpa(cands->addrs[j]);
                                printf(D2 "Candidate target %p (HPA 0x%lx) evicted by previously built evsets; skipping\n",
                                       cands->addrs[j], target_hpa);
                            } else if (verbose > 1) {
                                printf(V2 "Candidate target %p evicted by previous evsets, skipping\n",
                                       cands->addrs[j]);
                            }
                        }
                    }

                    if (is_non_conflicting) {
                        target = cands->addrs[j];

                        if (debug > 1) {
                            u64 target_hpa = va_to_hpa(target);
                            printf(D2 "Chosen target (HPA 0x%lx) not evicted by any previous evset\n",
                                   target_hpa);
                        }

                        if (verbose > 1) {
                            printf(V2 "Chosen target %p not evicted by any previous evset\n", target);
                        }

                        // remove this target from cands
                        _swap(cands->addrs[j], cands->addrs[cands->count-1]);
                        cands->count--;

                        found_target = true;
                        break;
                    }
                }

                if (!found_target) {
                    fprintf(stderr, ERR "Failed to find non-conflicting target for set %u\n", i);
                    continue;
                }
            }
            
            l2_evset_complex[0][i] = build_single_l2_evset(cands, target, i, all_addrs, n_all_addrs);
            
            if (l2_evset_complex[0][i]) {
                bool verification_failed = true;
                for (i32 verify_retry = 0; verify_retry < 3; verify_retry++) {
                    if (verify_evset(l2_evset_complex[0][i], target) == OK) {
                        verification_failed = false;
                        break;
                    }
            
                    if (verbose > 1)
                        printf(V2 "Verification attempt %d/3 failed for evset %u\n", verify_retry + 1, i);
                }

                if (verification_failed) {
                    if (verbose > 1)
                        fprintf(stderr, V2 ERR "Eviction set %u verification failed after 3 attempts, will retry.\n", i);

                    if (debug > 2) {
                        printf(D3 "Target that failed eviction: VA %p -> HPA 0x%lx\n",
                            target, va_to_hpa(target));

                        printf(D3 "Eviction set that failed to evict target:\n");
                        for (u32 j = 0; j < l2_evset_complex[0][i]->size; j++) {
                            u64 hpa = va_to_hpa(l2_evset_complex[0][i]->addrs[j]);
                            printf("  [%u]: %p -> HPA 0x%lx\n", j, l2_evset_complex[0][i]->addrs[j], hpa);
                        }
                    }

                    free(l2_evset_complex[0][i]->addrs);
                    free(l2_evset_complex[0][i]);
                    l2_evset_complex[0][i] = NULL;
                    continue;
                }
                
                // add evset addrs to tracking arr & remove from cands pool
                u8** new_all_addrs = _calloc(n_all_addrs + l2_evset_complex[0][i]->size, sizeof(u8*));
                if (new_all_addrs) {
                    if (all_addrs) {
                        memcpy(new_all_addrs, all_addrs, n_all_addrs * sizeof(u8*));
                        free(all_addrs);
                    }
                    
                    // add new addrs, remove from cand pool
                    for (u64 k = 0; k < l2_evset_complex[0][i]->size; k++) {
                        new_all_addrs[n_all_addrs + k] = l2_evset_complex[0][i]->addrs[k];
                        
                        // remove this from cands
                        for (u64 c = 0; c < cands->count; c++) {
                            if (cands->addrs[c] == l2_evset_complex[0][i]->addrs[k]) {
                                _swap(cands->addrs[c], cands->addrs[cands->count-1]);
                                cands->count--;
                                break;
                            }
                        }
                    }
                    
                    all_addrs = new_all_addrs;
                    n_all_addrs += l2_evset_complex[0][i]->size;

                    // add to flat array for future eviction checks
                    u8** new_built = _calloc(n_built_evset_addrs + l2_evset_complex[0][i]->size,
                                            sizeof(u8*));
                    if (new_built) {
                        if (built_evset_addrs) {
                            memcpy(new_built, built_evset_addrs,
                                   n_built_evset_addrs * sizeof(u8*));
                            free(built_evset_addrs);
                        }
                        memcpy(new_built + n_built_evset_addrs,
                               l2_evset_complex[0][i]->addrs,
                               l2_evset_complex[0][i]->size * sizeof(u8*));
                        built_evset_addrs = new_built;
                        n_built_evset_addrs += l2_evset_complex[0][i]->size;
                    }
                }
            }
            
            // enough cands left?
            if (cands->count < l2_info.n_ways * def_l2_build_conf.cand_scale)
                fprintf(stderr, WRN "Low n_candidates, remaining evsets might fail.\n");
        }
        
        // shifted variants of uncertain sets
        for (u32 n = 1; n < n_offsets; n++) {
            l2_evset_complex[n] = _calloc(uncertain_sets_to_build, sizeof(EvSet*));
            if (!l2_evset_complex[n]) {
                fprintf(stderr, ERR "Failed to allocate evset array for offset %u\n", n);
                continue;
            }
            
            for (u32 i = 0; i < uncertain_sets_to_build; i++) {
                if (l2_evset_complex[0][i]) {
                    l2_evset_complex[n][i] = evset_shift(l2_evset_complex[0][i], n * l2_info.cl_size);
                }
            }
        }
        
        if (all_addrs)
            free(all_addrs);
        if (built_evset_addrs)
            free(built_evset_addrs);
        built_evset_addrs = NULL;
        n_built_evset_addrs = 0;
        
        // all sets must be built (if still retries left)
        success_count = 0;
        for (u32 i = 0; i < uncertain_sets_to_build; i++) {
            if (l2_evset_complex[0][i])
                success_count++;
        }
        
        u32 total_sets_built = success_count * n_offsets;
        if (verbose && success_count != uncertain_sets_to_build) { // failed building all sets requested
            printf(SUC "Built %u/%u total sets (%u/%u uncertain sets; rest are shifted)\n", 
                   total_sets_built, total_sets_possible,
                   success_count, uncertain_sets_to_build);
        } else if (success_count == uncertain_sets_to_build) {
            printf(SUC "Built %u/%u total sets (%u/%u uncertain sets; rest are shifted)\n", 
                   total_sets_built, total_sets_possible,
                   success_count, uncertain_sets_to_build);
        }

        if (success_count == uncertain_sets_to_build)
            break;
        
        // otherwise clean up and retry
        whole_ret++;
        if (whole_ret <= max_whole_ret) {
            if (verbose > 1) {
                printf(V2 WRN "Couldn't build all uncertain sets (%u/%u), retrying whole procedure (%u/%u).\n", 
                    success_count, uncertain_sets_to_build, whole_ret, max_whole_ret);
            }
        } else {
            printf(WRN "Couldn't build all uncertain sets (%u/%u) after max retries (%u).\n", 
                  success_count, uncertain_sets_to_build, max_whole_ret);
            return NULL;
        }
        cleanup_mem(base_addr, cands, region_size);
        if (built_evset_addrs)
            free(built_evset_addrs);
        built_evset_addrs = NULL;
        n_built_evset_addrs = 0;
    }

    if (built_evset_addrs)
        free(built_evset_addrs);

    if (debug > 0) {
        srand(time(0)); // in case for d3: random shifted evset
        
        for (u32 i = 0; i < uncertain_sets_to_build; i++) {

            if (l2_evset_complex[0][i]) {

                printf(D1 "L2 evset %u address mapping sanity check:\n", i);

                EvSet* cur = l2_evset_complex[0][i];
                u32 t_hpa = va_to_hpa(cur->target_addr); // target addr HPA
                u32 t_hpa_l2_sib = cache_get_sib(t_hpa, &l2_info);
                u32 w = n_digits(cur->size); // width

                printf("  T_ad: %p -> HPA 0x%lx (L2 SIB 0x%x)\n", 
                       cur->target_addr, va_to_hpa(cur->target_addr), t_hpa_l2_sib);

                u32 n;
                n = (debug > 1) ? cur->size : 1;
                for (u32 j = 0; j < n; j++) {
                    u64 hpa = va_to_hpa(cur->addrs[j]);
                    u32 hpa_l2_sib = cache_get_sib(hpa, &l2_info);

                    printf("  [%*u]: %p -> HPA 0x%lx [%s]\n", 
                           w, j, cur->addrs[j], hpa,
                           (hpa_l2_sib == t_hpa_l2_sib) ?
                           GRN "L2 SIB match" RST:
                           RED "Bad L2 SIB " RST);
                }
                if (debug == 1)
                    printf("  ... (%u more addresses) | show with -d 2\n", cur->size - n);
                
                // d3
                if (debug > 2 && n_offsets > 1) {
                    printf(D3 "Sample of shifted evsets from EvSet %u:\n", i);
                    // adr at some random offset that has been shifted
                    u32 rand_o = 1 + rand() % (n_offsets - 1);
                    if (l2_evset_complex[rand_o] && l2_evset_complex[rand_o][i]) {
                        printf("  random shifted offset %u * 0x%x:\n", rand_o, l2_info.cl_size);
                        for (u32 j = 0; j < l2_evset_complex[rand_o][i]->size; j++) {
                            u64 hpa = va_to_hpa(l2_evset_complex[rand_o][i]->addrs[j]);
                            printf("  [%u]: %p -> HPA 0x%lx\n", j, l2_evset_complex[rand_o][i]->addrs[j], hpa);
                        }
                    }
                }
                printf("\n");
            }
        }

        // check if uncertain L2 set index bits (color) 
        // of different L2 evsets from diff colors are repeated or missing
        u32 *colors = _calloc(uncertain_sets_to_build, sizeof(u32));
        bool repeat_found = false;

        if (!colors) {
            printf(D1 WRN "Failed to allocate color tracking array\n");
        }

        for (u32 i = 0; i < uncertain_sets_to_build; i++) {
            if (!l2_evset_complex[0][i]) {
                printf(D1 WRN "Evset %u missing\n", i);
                repeat_found = true;
                if (colors)
                    colors[i] = UINT_MAX;
                continue;
            }

            u64 hpa = va_to_hpa(l2_evset_complex[0][i]->addrs[0]);
            u32 color = cache_get_color(hpa, &l2_info);
            if (colors)
                colors[i] = color;

            for (u32 j = 0; j < i; j++) {
                if (!l2_evset_complex[0][j] || !colors)
                    continue;
                if (colors[j] != UINT_MAX && colors[j] == color) {
                    repeat_found = true;
                    if (debug > 1)
                        printf(D1 WRN "Evset %u and %u share L2 color 0x%x (HPA 0x%lx)\n",
                               j, i, color, hpa);
                    else
                        printf(D1 WRN "Evset %u and %u share L2 color 0x%x\n",
                               j, i, color);
                }
            }
        }

        if (colors) {
            printf(D1 "L2 colors:");
            for (u32 i = 0; i < uncertain_sets_to_build; i++) {
                if (colors[i] != UINT_MAX)
                    printf(" 0x%x", colors[i]);
            }
            printf("\n");
        }

        if (!repeat_found)
            printf(D1 SUC "All L2 colors of uncertain L2 evsets are unique.\n");

        if (colors)
            free(colors);
    }
    
    all_end = time_us();
    printf(INFO "Completed L2 evsets build | %.3fms\n", (all_end - all_start) / 1e3);
    return l2_evset_complex;
}


bool final_l3_evset(EvSet* l3ev)
{
    u64 timeout_start = time_us();
    bool can_evict = false;
    u8* target = l3ev->target_addr;

    u32 ret = l3ev->build_conf->verify_retry;
    for (u32 r = 0; r < ret; r++) {
        l3ev->size = 0; // reset
        bool build_err = build_evset_zhao(target, l3ev);

        if (build_err) continue;
        
        if (test_eviction(target, l3ev->addrs, l3ev->size, l3ev->build_conf) == OK) {
            l3ev->size = prune_evcands(target, l3ev->addrs, l3ev->size, l3ev->build_conf);
            can_evict = true;
            break;
        } else {
            if (verbose > 1) {
                printf("     " V2 "Built evset's verification failed. Size: %u | retry %u/%u \n", 
                       l3ev->size, r + 1, ret);
            }
        }

        l3ev->target_addr = l3ev->cands->addrs[l3ev->cands->count - 1];
        l3ev->cands->count--;
        target = l3ev->target_addr;

        if (l3ev->build_conf->retry_timeout && 
            (time_us() - timeout_start) > l3ev->build_conf->retry_timeout * 1000) {
            printf(WRN "timeout during L3 eviction set construction\n");
            break;
        }
    }

    return !can_evict;
}

EvSet** build_single_l3_evset(void)
{
    u64 l3ev_build_start = 0, l3ev_build_end = 0,
        l2_build_start = 0, l2_build_end = 0,
        topo_start = 0, topo_end = 0,
        all_start = 0, all_end = 0;

    all_start = time_us();

    init_def_l2_conf(&def_l2_build_conf);
    
    u32 num_sets_to_build = 1;
    
    EvSet** l3_evsets = _calloc(num_sets_to_build, sizeof(EvSet*));
    if (!l3_evsets) {
        fprintf(stderr, ERR "Failed to allocate L3 evsets array\n");
        return NULL;
    }
    
    u8** all_evset_addrs = NULL;
    u64 n_all_evset_addrs = 0;
    
    EvSet* l2_evset = NULL;
    l2_build_start = time_us();
    l2_evset = build_single_l2_evset(NULL, NULL, 0, NULL, 0);
    l2_build_end = time_us();

    if (!l2_evset) {
        fprintf(stderr, ERR "Failed to build L2 evset\n");
        return NULL;
    }

    printf(INFO "Built L2 evset | %.2fms\n", (l2_build_end - l2_build_start) / 1e3);

    if (debug > 1) {
        printf(D2 "L2 evset for filtering: \n");
        u32 width = n_digits(l2_evset->size);

        u64 t_hpa = va_to_hpa(l2_evset->target_addr); // target addr HPA
        u64 t_hpa_l2_sib = cache_get_sib(t_hpa, &l2_info);

        printf("  Target: %p -> HPA 0x%lx (L2 SIB: 0x%lx)\n",
               l2_evset->target_addr, t_hpa, t_hpa_l2_sib);
        for (u32 i = 0; i < l2_evset->size; i++) {
            u64 hpa = va_to_hpa(l2_evset->addrs[i]);
            u64 hpa_l2_sib = cache_get_sib(hpa, &l2_info);
            bool is_good_l2_sib = (t_hpa_l2_sib == hpa_l2_sib);
            printf("  [%*u]: %p -> HPA 0x%lx [%s]\n", 
                    width, i, l2_evset->addrs[i], hpa,
                    (is_good_l2_sib) ? GRN "L2 SIB match" RST : RED "Bad L2 SIB" RST);
        }
    }

    init_def_l3_conf(&def_l3_build_conf, l2_evset, &hctrl);
    
    i32 main_core_id = -1; // default: don't pin
    i32 helper_core_id = -1; 

    if (vtop) {
        topo_start = time_us();
        cpu_topology_t* topo = get_vcpu_topo();
        topo_end = time_us();
        printf(INFO "vTop Done | %.3fms\n", (topo_end - topo_start) / 1e3);
        if (topo) print_cpu_topology(topo);
        find_same_socket_nonSMT_vcpu_pair(topo, &main_core_id, &helper_core_id);
        if (main_core_id != -1 && helper_core_id != -1) { // topology detection failed
            printf(INFO "Pinning main to vCPU %d; helper to vCPU %d\n\n", 
                    main_core_id, helper_core_id);
        }

        if (main_core_id != -1 && helper_core_id != -1) {
            // main thread
            if (set_cpu_affinity(main_core_id) != 0) {
                fprintf(stderr, ERR "failed to pin main thread to core %d\n", main_core_id);
            }
        }
        
        // set helper thread core id and start pinning
        hctrl.core_id = helper_core_id;
        start_helper_thread_pinned(&hctrl, helper_core_id);
    } else {
        hctrl.core_id = -1; // default: don't pin
        start_helper_thread(&hctrl);
    }
    
    u32 n_built_evsets = 0;
    
    u32 n_uncertain_sets = 1 << l3_info.unknown_sib;
    u32 n_cands = l3_info.n_ways * n_uncertain_sets
                    * l3_info.n_slices * def_l3_build_conf.cand_scale;
    
    EvCands* l3_cands = _calloc(1, sizeof(EvCands));
    if (!l3_cands) {
        fprintf(stderr, ERR "failed to allocate L3 cands\n");
        stop_helper_thread(&hctrl);
        free(l2_evset->addrs);
        free(l2_evset);
        if (l3_evsets[0]) {
            free(l3_evsets[0]->addrs);
            free(l3_evsets[0]);
        }
        free(l3_evsets);
        free(all_evset_addrs);
        return NULL;
    }
    
    l3_cands->cache = &l3_info;
    l3_cands->count = n_cands;
    
    u64 region_size = 0;
    u64 base_addr_int = init_base_cands(l3_cands, &region_size);
    if (base_addr_int == -1) {
        fprintf(stderr, ERR "failed to allocate memory for L3 candidates\n");
        return NULL;
    }
    
    u8* base_addr = (u8*)base_addr_int;
    u8* target = base_addr;
    
    l2_evset->build_conf = &def_l2_build_conf;
    u64 n_filtered = 0;
    u32 start = time_us();
#if FILTER_BATCH // works best on e.g. SKX, Cascade
    evcands_filter_batch(l3_cands->addrs, l3_cands->count, &n_filtered, 
                         l2_evset, &def_l2_build_conf);
#else // sequential filtering. some uarchs like icelake and larger LLCs on Intel Xeon platinum aren't reliable with batch filtering
    for (u32 i = 0; i < l3_cands->count; i++) {
        if (test_eviction(l3_cands->addrs[i], l2_evset->addrs,
                          l2_evset->size, &def_l2_build_conf) == OK) {
            _swap(l3_cands->addrs[n_filtered], l3_cands->addrs[i]);
                n_filtered += 1;
        }
    }
#endif

    u32 end = time_us();

    if (n_filtered == 0) {
        fprintf(stderr, ERR "0 cands filtered.\n");
        cleanup_mem(base_addr, l3_cands, region_size);
        return NULL;
    }

    printf(INFO "Filtered %lu candidate lines to %ld | %.3fms\n",
           l3_cands->count, n_filtered, (end - start) / 1e3);

    if (debug) {
        u32 t_hpa = va_to_hpa(l2_evset->target_addr); // target address's HPA
        u32 t_hpa_l2_sib = cache_get_sib(t_hpa, &l2_info);
        u32 n_healthy = 0; // healthy if cand's L2 SIB matches filter ev's target
        for (u32 i = 0; i < n_filtered; i++) {
            u32 cand_hpa = va_to_hpa(l3_cands->addrs[i]);
            u32 cand_hpa_l2_sib = cache_get_sib(cand_hpa, &l2_info);

            if (cand_hpa_l2_sib == t_hpa_l2_sib)
                n_healthy++;
        }

        printf(D1 "%u/%lu filtered candidate lines are healthy (%.2f%%)\n",
                n_healthy, n_filtered, (f32)n_healthy/(f32)n_filtered * 100.00);
    }

    u8 **tmp = realloc(l3_cands->addrs, n_filtered * sizeof(*l3_cands->addrs));
    if (!tmp) {
        fprintf(stderr, ERR "Failed to realloc filtered cand array\n");
        cleanup_mem(base_addr, l3_cands, region_size);
    }

    l3_cands->addrs = tmp;
    l3_cands->count = n_filtered;
    target = l3_cands->addrs[n_filtered - 1];
    l3_cands->count--; // exclude

    if (debug > 1) {
        u32 n_sample = 10; // cap to 10
        n_sample = l3_cands->count < n_sample ? l3_cands->count : n_sample;
        puts(D2 "sample of filtered L3/LLC candidates:");
        
        for (u32 i = 0; i < n_sample; i++) {
            u64 cand_hpa = va_to_hpa(l3_cands->addrs[i]);
            printf("  cand[%u]: %p -> HPA 0x%lx\n", 
                    i, l3_cands->addrs[i], cand_hpa);
        }
    }
    
    // check if filtered cands have a potential evset to begin with
    if (test_eviction(target, l3_cands->addrs,
                        l3_cands->count, &def_l3_build_conf) == FAIL) {
        fprintf(stderr, ERR "filtered L3 candidates do not have a potential evset for target\n");
        return NULL;
    }
    
    EvSet* l3_evset = _calloc(1, sizeof(EvSet));
    if (!l3_evset) {
        fprintf(stderr, ERR "failed to allocate L3 eviction set\n");
        cleanup_mem(base_addr, l3_cands, region_size);
    }
    
    l3_evset->size = 0;
    l3_evset->cands = l3_cands;
    l3_evset->ev_cap = l3_info.n_ways * def_l3_build_conf.cap_scaling;
    l3_evset->addrs = _calloc(l3_evset->ev_cap, sizeof(u8*));
    l3_evset->target_cache = &l3_info;
    l3_evset->build_conf = &def_l3_build_conf;
    l3_evset->target_addr = target;

    if (!l3_evset->addrs) {
        fprintf(stderr, ERR "failed to allocate L3 evset addresses array\n");
        free(l3_evset);
        cleanup_mem(base_addr, l3_cands, region_size);
    }
    
    if (debug > 1)
        printf(D2 "Target HPA: 0x%lx\n",
               va_to_hpa(l3_evset->target_addr));
    
    l3ev_build_start = time_us();
    bool build_error = final_l3_evset(l3_evset);
    l3ev_build_end = time_us();
    
    if (build_error) {
        fprintf(stderr, ERR "failed to build L3 eviction set (evset size: %u)\n", 
                l3_evset->size);

        free(l3_evset->addrs);
        free(l3_evset);
        return NULL;
    }
    
    printf(SUC "L3 evset built | size: %u | %.3fms pruning\n", 
           l3_evset->size, (l3ev_build_end - l3ev_build_start) / 1e3);
    
    EvRes verify_result = verify_evset(l3_evset, target);
    
    if (verify_result != OK) {
        if (debug > 1) {
            puts(D2 ERR "Failed L3 Evset");
            u32 width = n_digits(l3_evset->size);
            u64 t_hpa = va_to_hpa(target);
            u64 t_hpa_sib = cache_get_sib(t_hpa, &l3_info);
            u32 t_hpa_slice = l3_slice_skx_20(t_hpa);

            printf("  Target: %p -> HPA 0x%lx [L3 SIB: 0x%lx] [Slice: %u]\n", target,
                    t_hpa, t_hpa_sib, t_hpa_slice);
            for (u32 i = 0; i < l3_evset->size; i++) {
                u64 hpa = va_to_hpa(l3_evset->addrs[i]);
                u64 hpa_sib = cache_get_sib(hpa, &l3_info);
                u32 hpa_slice = l3_slice_skx_20(hpa);
                bool is_good_sib = (t_hpa_sib == hpa_sib);
                bool is_good_slice = (t_hpa_slice == hpa_slice);
                printf("  [%*u]: %p -> HPA 0x%lx [%s (0x%lx)] [%s (%u)]\n", 
                    width, i, l3_evset->addrs[i], hpa, 
                    (is_good_sib) ? GRN "L3 SIB match" RST : RED "Bad L3 SIB" RST,
                    hpa_sib,
                    (is_good_slice) ? GRN "Slice match" RST : RED "Bad Slice" RST,
                    hpa_slice);
            }
        }

        fprintf(stderr, ERR "L3 eviction set verification failed\n");
        free(l3_evset->addrs);
        free(l3_evset);
        return NULL;
    }
    
    if (debug) {
        u64 target_hpa = va_to_hpa(target);
        //  (addr >> 6) & 0x7FF in our processor gives L3 set index bits
        //  so we need bits [6:16]
        u64 t_l3_sib = cache_get_sib(target_hpa, &l3_info);
        u32 t_l3_slice = l3_slice_skx_20(target_hpa);
        
        puts(D1 "L3 eviction set addresses:");
        printf(D1 "Target: %p -> HPA 0x%lx (L3 SIB: 0x%lx | Slice: %u)\n", 
                    target, target_hpa, t_l3_sib, t_l3_slice);
        u32 w = n_digits(l3_evset->size); // width
        u32 good_sib_count = 0;
        
        for (u32 i = 0; i < l3_evset->size; i++) {
            u64 hpa = va_to_hpa(l3_evset->addrs[i]);
            u32 hpa_slice = l3_slice_skx_20(hpa);
            u64 addr_set_bits = cache_get_sib(hpa, &l3_info);
            bool is_good_sib = (addr_set_bits == t_l3_sib);
            bool is_good_slice = (t_l3_slice == hpa_slice);
            good_sib_count += is_good_sib;
            
            printf("  [%*u]: %p -> HPA 0x%lx [%s] [%s (%u)]\n", 
                    w, i, l3_evset->addrs[i], hpa, 
                    (is_good_sib ? GRN "L3 SIB match" RST : RED "Bad L3 SIB" RST),
                    (is_good_slice ? GRN "Slice match" RST : RED "Bad Slice" RST),
                    hpa_slice);
        }
    }
    
    /* for hosts where checking GPA would still be valid
    if (verbose) {
        u64 t_gpa = va_to_pa(target);
        //  (addr >> 6) & 0x7FF in our processor gives L3 set index bits
        //  so we need bits [6:16]
        u64 t_l3_sib = cache_get_sib(t_gpa, &l3_info);
        u32 t_l3_slice = l3_slice_skx_20(t_gpa);
        
        puts(D1 "L3 eviction set %u addresses:");
        printf(D1 "Target: %p -> GPA 0x%lx (L3 SIB: 0x%lx | Slice: %u)\n", 
                    target, t_gpa, t_l3_sib, t_l3_slice);
        u32 w = n_digits(l3_evset->size); // width
        u32 good_sib_count = 0;
        
        for (u32 i = 0; i < l3_evset->size; i++) {
            u64 gpa = va_to_pa(l3_evset->addrs[i]);
            u64 addr_set_bits = cache_get_sib(gpa, &l3_info);
            bool is_good_sib = (addr_set_bits == t_l3_sib);
            good_sib_count += is_good_sib;
            
            printf("  [%*u]: %p -> GPA 0x%lx [%s]\n", 
                   w, i, l3_evset->addrs[i], gpa, 
                   (is_good_sib ? GRN "L3 SIB match" RST : RED "Bad L3 SIB" RST));
        }
    }
    */


    l3_evsets[n_built_evsets++] = l3_evset;
    
    // add all addrs from this evset to tracking arr
    u8** new_all_addrs = _calloc(n_all_evset_addrs + l3_evset->size + 1, sizeof(u8*));
    if (new_all_addrs) {
        if (all_evset_addrs) {
            memcpy(new_all_addrs, all_evset_addrs, n_all_evset_addrs * sizeof(u8*));
            free(all_evset_addrs);
        }
        
        // add target first
        new_all_addrs[n_all_evset_addrs++] = target;
        
        // add all addresses from the evset
        for (u32 i = 0; i < l3_evset->size; i++) {
            new_all_addrs[n_all_evset_addrs++] = l3_evset->addrs[i];
        }
        
        all_evset_addrs = new_all_addrs;
    }
    
    all_end = time_us();

    /*
    * if called from vset, we should not free or stop helper.
    * vSet needs them both later to perform monitoring.
    * freeing causes segfaults later on, and stopping helper
    * results in hanging (main waits infinitely)
    */
    if (!vset) {
        stop_helper_thread(&hctrl);

        if (all_evset_addrs)
            free(all_evset_addrs);

        free(l2_evset->addrs);
        free(l2_evset);
    }     

    printf(SUC "Built %u/%u L3 eviction sets | %.3fms\n", 
           n_built_evsets, num_sets_to_build, (all_end - all_start) / 1e3);
    
    if (n_built_evsets == 0) { // failed
        free(l3_evsets);
        return NULL;
    }
    
    return l3_evsets;
}

void *vtop_main_thread_worker(void *arg)
{
    vtop_thread_pair *tp = (vtop_thread_pair *)arg;
    if (!tp) return NULL;
    
    if (set_cpu_affinity(tp->main_vcpu) != 0) {
        fprintf(stderr, ERR "failed to pin main thread to vCPU %d\n", tp->main_vcpu);
    }

    if (start_helper_thread_pinned(&tp->helper_ctrl, tp->helper_vcpu)) {
        fprintf(stderr, ERR "failed to start helper thread on vCPU %d\n", tp->helper_vcpu);
        return NULL;
    }    

    u64 l3_cnt = 0;
    tp->total_built = 0;
    
    // all assigned offsets
    while (tp->current_offset_idx <= tp->max_offset_idx) {
        u32 c = tp->current_offset_idx++;
        
        if (c > tp->max_offset_idx)
            break; // done
            
        u32 n = tp->idxs[c];
        u32 offset = n * CL_SIZE;
        
        printf("%s Offset 0x%x: Done\n",
              (c == tp->max_offset_idx) ? "└─ " : "├─ ",
              offset);
        
        u64 offset_succ = 0;
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
                    if (l3_evsets[j] && l3_evsets[j]->size > 0) {
                        tp->total_built++;
                        offset_succ++;
                    }
                }
            }
        }
        
        // success count for this offset
        tp->offset_success[c] = offset_succ;
    }
    
    stop_helper_thread(&tp->helper_ctrl);
    return NULL;
}




/*
 * modified: https://github.com/zzrcxb/LLCFeasible/
 * Supports early termination via 
 * @p max_evsets specifying how many eviction sets to build at most
 */
EvSet **build_evsets_at(u32 offset, EvBuildConf *conf, CacheInfo *cache,
                        EvCands *_cands, u64 *ev_cnt, CacheInfo *lower_cache,
                        EvBuildConf *lower_conf, EvSet **lower_evsets,
                        u64 n_lower_evsets, u32 max_evsets)
{
    u32 built_count = 0;
    EvCands *cands = _cands;
    u8 **cands_backup = NULL;
    u64 cands_sz_backup = 0;
    EvSet **evsets = NULL;
    u64 n_evsets = cache_uncertainty(cache), lower_skipped = 0;
    if (conf->filter_ev) {
        CacheInfo *lower = conf->filter_ev->target_cache;
        n_evsets /= cache_uncertainty(lower);
    }
    *ev_cnt = n_evsets;
    if (max_evsets > 0 && max_evsets < n_evsets) {
        n_evsets = max_evsets;
    }
    *ev_cnt = n_evsets;

    u8 **addrs = NULL;
    u64 acc_cnt = 0;
    if (!cands) {
        cands = evcands_new(cache, conf, NULL);
        if (!cands) {
            fprintf(stderr, ERR "Failed to allocate evcands\n");
            goto err;
        }

        if (evcands_populate(offset, cands, conf,
                             -1, offset)) {
            fprintf(stderr, ERR "Failed to populate evcands\n");
            goto err;
        }
    }

    cands_backup = cands->addrs;
    cands_sz_backup = cands->count;

    evsets = _calloc(n_evsets, sizeof(EvSet*));
    if (!evsets || cands->count == 0) {
        goto err;
    }

    u64 l2_time_us = 0;
    u8 *target = cands->addrs[cands->count - 1];
    cands->count -= 1;

    for (u64 i = 0; i < n_evsets && cands->count > 0; i++) {
        EvSet *evset = NULL;
        if (n_lower_evsets) {
            u64 start = time_us();
            conf->lower_ev = NULL;
            for (u32 i = 0; i < n_lower_evsets; i++) {
                if (verify_evset(lower_evsets[i], target) == OK) {
                    conf->lower_ev = lower_evsets[i];
                    break;
                }
            }
            l2_time_us += (time_us() - start);

            if (!conf->lower_ev) {
                lower_skipped += 1;
                goto l2_ev_fail;
            }
        }

        evset = _calloc(1, sizeof(EvSet));
        if (!evset) {
            fprintf(stderr, ERR "Failed to alloc EvSet at build_evsets_at\n");
        }
        evset->cands = cands;
        evset->ev_cap = l3_info.n_ways * conf->cap_scaling;
        evset->build_conf = conf;
        evset->target_cache = cache;
        evset->addrs = _calloc(evset->ev_cap, sizeof(u8*));
        evset->target_addr = target;

        bool build_error = final_l3_evset(evset);

        if (!build_error && evset->size != 0) {
            evsets[i] = evset;
            built_count += 1;
            cands->addrs += evset->size;
            cands->count -= evset->size;

            evsets[i] = evset;
            if (!addrs) {
                addrs = _calloc(evset->ev_cap * n_evsets, sizeof(*addrs));
                if (!addrs)
                    goto err;
            }

            memcpy(&addrs[acc_cnt], evset->addrs, evset->size * sizeof(*addrs));
            acc_cnt += evset->size;
        } else {
            if (verbose > 1) {
                fprintf(stderr, V2 ERR "failed to build L3 eviction set %lu\n", i);
            }
            free(evset->addrs);
            free(evset);
            continue;
        }

    l2_ev_fail:
        if (i == n_evsets - 1 || cands->count == 0) break;

        // find next target
        if (addrs) {
            bool found = false;
            for (u64 j = 0; j < cands->count && cands->count > 0; j++) {
                EvRes res = test_eviction(cands->addrs[j], addrs, acc_cnt, conf);
                if (res == FAIL) {
                    target = cands->addrs[j];
                    _swap(cands->addrs[j], cands->addrs[cands->count - 1]);
                    cands->count -= 1;
                    found = true;
                    break;
                }
            }

            if (!found) {
                fprintf(stderr, ERR "Cannot find the next target: cands size: %lu | addr: %lu\n",
                    cands->count, acc_cnt);
                break;
            }
        } else {
            cands->count -= 1;
            target = cands->addrs[cands->count];
        }
    }

    cleanup:
        if (lower_skipped) {
            printf(INFO "skipped due to lower evset fail: %lu\n", lower_skipped);
        }

        if (cands_backup) {
            cands->addrs = cands_backup;
            cands->count = cands_sz_backup;
        }
        free(addrs);
        return evsets;

    err:
        free(evsets);
        evsets = NULL;
        goto cleanup;
}


void evbuffer_free(EvBuffer *evb)
{
    if (!evb)
        return;
    if (evb->buf) {
        munmap(evb->buf, evb->n_pages * PAGE_SIZE);
        evb->buf = NULL;
    }
    free(evb);
}

void evcands_free(EvCands *cands)
{
    if (!cands)
        return;
    if (cands->evb) {
        if (cands->evb->ref_cnt > 0)
            cands->evb->ref_cnt -= 1;

        if (cands->evb->ref_cnt == 0)
            evbuffer_free(cands->evb);
    }
    if (cands->addrs)
        free(cands->addrs);

    free(cands);
}

void free_evset_complex(EvSet ****complex, u32 num_offsets,
                        u32 num_l2_sets, u32 evsets_per_l2)
{
    if (!complex)
        return;
    for (u32 off = 0; off < num_offsets; off++) {
        for (u32 c = 0; c < num_l2_sets; c++) {
            for (u32 e = 0; e < evsets_per_l2; e++) {
                EvSet *ev = complex[off][c][e];
                if (ev) {
                    if (ev->addrs)
                        free(ev->addrs);
                    if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                        ev->build_conf != &def_l2_build_conf)
                        free(ev->build_conf);
                    free(ev);
                }
            }
            free(complex[off][c]);
        }
        free(complex[off]);
    }
    free(complex);
}

