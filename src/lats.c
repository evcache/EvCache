/*
* timing and cache latency related
*/
#include "../include/lats.h"
#include "../include/cache_info.h"
#include "../include/cache_ops.h"
#include "../include/bitwise.h"
#include "../include/utils.h"

CacheLats g_lats = {0};

static ALWAYS_INLINE i32 compare_lats(const void *lhs, const void *rhs) 
{
    const i64 *l = (i64 *)lhs, *r = (i64 *)rhs;
    if (*l < *r) return -1;
    if (*l > *r) return 1;
    return 0;
}

i32 calc_median(i32 *nums_arr, u32 cnt) 
{
    if (cnt == 0) return -1;
    if (cnt == 1) return nums_arr[0];

    qsort(nums_arr, cnt, sizeof(nums_arr[0]), compare_lats);
    if (cnt % 2) {
        return nums_arr[cnt / 2];
    } else {
        return (nums_arr[cnt / 2] + nums_arr[cnt / 2 - 1]) / 2;
    }
}

i32 calc_avg(i32 *nums_arr, u32 cnt)
{
    if (cnt == 0) return -1;
    if (cnt == 1) return nums_arr[0];

    i32 sum = 0;
    for (u32 i = 0; i < cnt; i++)
        sum += nums_arr[i];

    return sum / cnt;
}

i32 calc_min_cluster(i32 *arr, u32 cnt) 
{
    if (cnt == 0) return -1;
    if (cnt == 1) return arr[0];

    qsort(arr, cnt, sizeof(arr[0]), compare_lats);
    
    i32 *cleaned = _calloc(cnt, sizeof(i32)); // tmp arr for valid vals
    if (!cleaned)
        return -1;
    u32 clean_len = 0;
    u32 max_ways = l3_info.n_ways;
    
    // filter out 0s and values greater than LLC ways
    for (u32 i = 0; i < cnt; i++) {
        if (arr[i] > 0 && arr[i] <= max_ways) {
            cleaned[clean_len++] = arr[i];
        }
    }
    
    if (clean_len == 0) {
        free(cleaned);
        return -1; // no valid measurements
    }
    
    // count freqs of each value
    i32 vals[32] = {0}; // should be maximally LLC n_ways, but needs to be resolved compile time
    u32 freqs[32] = {0}; // same here
    u32 num_vals = 0;
    
    for (u32 i = 0; i < clean_len; i++) {
        i32 val = cleaned[i];
        u32 found = 0;
        
        for (u32 j = 0; j < num_vals; j++) {
            if (vals[j] == val) {
                freqs[j]++;
                found = 1;
                break;
            }
        }
        
        if (!found) {
            vals[num_vals] = val;
            freqs[num_vals] = 1;
            num_vals++;
        }
    }
    
    // find smallest val with freq >= 5%
    u32 min_freq = (clean_len * 5) / 100;
    i32 best_val = -1;
    
    for (u32 i = 0; i < num_vals; i++) {
        if (freqs[i] >= min_freq) {
            if (best_val == -1 || vals[i] < best_val) {
                best_val = vals[i];
            }
        }
    }
    
    // fallback to median if no value meets threshold
    if (best_val == -1) {
        if (clean_len % 2) {
            best_val = cleaned[clean_len / 2];
        } else {
            best_val = (cleaned[clean_len / 2] + cleaned[clean_len / 2 - 1]) / 2;
        }
    }
    
    free(cleaned);
    return best_val;
}

void init_dram_lat(u32 reps)
{
    u8 *target = _calloc(8, 1);
    i32 *lats_arr = _calloc(reps, sizeof(lats_arr[0]));
    if (!target || !lats_arr) {
        fprintf(stderr, ERR "Failed allocation for DRAM lat.");
        goto err;
    } 

    i32 median = -1, lat_cnt = 0;

    for (u32 i = 0; i < reps; i++) {
        u32 aux1, aux2;
        _rdtscp_aux(&aux1);

        _clflush(target);
        u64 lat = _time_maccess(target);

        _rdtscp_aux(&aux2);

        if (aux1 == aux2)
            lats_arr[lat_cnt++] = lat;
    }
    if (lat_cnt > reps / 2) {
        median = calc_median(lats_arr, reps);
    } else {
        fprintf(stderr, ERR "Too many context switches during measurement of DRAM latency.");
    }

    g_lats.dram = median;

err:
    free(target);
    free(lats_arr);
}

void init_l1d_lat(u32 reps)
{
    u8 *target = _calloc(8, 1);
    i32 *measurements = _calloc(reps, sizeof(i32));
    if (!target || !measurements) {
        fprintf(stderr, ERR "Failed allocation for L1d lat.");
        goto cleanup;
    } 

    i32 median = -1;

    for (u32 i = 0; i < reps; i++) {
        _mwrite(target, 0x1);
        u64 lat = _time_maccess(target);
        measurements[i] = lat;
    }
    // the array has reps-many elements
    median = calc_median(measurements, reps);
    g_lats.l1d = median;

cleanup:
    free(target);
    free(measurements);
}

void init_l2_lat(u32 reps)
{
    u32 aux1, aux2;
    u32 ev_size = 3 * l1_info.n_ways * (1 << l1_info.unknown_sib);
    u32 buf_size = (ev_size + 2) * PAGE_SIZE;
    u8 *ev_buf = _calloc(buf_size, 1);
    i32 *measurements = _calloc(reps, sizeof(i32));
    i32 median = -1, lat_cnt = 0;
    
    if (!ev_buf || !measurements) {
        fprintf(stderr, ERR "failed allocation for l2 latency measurement\n");
        goto cleanup;
    }
    
    u8 *page = (u8 *)__ALIGN_UP(ev_buf, PAGE_SHIFT);
    u32 offsets_per_page = 16;
    u32 offset_step = PAGE_SIZE / offsets_per_page;
    
    for (u32 i = 0; i < reps; i++) {
        u32 offset_index = (i / (reps / offsets_per_page)) % offsets_per_page;
        
        u8 *target_addr = page + offset_index * offset_step;
        u8 *tlb_target = page + ((offset_index + 1) % offsets_per_page) * offset_step;
        u8 *ev_start = target_addr + PAGE_SIZE;
        
        _rdtscp_aux(&aux1);
        _mwrite(target_addr, 0x1);
        _mfence();
        _lfence();
        
        for (u32 j = 0; j < 5; j++)
            access_stride(ev_start, PAGE_SIZE, ev_size);

        _lfence();
        maccess(tlb_target);
        u64 lat = _time_maccess(target_addr);

        _rdtscp_aux(&aux2);
        if (aux1 == aux2 && lat < g_lats.interrupt_thresh)
            measurements[lat_cnt++] = lat;
    }
    
    if (lat_cnt < reps / 2) {
        fprintf(stderr, ERR "too many context switches during l2 latency detection\n");
    } else {
        median = calc_median(measurements, lat_cnt);
    }
    g_lats.l2 = median;
    
cleanup:
    free(ev_buf);
    free(measurements);
}

void init_l3_lat(u32 reps)
{
    u32 interrupt_thresh = g_lats.interrupt_thresh;
    u32 dram_lat = g_lats.dram;
    u32 max_ret = 3;
    
    for (u32 attempt = 0; attempt < max_ret; attempt++) {
        u32 aux1, aux2;
        u32 ev_size = 2.5 * l2_info.n_ways * g_n_uncertain_l2_sets;
        u32 buf_size = (ev_size + 2) * PAGE_SIZE;
        u8 *ev_buf = _calloc(buf_size, 1);
        i32 *measurements = _calloc(reps, sizeof(i32));
        i32 median = -1, lat_cnt = 0;

        if (!ev_buf || !measurements) {
            fprintf(stderr, ERR "failed allocation for L3 latency measurement.\n");
            free(ev_buf);
            free(measurements);
            continue;  // try again
        }
        
        u8 *page = (u8 *)__ALIGN_UP(ev_buf, PAGE_SHIFT);
        u32 offsets_per_page = 16;
        u32 offset_step = PAGE_SIZE / offsets_per_page;
        
        for (u32 i = 0; i < reps; i++) {
            u32 offset_index = (i / (reps / offsets_per_page)) % offsets_per_page;
            
            u8 *target_addr = page + offset_index * offset_step;
            u8 *tlb_target = page + ((offset_index + 1) % offsets_per_page) * offset_step;
            u8 *ev_start = target_addr + PAGE_SIZE;
            
            _mfence();
            _lfence();

            _rdtscp_aux(&aux1);
            _mwrite(target_addr, 0x1);

            _mfence();
            _lfence();
            
            for (u32 j = 0; j < 5; j++) {
                access_stride(ev_start, PAGE_SIZE, ev_size);
            }

            _lfence();
            maccess(tlb_target);
            u64 lat = _time_maccess(target_addr);
            
            _rdtscp_aux(&aux2);
            if (aux1 == aux2 &&
                lat < interrupt_thresh &&
                lat < dram_lat * 8 / 10) {
                measurements[lat_cnt++] = lat;
            }
        }
        
        if (lat_cnt < reps / 3) { // more mercy
            if (verbose)
                printf(V1 WRN "Attempt %u/%u: too many context switches during L3 latency detection.\n", 
                        attempt + 1, max_ret);
            free(ev_buf);
            free(measurements);
            continue;  // try again
        }
        
        median = calc_median(measurements, lat_cnt);
        g_lats.l3 = median;
        
        free(ev_buf);
        free(measurements);
        return; // success
    }
    
    // all retries failed
    fprintf(stderr, ERR "Failed to measure L3 latency after %u attempts.\n", max_ret);
    exit(EXIT_FAILURE);
}

void init_cache_lats_thresh(u32 reps)
{
    // setting up interrupt threshold first, because it's
    // used to sanity check measurements for upcoming functions
    init_dram_lat(reps); 
    init_interrupt_thresh();
    init_l1d_lat(reps);
    init_l2_lat(reps);
    init_l3_lat(reps);

    // rare abnormal latency edge case
    if (g_lats.l2 <= g_lats.l1d) g_lats.l2 = g_lats.l1d * 1.2;
    if (g_lats.l3 <= g_lats.l2 * 1.15) g_lats.l3 = g_lats.l2 * 1.8;

    g_lats.l1d_thresh = hit_thresh_zhao(g_lats.l1d, g_lats.l2);
    g_lats.l2_thresh = hit_thresh_zhao(g_lats.l2, g_lats.l3);
    g_lats.l3_thresh = hit_thresh_zhao(g_lats.l3, g_lats.dram);
    if (g_lats.l3_thresh > (u32)g_lats.l3 * 2.5) {
        g_lats.l3_thresh = g_lats.l3 * 2; // adjust abnormal cases
    }
}
