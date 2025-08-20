#ifndef VSET_OPS_H
#define VSET_OPS_H

#include "common.h"
#include "evset.h"
#include "helper_thread.h"
#include "cache_ops.h"
#include "cache_info.h"
#include "asm.h"
#include "utils.h"
#include "lats.h"
#include "../vm_tools/vtop.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif // VSET_OPS_H

// access frequency
#define GRAPH_MAX_DATA_POINTS 50000 // for frequency monitor with -G
#define GRAPH_TIME_LIMIT_MS 200     // in ms

// occupancy heatmap params
#define DEFAULT_HEATMAP_TIME_STEP_US 25  // default time step in microseconds
#define DEFAULT_HEATMAP_MAX_TIME_US 7000 // default max timeline in microseconds
#define HEATMAP_SAMPLES_PER_SLOT 40      // experiments per time slot

typedef struct {
    i32 main_vcpu;
    i32 helper_vcpu;
    EvSet *l3ev;
    i64 threshold;
    u64 rate_cycles;
    helper_thread_ctrl hctrl;
} socket_monitor_ctx;

typedef struct {
    u32 set_idx;
    EvSet *evset;
    i64 threshold;
    u64 rate_cycles;
    u64 n_samples;
    u32 *cycle_diffs;
    helper_thread_ctrl hctrl;
} set_monitor_ctx;

typedef struct {
    u32 time_slot_us;
    u32 n_evicted;         // number of lines evicted (0 to n_ways)
    f64 percentage;        // % of occurrences at this slot
} heatmap_data_point;

typedef struct {
    u32 tid;
    u32 wait_us;
    u64 cycles_per_us;
    u32 iterations;
    EvSet ***color_sets;
    u32 *color_counts;
    f64 **tot_avg;
    u32 start_color;
    u32 num_colors;
    i32 core_main;
    i32 core_helper;
    u64 *prime_times;
    u64 *probe_times;
} l2c_occ_worker_arg;

extern bool lcas_mode;
extern u32 lcas_period_ms;
extern u32 scan_period_ms;
extern f64 lcas_alpha_rise;
extern f64 lcas_alpha_fall;
extern u32 granular_sets;
extern u32 check_remap;
extern u64 max_num_recs;
extern u64 retry;
extern u64 wait_time_us;
extern u64 og_wait_time_us;
extern bool fix_wait;
extern u32 heatmap_time_step_us;
extern u32 heatmap_max_time_us;

i32 move_cgroup_hi(void);

void *l2c_occ_worker(void *arg);

void write_eviction_freq_data(u32 *cycle_diffs, u64 n_samples, bool plot);

void write_heatmap_data(heatmap_data_point *data, u32 n_time_slots, u32 n_ways,
                        f64 *avg_evicted_per_slot, i32 socket_id);

void write_evrate_wait_data(u32 *times_us, f64 *rates, u32 n_points,
                            u32 prime_time_us);

void write_l2color_data(f64 **avg_data, u32 n_colors, u32 n_iters, u32 wait_us,
                        u32 n_ways, u32 *color_counts);

u32 monitor_l3_occupancy(void);

u32 monitor_eviction_rate_wait(void);

u32 monitor_l3_occupancy_l2color(void);

u32 monitor_l3_occupancy_lcas(void);

u32 monitor_eviction_pct_single(u32 iterations);

u32 monitor_eviction_rate_multi(u32 iterations);

f64 estimate_heatmap_wait_time(void);

f64 estimate_heatmap_runtime(u32 n_time_slots, u32 batch_prime_us,
                             u32 single_prime_us);

f64 estimate_l2color_runtime(u32 n_colors, u32 iterations, u32 wait_us);

void l3_evset_prime(EvSet *evset, u64 threshold);

i64 calibrate_grp_access_lat(u8 *target, EvSet *evset, EvBuildConf *tconf);

u32 monitor_socket_activity_freq(socket_monitor_ctx *ctx, bool plot);

u32 monitor_l3_activity_freq(bool plot);

void graph_l2color_distribution(void);

i32 check_mem_remap_cheat(void);

void perf_prime_probe(void);

i32 fraction_check(void);

#ifdef __cplusplus
}
#endif // VSET_OPS_H

#endif
