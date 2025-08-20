// latency related
#ifndef LATS_H
#define LATS_H

#include "cache_info.h"
#include "asm.h"

extern CacheLats g_lats;

void init_cache_lats_thresh(u32 reps);

// modified: https://github.com/zzrcxb/LLCFeasible/
u32 ALWAYS_INLINE hit_thresh_zhao(u32 hit_lat, u32 miss_lat)
{
    // I find 4.6 to have less edge cases than 5.0
    return (3 * hit_lat + 2 * miss_lat) / 4.6;
}

void init_l1d_lat(u32 reps);

void init_l2_lat(u32 reps);

void init_l3_lat(u32 reps);

void init_dram_lat(u32 reps);

void ALWAYS_INLINE init_interrupt_thresh()
{
    g_lats.interrupt_thresh = g_lats.dram * 5;
}

i32 calc_median(i32 *nums_arr, u32 cnt);

i32 calc_avg(i32 *num_arr, u32 cnt);

i32 calc_min_cluster(i32 *arr, u32 cnt);

#endif // LATS_H

