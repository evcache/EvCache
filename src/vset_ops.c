#include "../include/vset_ops.h"
#include "../include/cache_info.h"
#include "time.h"
#include "../include/bitwise.h"
#include "../include/cache_ops.h"
#include "../include/utils.h"
#include "../include/lats.h"
#include "../include/config.h"
#include "../include/evset.h"
#include "../include/evset_para.h"
#include "../vm_tools/gpa_hpa.h"
#include "../vm_tools/vtop.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/bpf.h>
#include <unistd.h>
#include <math.h>

#define LCAS_MAP_PATH "/sys/fs/bpf/lcas_dom_order"
#define LCAS_MAX_SOCKETS 16
#define PERF_PP_ITERS 10

bool lcas_mode = false;
u32 lcas_period_ms = 1000; // update interval for --lcas
u32 scan_period_ms = 1000; // interval between iterations for live/graph modes
f64 lcas_alpha_rise = 0.85, lcas_alpha_fall = 0.85;
u32 granular_sets = 16;
u32 check_remap = 60 * 60 * 12; // 12 hrs in secs
u64 max_num_recs = 300;
u64 retry = 3;
u64 wait_time_us = 7000; // default wait time in microseconds
u64 og_wait_time_us = 7000; // for the case of expanding, we need to save this val to jump back to it
bool fix_wait = false;
u32 heatmap_time_step_us = DEFAULT_HEATMAP_TIME_STEP_US;
u32 heatmap_max_time_us = DEFAULT_HEATMAP_MAX_TIME_US;

static void select_vcpu_pair(socket_info_t *si, cpu_topology_t *topo,
                             i32 *main_vcpu, i32 *helper_vcpu)
{
    *main_vcpu = -1;
    *helper_vcpu = -1;
    for (i32 i = 0; i < si->vcpu_count; i++) {
        for (i32 j = i + 1; j < si->vcpu_count; j++) {
            i32 v1 = si->vcpus[i];
            i32 v2 = si->vcpus[j];
            if (topo->relation_matrix[v1][v2] == CPU_RELATION_SMT)
                continue; // skip if they're smt siblings
            *main_vcpu = v1;
            *helper_vcpu = v2;
            return;
        }
    }
}

// from other modules
extern i32 verbose;
extern EvBuildConf def_l2_build_conf;
extern EvBuildConf def_l3_build_conf;
extern i32 debug;
extern bool remap;
extern bool graph_mode;
extern bool vtop;
extern CacheLats g_lats;
extern CacheInfo l1_info, l2_info, l3_info;

f64 ALWAYS_INLINE binomial(i32 n, i32 k)
{
    return exp(lgamma(n + 1) - lgamma(k + 1) - lgamma(n - k + 1));
}

static void update_lcas_map(u32 *order, u32 n_sockets, bool no_pref)
{
    i32 fd = bpf_obj_get(LCAS_MAP_PATH);
    if (fd < 0)
        return;

    if (no_pref) {
        u32 inval = n_sockets;
        bpf_map_update_elem(fd, &(u32){0}, &inval, BPF_ANY);
    } else {
        for (u32 i = 0; i < n_sockets; i++)
            bpf_map_update_elem(fd, &i, &order[i], BPF_ANY);
    }

    close(fd);
}

static i32 lcas_level(f64 h)
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
    u32 socket;
    f64 hot;
} socket_hot_t;

static i32 cmp_cool(const void *a, const void *b)
{
    const socket_hot_t *aa = a, *bb = b;
    if (aa->hot > bb->hot)
        return 1;
    if (aa->hot < bb->hot)
        return -1;
    return 0;
}

typedef struct {
    i32 main_vcpu;
    i32 helper_vcpu;
    i32 socket_id;
    EvSet ****complex;
    u32 result;
} heatmap_thread_arg;

i32 move_cgroup_hi(void)
{
    FILE *f;
    pid_t pid;
    
    pid = getpid();
    
    f = fopen("/sys/fs/cgroup/hi_prgroup/cgroup.procs", "w");
    if (f == NULL) {
        perror("fopen");
        return -1;
    }
    
    if (fprintf(f, "%d\n", pid) < 0) {
        perror("fprintf");
        fclose(f);
        return -1;
    }
    
    if (fclose(f) != 0) {
        perror("fclose");
        return -1;
    }
    
    return 0;
}

static u32 monitor_l3_occupancy_heatmap_impl(i32 main_vcpu, i32 helper_vcpu,
                                             i32 socket_id, EvSet ****prebuilt);

void *l2c_occ_worker(void *arg)
{
    l2c_occ_worker_arg *w = arg;

    set_cpu_affinity(w->core_main);
    helper_thread_ctrl hctrl = {0};
    start_helper_thread_pinned(&hctrl, w->core_helper);

    /* attach this helper thread to all eviction sets this worker will
       operate on */
    attach_helper_to_evsets(w->color_sets, w->color_counts, w->start_color,
                            w->num_colors, &hctrl);

    for (u32 it = 0; it < w->iterations; it++) {
        u64 prime_begin_tsc = _rdtsc();

        /* prime all colors up front */
        for (u32 idx = 0; idx < w->num_colors; idx++) {
            u32 color = w->start_color + idx;
            u32 set_cnt = w->color_counts[color];
            for (u32 s = 0; s < set_cnt; s++) {
                EvSet *ev = w->color_sets[color][s];
                flush_array(ev->addrs, ev->size);
                _lfence();
                l3_evset_prime(ev, g_lats.l3_thresh);
                _lfence();
            }
        }

        u64 prime_end_tsc = _rdtsc();
        u64 prime_cycles = prime_end_tsc - prime_begin_tsc;
        if (w->prime_times)
            w->prime_times[it] = prime_cycles / w->cycles_per_us;

        /* wait for remaining portion of the configured delay */
        if (w->wait_us > 0) {
            u32 prime_time_us = prime_cycles / w->cycles_per_us;
            if (w->wait_us > prime_time_us) {
                u64 remaining_cycles =
                    (u64)(w->wait_us - prime_time_us) * w->cycles_per_us;
                u64 end_tsc = _rdtsc() + remaining_cycles;
                while (_rdtsc() < end_tsc)
                    ;
            }
        }

        u64 total_probe_cycles = 0;
        for (u32 idx = 0; idx < w->num_colors; idx++) {
            u32 color = w->start_color + idx;
            u32 set_cnt = w->color_counts[color];
            if (set_cnt == 0)
                continue;

            u64 probe_start = _rdtsc();
            u32 total_evictions = 0;
            for (u32 s = 0; s < set_cnt; s++) {
                EvSet *ev = w->color_sets[color][s];

                for (i32 j = ev->size - 1; j >= 0; j--) {
                    bool valid = false;
                    u64 lat = 0;

                    for (u32 r = 0; r < 3 && !valid; r++) {
                        u32 a1, a2;
                        _rdtscp_aux(&a1);
                        _lfence();
                        lat = _time_maccess(ev->addrs[j]);
                        _rdtscp_aux(&a2);
                        if (a1 == a2 && lat < g_lats.interrupt_thresh)
                            valid = true;
                    }

                    if (lat >= g_lats.l3_thresh)
                        total_evictions++;
                }
            }
            u64 probe_end = _rdtsc();
            total_probe_cycles += probe_end - probe_start;

            w->tot_avg[color][it] = (f64)total_evictions;
        }

        if (w->probe_times)
            w->probe_times[it] = total_probe_cycles / w->cycles_per_us;

        if (scan_period_ms && it + 1 < w->iterations)
            usleep(scan_period_ms * 1000);
    }

    stop_helper_thread(&hctrl);
    return NULL;
}

typedef struct {
    EvSet **sets;
    u32 set_cnt;
    u32 wait_us;
    u64 cycles_per_us;
    u32 retries;
    u32 n_ways;
    u32 *counts;
    u32 total_evicted;
    i32 core_main;
    i32 core_helper;
} heatmap_worker_arg;

static void *heatmap_worker(void *arg)
{
    heatmap_worker_arg *w = arg;

    set_cpu_affinity(w->core_main);
    helper_thread_ctrl hctrl = {0};
    start_helper_thread_pinned(&hctrl, w->core_helper);

    for (u32 s = 0; s < w->set_cnt; s++) {
        EvSet *ev = w->sets[s];
        if (ev && ev->size > 0) {
            ev->build_conf->hctrl = &hctrl;
            if (ev->build_conf->lower_ev &&
                ev->build_conf->lower_ev->build_conf)
                ev->build_conf->lower_ev->build_conf->hctrl = &hctrl;
        }
    }

    u64 prime_begin = _rdtsc();
    for (u32 s = 0; s < w->set_cnt; s++) {
        EvSet *ev = w->sets[s];
        flush_array(ev->addrs, ev->size);
        _lfence();
        l3_evset_prime(ev, g_lats.l3_thresh);
        _lfence();
    }
    u64 prime_cycles = _rdtsc() - prime_begin;

    if (w->wait_us > 0) {
        u32 prime_us = prime_cycles / w->cycles_per_us;
        if (w->wait_us > prime_us)
            usleep(w->wait_us - prime_us);
    }

    for (u32 s = 0; s < w->set_cnt; s++) {
        EvSet *ev = w->sets[s];
        u32 n_evicted = 0;
        for (i32 j = ev->size - 1; j >= 0; j--) {
            bool valid = false;
            u64 lat = 0;
            for (u32 r = 0; r < w->retries && !valid; r++) {
                u32 a1, a2;
                _rdtscp_aux(&a1);
                _lfence();
                lat = _time_maccess(ev->addrs[j]);
                _rdtscp_aux(&a2);
                if (a1 == a2)
                    valid = true;
            }
            if (lat >= g_lats.l3_thresh)
                n_evicted++;
        }

        if (n_evicted <= w->n_ways) {
            w->counts[n_evicted]++;
            w->total_evicted += n_evicted;
        }
    }

    stop_helper_thread(&hctrl);
    return NULL;
}

void write_eviction_freq_data(u32 *cycle_diffs, u64 n_samples, bool plot)
{
    u64 cpu_freq_hz = get_cpu_freq_hz();
    if (cpu_freq_hz == 0) {
        fprintf(stderr, ERR "failed to get cpu freq for graph generation\n");
        return;
    }
    
    // configurable time limit in cycles
    u64 time_limit_cycles = (cpu_freq_hz * GRAPH_TIME_LIMIT_MS) / 1000;
    
    // calculate optimal batch size to achieve target data points
    u32 batch_size = time_limit_cycles / GRAPH_MAX_DATA_POINTS;
    if (batch_size < 100) batch_size = 100; // minimum batch size
    
    // calculate actual timeline based on data
    u64 accumulated_cycles = 0;
    u64 actual_timeline_cycles = 0;
    for (u64 i = 0; i < n_samples; i++) {
        accumulated_cycles += cycle_diffs[i];
        if (accumulated_cycles <= time_limit_cycles) {
            actual_timeline_cycles = accumulated_cycles;
        } else {
            break;
        }
    }
    
    // convert actual timeline to milliseconds
    f64 actual_timeline_ms = (f64)actual_timeline_cycles / ((f64)cpu_freq_hz / 1000.0);
    
    // filename w/ current date/time
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename), "data/vset_freq-%04d-%02d-%02d-%02d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append, sizeof(filename) - strlen(filename) - 1);
    }
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create eviction freq data file: %s\n", filename);
        return;
    }
    
    fprintf(fp, "# l3 cache eviction activity data\n");
    fprintf(fp, "# generated by vset on ");
    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    fprintf(fp, "# cpu freq: %.2f ghz\n", (f64)cpu_freq_hz / 1000000000.0);
    fprintf(fp, "# configured time limit: %d ms\n", GRAPH_TIME_LIMIT_MS);
    fprintf(fp, "# actual timeline: %.3f ms\n", actual_timeline_ms);
    fprintf(fp, "# target data points: %d\n", GRAPH_MAX_DATA_POINTS);
    fprintf(fp, "# batch size: %u cycles\n", batch_size);
    fprintf(fp, "# columns: time_ms eviction_status\n");
    
    // max batches based on actual timeline
    u32 max_batches = (actual_timeline_cycles / batch_size) + 1;
    bool *batch_has_eviction = _calloc(max_batches, sizeof(bool));
    
    if (!batch_has_eviction) {
        fprintf(stderr, ERR "failed to allocate batch tracking arr\n");
        fclose(fp);
        return;
    }
    
    // process cycle diffs into timeline and mark batches that contain evictions
    accumulated_cycles = 0;
    for (u64 i = 0; i < n_samples && accumulated_cycles < actual_timeline_cycles; i++) {
        accumulated_cycles += cycle_diffs[i];
        u32 batch_num = accumulated_cycles / batch_size;
        
        if (batch_num < max_batches) {
            batch_has_eviction[batch_num] = true;
        }
    }
    
    // write data points; each batch gets one data point
    u32 actual_batches = 0;
    for (u32 batch = 0; batch < max_batches; batch++) {
        f64 time_ms = (f64)(batch * batch_size) / ((f64)cpu_freq_hz / 1000.0);
        
        // stop if we exceed actual timeline
        if (time_ms > actual_timeline_ms) break;
        
        u32 eviction_status = batch_has_eviction[batch] ? 1 : 0;
        fprintf(fp, "%.3f %u\n", time_ms, eviction_status);
        actual_batches++;
    }
    
    fclose(fp);
    free(batch_has_eviction);

    printf(INFO "eviction frequency data written to: %s\n", filename);
    if (plot) {
        char command[512];
        sprintf(command, "python3 ../scripts/plot_evictions_freq.py ./%s", filename);
        i32 r = system(command);
        if (r < 0)
            fprintf(stderr, ERR "failed to execute command!\n");
    }
    if (verbose) {
        printf(V1 "  actual timeline: %.3f ms (target: %d ms)\n", actual_timeline_ms, GRAPH_TIME_LIMIT_MS);
        printf(V1 "  data points: %u (target: %d)\n", actual_batches, GRAPH_MAX_DATA_POINTS);
        printf(V1 "  batch size: %u cycles\n", batch_size);
    }
}

u32 monitor_l3_occupancy(void)
{
    i32 ret = EXIT_SUCCESS;
    u32 n_evicted = 0;

    u32 wait_us = wait_time_us;
    u32 retries = 5; // retries per element if context switch occurs

    EvSet** l3_evset = build_single_l3_evset();
    if (!l3_evset) {
        fprintf(stderr, ERR "no l3 evset returned for occupancy monitoring\n");
        return 0;
    }
    
    EvSet* l3ev = l3_evset[0];
    if (!l3ev || l3ev->size == 0) {
        fprintf(stderr, ERR "invalid l3 evset for occupancy monitoring\n");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    flush_array(l3ev->addrs, l3ev->size);
    u32 s = _rdtsc();
    _mfence();
    l3_evset_prime(l3ev, g_lats.l3_thresh);
    _mfence();
    u32 e = _rdtsc();
    if (verbose) 
        printf("arr prime done | %u cycles\n", e - s);
    
    // controlled test
    /* u32 n_to_evict = 0; */
    /* for (u32 i = 0; i < n_to_evict; i++) { */
    /*     _clflushopt(l3ev->addrs[i]); */
    /* } */
    /* _mfence(); _lfence(); */
    
    usleep(wait_us);
    
    if (verbose)
        printf(V1 "wait completed; probing individual array method\n");
    
    u64 arr_start = timer_start();
    
    // bwd for thrashing prevention
    for (i32 i = l3ev->size - 1; i >= 0; i--) {
        bool valid = false;
        u64 lat = 0;
        
        // retry on ctx switches
        for (u32 retry = 0; retry < retries && !valid; retry++) {
            u32 aux1, aux2;
            
            _rdtscp_aux(&aux1);
            _lfence();
            lat = _time_maccess(l3ev->addrs[i]);
            _rdtscp_aux(&aux2);
            
            if (aux1 == aux2) {
                valid = true;
            }
        
            if (!valid) continue;
        
            // got evicted?
            if (lat >= g_lats.l3_thresh)
                n_evicted++;
        }
    }
    
    u64 arr_end = timer_stop();
    u64 arr_probe_time = arr_end - arr_start;
    
    f64 ratio = (f64)n_evicted / (f64)l3ev->size;
    printf(SUC "array individual: %u/%u lines evicted (%.1f%%) | %lu cycles\n", 
            n_evicted, l3ev->size, ratio * 100.0, arr_probe_time);
    
cleanup:
    if (l3ev) {
        free(l3ev->addrs);
        free(l3ev);
    }

    // already checked when returned
    free(l3_evset);
    
    if (ret == EXIT_FAILURE)
        return 0;
    
    return n_evicted;
}

void l3_evset_prime(EvSet *evset, u64 threshold)
{
    addrs_traverse(evset->build_conf->lower_ev->addrs, 
                   evset->build_conf->lower_ev->size,
                   evset->build_conf->lower_ev->build_conf);
    _lfence();
    for (u32 i = 0; i < 10; i++) {
        traverse_cands_mt(evset->addrs, evset->size, evset->build_conf);
        u64 begin = timer_start();
        access_array(evset->addrs, evset->size);
        u64 end = timer_stop();
        if (end - begin < threshold) {
            break;
        }
    }
}

i64 calibrate_grp_access_lat(u8 *target, EvSet *evset, EvBuildConf *tconf)
{
    u64 n_repeat = 500;
    i32 *no_acc_lats = _calloc(n_repeat, sizeof(no_acc_lats[0]));
    i32 *acc_lats = _calloc(n_repeat, sizeof(acc_lats[0]));
    if (!no_acc_lats || !acc_lats) {
        fprintf(stderr, ERR "Failed to calibrate grp access latency\n");
        return 0;
    }

    for (u64 r = 0; r < n_repeat;) {
        u32 aux_before, aux_after;
        _rdtscp_aux(&aux_before);

        // prime
        flush_array(evset->addrs, evset->size);
        _lfence();
        l3_evset_prime(evset, 0);
        _lfence();

        // probe
        u64 begin = timer_start();
        access_array_bwd(evset->addrs, evset->size);
        u64 end = _rdtscp_aux(&aux_after);

        if (aux_before == aux_after) {
            no_acc_lats[r] = end - begin;
            r++;
        }
    }

    for (u64 r = 0; r < n_repeat;) {
        u32 aux_before, aux_after;
        _rdtscp_aux(&aux_before);

        _clflush(target);
        flush_array(evset->addrs, evset->size);
        _lfence();
        l3_evset_prime(evset, 0);
        _lfence();
        maccess(target);
        helper_thread_read_single(target, tconf->hctrl);

        u64 begin = timer_start();
        access_array(evset->addrs, evset->size);
        u64 end = _rdtscp_aux(&aux_after);

        if (aux_before == aux_after) {
            acc_lats[r] = end - begin;
            r++;
        }
    }

    i64 no_acc_lat = calc_median(no_acc_lats, n_repeat);
    i64 acc_lat = calc_median(acc_lats, n_repeat);
    i64 threshold = (no_acc_lat + acc_lat) / 2;
    u32 otc = 0, utc = 0;
    for (u32 i = 0; i < n_repeat; i++) {
        otc += no_acc_lats[i] > threshold;
        utc += acc_lats[i] < threshold;
    }

    printf(INFO "no access: %ld | access: %ld | threshold: %ld | OTC: %u | UTC: %u\n",
           no_acc_lat, acc_lat, threshold, otc, utc);

    if (otc > n_repeat * 0.05 || utc > n_repeat * 0.05) {
        threshold = 0; // bad threshold
    }

    free(no_acc_lats);
    free(acc_lats);
    return threshold;
}

u32 monitor_socket_activity_freq(socket_monitor_ctx *ctx, bool plot)
{
    if (!ctx || !ctx->l3ev) {
        fprintf(stderr, ERR "invalid ctx or evset\n");
        return 0;
    }
    
    // pin main thread
    if (ctx->main_vcpu >= 0) {
        if (set_cpu_affinity(ctx->main_vcpu) != 0) {
            fprintf(stderr, ERR "failed to pin main to vcpu %d\n", ctx->main_vcpu);
        } else if (verbose > 1) {
            printf(V2 "main thread pinned to vcpu %d\n", ctx->main_vcpu);
        }
    }
    
    // pin helper thread
    if (ctx->helper_vcpu >= 0) {
        pin_helper_by_ctrl(&ctx->hctrl, ctx->helper_vcpu);
    }
    
    u64 *timestamps = _calloc(max_num_recs, sizeof(*timestamps));
    u64 *iters = _calloc(max_num_recs, sizeof(*timestamps));
    u64 sz = 0;
    
    u32 aux, last_aux;
    u64 iter = 0;
    _rdtscp_aux(&last_aux);
    l3_evset_prime(ctx->l3ev, ctx->threshold);
    
    while (sz < max_num_recs) {
        u64 begin = timer_start();
        access_array(ctx->l3ev->addrs, ctx->l3ev->size);
        u64 end = _rdtscp_aux(&aux);
        bool ctx_switch = aux != last_aux;
        if ((end - begin) > ctx->threshold || ctx_switch) {
            if (!ctx_switch) {
                iters[sz] = iter;
                timestamps[sz++] = end;
            }
            l3_evset_prime(ctx->l3ev, ctx->threshold);
            last_aux = aux;
        }
        iter += 1;
    }
    
    if (sz < 2) {
        fprintf(stderr, ERR "too few timestamp samples: sz = %lu\n", sz);
        free(timestamps);
        free(iters);
        return 0;
    }
    
    u32 *diffs = _calloc(sz - 1, sizeof(*diffs));
    for (u64 i = 1; i < sz; i++) {
        diffs[i - 1] = timestamps[i] - timestamps[i - 1];
        u64 cycle_diff = timestamps[i] - timestamps[i - 1];
        u64 iter_diff = iters[i] - iters[i - 1];
        f64 cycles_per_iter = (iter_diff > 0) ? (f64)cycle_diff / iter_diff : (f64)cycle_diff;

        if (verbose > 1) {
            printf(V2 "sample %lu: time between accesses: %lu cycles over %lu iterations (%.2f cycles/iteration)\n", 
                   i, cycle_diff, iter_diff, cycles_per_iter);
        }
    }
    
    write_eviction_freq_data(diffs, sz - 1, plot);
    
    ctx->rate_cycles = calc_avg((i32*)diffs, sz - 1);
    
    free(diffs);
    free(timestamps);
    free(iters);
    
    return ctx->rate_cycles;
}

u32 monitor_l3_activity_freq(bool plot)
{
    i32 ret = EXIT_SUCCESS;
    u64 cpu_freq_hz = get_cpu_freq_hz();
    
    u32 rate_cycles = 0;
    cpu_topology_t *topo = NULL;
    multi_socket_info_t socket_info = {0};
    
    if (vtop) {
        topo = get_vcpu_topo();
        if (topo) {
            if (verbose) {
                print_cpu_topology(topo);
            }
            socket_info = get_socket_info(topo);
        } else {
            fprintf(stderr, WRN "failed to detect vcpu topology, proceeding w/o vtop\n");
            vtop = false;
        }
    }

    EvSet** l3_evset = build_single_l3_evset();

    if (!l3_evset) {
        fprintf(stderr, ERR "No L3 evset returned\n");
        if (topo) free(topo);
        return 0;
    }
    
    EvSet* l3ev = NULL;
    l3ev = l3_evset[0];

    i64 threshold = 0;
    
    for (u32 i = 0; i <= retry; i++) {
        if (l3ev && l3ev->size <= l3_info.n_ways) {
            threshold = calibrate_grp_access_lat(l3ev->target_addr, l3ev,
                                                 l3ev->build_conf);
            if (threshold < g_lats.l3) {
                fprintf(stderr, ERR "bad threshold: %ld\n", threshold);
                continue;
            } else {
                break;
            }
        }

        if (verbose > 0) {
            printf(V1 "retry %u\n", i+1);
        }

        free(l3ev->addrs);
        free(l3ev);
        l3ev = NULL;
        threshold = 0;
    }

    if (!l3ev || !threshold) {
        fprintf(stderr, ERR "failed to build an llc evset or calibrate the threshold\n");
        ret = EXIT_FAILURE;
        goto err;
    }

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        ret = EXIT_FAILURE;
        goto err;
    }

    // temporal resolution
    u32 tr_ret = 100;
    u64 begin = timer_start();
    for (size_t i = 0; i < tr_ret; i++) {
        timer_start();
        access_array(l3ev->addrs, l3ev->size);
        timer_stop();
    }
    u64 end = timer_stop();
    printf(INFO "temporal resolution: %lu cycles\n", (end - begin) / tr_ret);
    
    // setup helper thread control
    helper_thread_ctrl hctrl = {0};
    
    if (vtop && socket_info.n_sockets > 1) {
        printf(INFO "vtop: detected %d sockets, monitoring each\n", socket_info.n_sockets);
        
        socket_monitor_ctx *ctx = _calloc(socket_info.n_sockets, sizeof(socket_monitor_ctx));
        if (!ctx) {
            fprintf(stderr, ERR "failed to allocate socket ctx\n");
            goto err;
        }
        
        if (start_helper_thread(&hctrl)) {
            fprintf(stderr, ERR "failed to start helper thread\n");
            free(ctx);
            goto err;
        }
        
        u32 *socket_rates = _calloc(socket_info.n_sockets, sizeof(u32));
        if (!socket_rates) {
            fprintf(stderr, ERR "failed to allocate socket rates\n");
            free(ctx);
            goto err;
        }
        
        // monitor each socket
        for (i32 s = 0; s < socket_info.n_sockets; s++) {
            if (socket_info.sockets[s].vcpu_count < 2) {
                fprintf(stderr, WRN "not enough vcpus on socket %d to monitor\n", s);
                continue;
            }
            
            i32 main_vcpu = -1, helper_vcpu = -1;
            // find a suitable pair of vcpus on this socket
            select_vcpu_pair(&socket_info.sockets[s], topo,
                             &main_vcpu, &helper_vcpu);
            
            if (main_vcpu == -1 || helper_vcpu == -1) {
                fprintf(stderr, WRN "couldn't find suitable vcpu pair on socket %d\n", s);
                continue;
            }
            
            printf(INFO "monitoring socket %d using vcpus %d (main) and %d (helper)\n", 
                   s, main_vcpu, helper_vcpu);
            
            ctx[s].main_vcpu = main_vcpu;
            ctx[s].helper_vcpu = helper_vcpu;
            ctx[s].l3ev = l3ev;
            ctx[s].threshold = threshold;
            ctx[s].hctrl = hctrl;
            
            socket_rates[s] = monitor_socket_activity_freq(&ctx[s], plot);
            
            if (socket_rates[s] > 0) {
                if (cpu_freq_hz != 0) {
                    f64 rate_us = (f64)socket_rates[s] / ((f64)cpu_freq_hz / 1000000.0);
                    printf(SUC "Access rate to Socket %d LLC: %.3f microseconds (CPU @ %.2f GHz)\n", 
                           s, rate_us, (f64)cpu_freq_hz / 1000000000.0);
                } else {
                    printf(SUC "Access rate to Socket %d LLC: %u cycles\n", s, socket_rates[s]);
                }
            } else {
                fprintf(stderr, WRN "failed to measure activity on socket %d\n", s);
            }
        }
        
        // for printing what is returned: avg from all sockets
        for (i32 s = 0; s < socket_info.n_sockets; s++) {
            if (socket_rates[s] > 0) {
                rate_cycles += socket_rates[s];
            }
        }
        rate_cycles /= socket_info.n_sockets;
        
        free(socket_rates);
        free(ctx);
        stop_helper_thread(&hctrl);
    } else {
        // fallback to non-vtop monitoring
        if (start_helper_thread(&hctrl)) {
            fprintf(stderr, ERR "failed to start helper thread\n");
            goto err;
        }
        
        socket_monitor_ctx ctx = {
            .main_vcpu = -1,
            .helper_vcpu = -1,
            .l3ev = l3ev,
            .threshold = threshold,
            .hctrl = hctrl
        };
        
        rate_cycles = monitor_socket_activity_freq(&ctx, plot);
        stop_helper_thread(&hctrl);
    }

err:
    if (l3ev) {
        free(l3ev->addrs);
        free(l3ev);
    }
    free(l3_evset);
    if (topo) free(topo);
    
    if (ret == EXIT_FAILURE) {
        return 0;
    }

    return rate_cycles;
}

void write_heatmap_data(heatmap_data_point *data, u32 n_time_slots, u32 n_ways,
                        f64 *avg_evicted_per_slot, i32 socket_id)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    if (socket_id >= 0) {
        snprintf(filename, sizeof(filename),
                 "data/heatmap-%04d-%02d-%02d-%02d-%02d-%02d-s%d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, socket_id);
    } else {
        snprintf(filename, sizeof(filename),
                 "data/heatmap-%04d-%02d-%02d-%02d-%02d-%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    }
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append, sizeof(filename) - strlen(filename) - 1);
    }
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create heatmap data file: %s\n", filename);
        return;
    }
    
    fprintf(fp, "# l3 cache occupancy heatmap data\n");
    fprintf(fp, "# generated by vset on ");
    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    fprintf(fp, "# time step: %u microseconds\n", heatmap_time_step_us);
    fprintf(fp, "# max time: %u microseconds\n", heatmap_max_time_us);
    fprintf(fp, "# samples per slot: %u\n", HEATMAP_SAMPLES_PER_SLOT);
    fprintf(fp, "# l3 ways: %u\n", n_ways);
    fprintf(fp, "# columns: time_us n_evicted percentage\n");
    
    // write heatmap data points
    for (u32 i = 0; i < n_time_slots * (n_ways + 1); i++) {
        fprintf(fp, "%u %u %.2f\n", 
                data[i].time_slot_us, 
                data[i].n_evicted, 
                data[i].percentage);
    }
    
    // write average line data
    fprintf(fp, "\n# average evicted lines per time slot\n");
    fprintf(fp, "# columns: time_us avg_evicted\n");
    for (u32 t = 0; t < n_time_slots; t++) {
        u32 time_us = t * heatmap_time_step_us;
        fprintf(fp, "%u %.2f\n", time_us, avg_evicted_per_slot[t]);
    }
    
    fclose(fp);
    
    printf(INFO "heatmap data written to: %s\n", filename);
    if (verbose) {
        printf(V1 "  time slots: %u (0 to %u us in %u us steps)\n",
               n_time_slots, heatmap_max_time_us, heatmap_time_step_us);
        printf(V1 "  samples per slot: %u\n", HEATMAP_SAMPLES_PER_SLOT);
        printf(V1 "  detected l3 ways: %u\n", n_ways);
    }
}

void write_l2color_data(f64 **avg_data, u32 n_colors, u32 n_iters, u32 wait_us,
                        u32 n_ways, u32 *color_counts)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "data/l2color-%04d-%02d-%02d-%02d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append, sizeof(filename) - strlen(filename) - 1);
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create l2color data file: %s\n", filename);
        return;
    }

    fprintf(fp, "# l2 color heatmap data\n");
    fprintf(fp, "# generated by vset on %04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    fprintf(fp, "# wait_us: %u\n", wait_us);
    fprintf(fp, "# period_ms: %u\n", scan_period_ms);
    fprintf(fp, "# iterations: %u\n", n_iters);
    fprintf(fp, "# l3 ways: %u\n", n_ways);
    fprintf(fp, "# evsets_per_color:");
    for (u32 c = 0; c < n_colors; c++)
        fprintf(fp, " %u", color_counts[c]);
    fprintf(fp, "\n");
    fprintf(fp, "# columns: color iteration eviction_rate\n");
    fprintf(fp, "# eviction_rate expressed as percentage of evicted lines\n");

    for (u32 c = 0; c < n_colors; c++) {
        for (u32 it = 0; it < n_iters; it++) {
            fprintf(fp, "%u %u %.2f\n", c, it, avg_data[c][it]);
        }
    }

    fclose(fp);
    printf(INFO "l2color data written to: %s\n", filename);
    char command[512];
    sprintf(command, "python3 ../scripts/plot_l2color_heatmap.py --no-line ./%s", filename);
    i32 r = system(command);
    if (r < 0) fprintf(stderr, ERR "failed to execute command!\n");
}

static u32 monitor_l3_occupancy_heatmap_impl(i32 main_vcpu, i32 helper_vcpu,
                                             i32 socket_id,
                                             EvSet ****prebuilt)
{
    i32 ret = EXIT_SUCCESS;
u32 n_time_slots = (heatmap_max_time_us / heatmap_time_step_us) + 1;
    u32 retries = 5; // retries per element if context switch occurs
    f64 *avg_evicted_per_slot = _calloc(n_time_slots, sizeof(f64));
    u32 *samples_per_slot = _calloc(n_time_slots, sizeof(u32));
    u64 total_samples = 0;
    EvSet **all_evsets = NULL;

    if (verbose) {
        printf(V1 "timeline: 0 to %u us in %u us steps (%u slots)\n",
               heatmap_max_time_us, heatmap_time_step_us, n_time_slots);
        printf(V1 "samples per time slot: %u\n", HEATMAP_SAMPLES_PER_SLOT);
    }
    
    u32 max_unc_sets = g_n_uncertain_l2_sets;
    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = max_unc_sets;
    else if (g_config.num_l2_sets > max_unc_sets)
        g_config.num_l2_sets = max_unc_sets;

    EvSet ****l3_complex = prebuilt;
    bool own_complex = false;

    if (!l3_complex) {
        l3_complex =
            build_l3_evsets_para_gran(g_config.num_l2_sets,
                                     g_config.num_offsets, NULL);
        own_complex = true;
    }

    if (!l3_complex) {
        fprintf(stderr, ERR "failed to build granular eviction sets\n");
        return 0;
    }

    u32 max_sets = g_config.num_offsets * g_config.num_l2_sets *
                   g_config.evsets_per_l2;
    all_evsets = _calloc(max_sets, sizeof(EvSet *));
    u32 total_sets = 0;
    if (all_evsets) {
        for (u32 off = 0; off < g_config.num_offsets; off++) {
            for (u32 s = 0; s < g_config.num_l2_sets; s++) {
                for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                    EvSet *ev = l3_complex[off][s][e];
                    if (ev && ev->size > 0)
                        all_evsets[total_sets++] = ev;
                }
            }
        }
    }

    if (total_sets == 0) {
        fprintf(stderr, ERR "no eviction sets built for heatmap monitoring\n");
        ret = EXIT_FAILURE;
        goto cleanup_complex;
    }

    // if CAT is enabled, sys info could be wrong
    // we take observed evsize instead
    u32 n_ways = all_evsets[0]->size;
    printf(INFO "Detected L3 ways: %u\n", n_ways);

    // cycles per microsecond for busy wait loop
    u64 cpu_freq_hz = get_cpu_freq_hz();
    u64 cycles_per_us = cpu_freq_hz ? cpu_freq_hz / 1000000ULL : 2000ULL;
    
    u32 **eviction_counts = _calloc(n_time_slots, sizeof(u32*));
    if (!eviction_counts) {
        fprintf(stderr, ERR "failed to allocate eviction counts array\n");
        ret = EXIT_FAILURE;
        goto cleanup_complex;
    }

    // single set priming time
    u64 tmp_start = _rdtsc();
    flush_array(all_evsets[0]->addrs, all_evsets[0]->size);
    _lfence();
    l3_evset_prime(all_evsets[0], g_lats.l3_thresh);
    _lfence();
    u32 single_prime_us = (u32)((_rdtsc() - tmp_start) / cycles_per_us);

    // batch priming time 
    tmp_start = _rdtsc();
    for (u32 i = 0; i < total_sets; i++) {
        flush_array(all_evsets[i]->addrs, all_evsets[i]->size);
        _lfence();
        l3_evset_prime(all_evsets[i], g_lats.l3_thresh);
        _lfence();
    }
    u32 batch_prime_us = (u32)((_rdtsc() - tmp_start) / cycles_per_us);

    f64 estimated_wait_seconds =
        estimate_heatmap_runtime(n_time_slots, batch_prime_us, single_prime_us);
    u32 estimated_minutes = (u32)(estimated_wait_seconds / 60.0);
    u32 estimated_seconds_remainder = (u32)estimated_wait_seconds % 60;
    
    if (socket_id >= 0)
        printf(INFO "Estimated wait time for socket %d: ", socket_id);
    else
        printf(INFO "Estimated wait time: ");
    if (estimated_minutes > 0) {
        printf("%u minutes %u seconds\n",
               estimated_minutes, estimated_seconds_remainder);

        time_t now;
        struct tm *local_time;
        time(&now);
        local_time = localtime(&now);
        printf("       Start: %02d:%02d:%02d\n", 
                local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
    } else {
        printf("%.1f seconds\n", estimated_wait_seconds);
    }


    for (u32 t = 0; t < n_time_slots; t++) {
        eviction_counts[t] = _calloc(n_ways + 1, sizeof(u32));
        if (!eviction_counts[t]) {
            fprintf(stderr, ERR "failed to allocate eviction counts for time slot %u\n", t);
            ret = EXIT_FAILURE;
            goto cleanup_arrays;
        }
    }

    if (!avg_evicted_per_slot) {
        fprintf(stderr, ERR "failed to allocate avg evicted array\n");
        ret = EXIT_FAILURE;
        goto cleanup_arrays;
    }

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        ret = EXIT_FAILURE;
        goto cleanup_arrays;
    }

    pthread_t *threads = NULL;
    heatmap_worker_arg *wargs = NULL;
    u32 n_pairs = 0;

    if (main_vcpu >= 0 && helper_vcpu >= 0) {
        n_pairs = 1;
        threads = _calloc(1, sizeof(pthread_t));
        wargs = _calloc(1, sizeof(heatmap_worker_arg));
        if (!threads || !wargs) {
            ret = EXIT_FAILURE;
            goto cleanup_arrays;
        }
        wargs[0].sets = all_evsets;
        wargs[0].set_cnt = total_sets;
        wargs[0].core_main = main_vcpu;
        wargs[0].core_helper = helper_vcpu;
    } else {
        i32 n_cores = n_system_cores();
        u32 req_threads = g_config.num_threads ? g_config.num_threads : n_cores;
        if (req_threads > (u32)n_cores)
            req_threads = n_cores;
        n_pairs = req_threads / 2;
        if (n_pairs == 0)
            n_pairs = 1;
        if (n_pairs > total_sets)
            n_pairs = total_sets;
        if (n_cores % 2 != 0 && g_config.num_threads == 0)
            printf(NOTE "odd number of cores (%d) detected. using %u thread pairs\n",
                   n_cores, n_pairs);
        threads = _calloc(n_pairs, sizeof(pthread_t));
        wargs = _calloc(n_pairs, sizeof(heatmap_worker_arg));
        if (!threads || !wargs) {
            ret = EXIT_FAILURE;
            goto cleanup_arrays;
        }
        u32 base = total_sets / n_pairs;
        u32 extra = total_sets % n_pairs;
        u32 next = 0;
        for (u32 i = 0; i < n_pairs; i++) {
            u32 cnt = base + (i < extra ? 1 : 0);
            wargs[i].sets = &all_evsets[next];
            wargs[i].set_cnt = cnt;
            wargs[i].core_main = i * 2;
            wargs[i].core_helper = i * 2 + 1;
            next += cnt;
        }
    }

    // for each time slot
    for (u32 t = 0; t < n_time_slots; t++) {
        u32 current_wait_us = t * heatmap_time_step_us;
        u32 total_evicted = 0;

        if (verbose && (t % 20 == 0 || t == n_time_slots - 1)) {
            printf(V1 "processing time slot %u/%u (%u us). Not all are shown.\n",
                   t + 1, n_time_slots, current_wait_us);
        }

        for (u32 i = 0; i < n_pairs; i++) {
            wargs[i].wait_us = current_wait_us;
            wargs[i].cycles_per_us = cycles_per_us;
            wargs[i].retries = retries;
            wargs[i].n_ways = n_ways;
            wargs[i].counts = _calloc(n_ways + 1, sizeof(u32));
            wargs[i].total_evicted = 0;
            if (!wargs[i].counts) {
                fprintf(stderr, ERR "failed to allocate worker counts\n");
                ret = EXIT_FAILURE;
                n_pairs = i;
                goto cleanup_workers;
            }
            pthread_create(&threads[i], NULL, heatmap_worker, &wargs[i]);
        }
        for (u32 i = 0; i < n_pairs; i++) {
            pthread_join(threads[i], NULL);
            for (u32 ev = 0; ev <= n_ways; ev++)
                eviction_counts[t][ev] += wargs[i].counts[ev];
            total_evicted += wargs[i].total_evicted;
            free(wargs[i].counts);
        }
        avg_evicted_per_slot[t] = (f64)total_evicted / (f64)total_sets;
        samples_per_slot[t] = total_sets;
        total_samples += samples_per_slot[t];
    }

cleanup_workers:
    if (threads)
        free(threads);
    if (wargs)
        free(wargs);
    if (ret == EXIT_FAILURE)
        goto cleanup_arrays;
    
    // convert counts to percentages
    u32 tot_data_points = n_time_slots * (n_ways + 1);
    heatmap_data_point *data = _calloc(tot_data_points, sizeof(heatmap_data_point));
    if (!data) {
        fprintf(stderr, ERR "failed to allocate heatmap data points\n");
        ret = EXIT_FAILURE;
        goto cleanup_arrays;
    }
    
    u32 data_idx = 0;
    for (u32 t = 0; t < n_time_slots; t++) {
        u32 time_us = t * heatmap_time_step_us;
        
        for (u32 evicted = 0; evicted <= n_ways; evicted++) {
            data[data_idx].time_slot_us = time_us;
            data[data_idx].n_evicted = evicted;
            data[data_idx].percentage = (f64)eviction_counts[t][evicted] / (f64)samples_per_slot[t] * 100.0;
            data_idx++;
        }
    }
    
    write_heatmap_data(data, n_time_slots, n_ways, avg_evicted_per_slot,
                       socket_id);
    
    // print summary
    printf(SUC "heatmap occupancy monitoring completed\n");
    if (verbose) {
        f64 avg_samples = (f64)total_samples / (f64)n_time_slots;
        printf(V1 "processed %u time slots (avg %.1f samples each)\n",
               n_time_slots, avg_samples);
        
        // show some sample data
        printf(V1 "sample results:\n");
        for (u32 t = 0; t < _min(5, n_time_slots); t++) {
            u32 time_us = t * heatmap_time_step_us;
            printf(V1 "  %u us: avg %.1f lines evicted\n", time_us, avg_evicted_per_slot[t]);
        }
        if (n_time_slots > 5)
            printf(V1 "  ... (remaining %u slots)\n", n_time_slots - 5);
    }
    
    free(data);

cleanup_arrays:
    if (eviction_counts) {
        for (u32 t = 0; t < n_time_slots; t++) {
            if (eviction_counts[t])
                free(eviction_counts[t]);
        }
        free(eviction_counts);
    }
    if (avg_evicted_per_slot)
        free(avg_evicted_per_slot);
    if (samples_per_slot)
        free(samples_per_slot);



cleanup_complex:
    if (own_complex && l3_complex) {
        for (u32 off = 0; off < g_config.num_offsets; off++) {
            for (u32 s = 0; s < g_config.num_l2_sets; s++) {
                for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                    EvSet *ev = l3_complex[off][s][e];
                    if (ev) {
                        if (ev->addrs)
                            free(ev->addrs);
                        if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                            ev->build_conf != &def_l2_build_conf)
                            free(ev->build_conf);
                        free(ev);
                    }
                }
                free(l3_complex[off][s]);
            }
            free(l3_complex[off]);
        }
        free(l3_complex);
    }
    if (all_evsets)
        free(all_evsets);
    if (ret == EXIT_FAILURE)
        return 0;

    return 1; // success
}

// wrapper
u32 monitor_l3_occupancy_heatmap_pinned(i32 main_vcpu, i32 helper_vcpu,
                                        i32 socket_id)
{
    return monitor_l3_occupancy_heatmap_impl(main_vcpu, helper_vcpu, socket_id,
                                             NULL);
}

// wrapper
u32 monitor_l3_occupancy_heatmap(void)
{
    return monitor_l3_occupancy_heatmap_impl(-1, -1, -1, NULL);
}

// wrapper
u32 monitor_l3_occupancy_heatmap_vtop(void)
{
    cpu_topology_t *topo = get_vcpu_topo();
    multi_socket_info_t socket_info = {0};

    if (!topo) {
        fprintf(stderr, WRN "failed to detect vcpu topology, proceeding w/o vtop\n");
        return monitor_l3_occupancy_heatmap_impl(-1, -1, -1, NULL);
    }

    socket_info = get_socket_info(topo);

    EvSet ****complex =
        build_l3_evsets_para_gran(g_config.num_l2_sets,
                                  g_config.num_offsets, NULL);
    if (!complex) {
        free(topo);
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return 0;
    }

    u32 rate_cycles = 0;
    u32 sockets_used = 0;

    for (i32 s = 0; s < socket_info.n_sockets; s++) {
        if (socket_info.sockets[s].vcpu_count < 2) {
            fprintf(stderr, WRN "not enough vcpus on socket %d to monitor\n", s);
            continue;
        }

        i32 main_vcpu = -1, helper_vcpu = -1;
        // find a suitable pair of vcpus on this socket
        select_vcpu_pair(&socket_info.sockets[s], topo,
                         &main_vcpu, &helper_vcpu);

        if (main_vcpu == -1 || helper_vcpu == -1) {
            fprintf(stderr, WRN "couldn't find suitable vcpu pair on socket %d\n", s);
            continue;
        }

        printf(INFO "monitoring socket %d using vcpus %d (main) and %d (helper)\n",
               s, main_vcpu, helper_vcpu);

        u32 res = monitor_l3_occupancy_heatmap_impl(main_vcpu, helper_vcpu, s,
                                                   complex);
        if (res > 0) {
            sockets_used++;
            rate_cycles += res;
        }

        printf(INFO "socket %d monitoring done\n", s);
    }

    if (topo)
        free(topo);

    free_evset_complex(complex, g_config.num_offsets, g_config.num_l2_sets,
                       g_config.evsets_per_l2);

    if (sockets_used)
        rate_cycles /= sockets_used;

    return sockets_used ? rate_cycles : 0;
}

u32 monitor_l3_occupancy_l2color(void)
{
    // -M specifies iterations, -w wait time, -t sleep between iterations (ms)
    u32 iterations = heatmap_max_time_us;
    u32 wait_us = (u32)wait_time_us;
    f64 **tot_line_ev_avg = NULL;
    u32 ret = 1;

    u32 max_unc_sets = 1 << l2_info.unknown_sib;

    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = max_unc_sets;
    else if (g_config.num_l2_sets > max_unc_sets)
        g_config.num_l2_sets = max_unc_sets;

    EvSet ****l3_complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                    g_config.num_offsets, NULL);
    if (!l3_complex) {
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return 0;
    }

    u32 n_colors = g_config.num_l2_sets;
    u32 max_sets_per_color = g_config.num_offsets * g_config.evsets_per_l2;
    
    EvSet ***color_sets = _calloc(n_colors, sizeof(EvSet**));
    u32 *color_counts = _calloc(n_colors, sizeof(u32));
    
    for (u32 c = 0; c < n_colors; c++) {
        color_sets[c] = _calloc(max_sets_per_color, sizeof(EvSet*));
    }

    u32 n_ways = 0;

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < n_colors; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = l3_complex[off][c][e];
                if (ev && ev->size > 0) {
                    u32 idx = color_counts[c]++;
                    if (idx < max_sets_per_color)
                        color_sets[c][idx] = ev;
                }
            }
        }
    }

    u32 total_valid_colors = 0;
    for (u32 c = 0; c < n_colors; c++) {
        if (color_counts[c] > 0) {
            total_valid_colors++;
        }
    }
    
    if (total_valid_colors < n_colors) {
        fprintf(stderr, ERR "One or more of the colors' evsets were not built.\n");
        goto cleanup;
    }

    for (u32 c = 0; c < n_colors; c++) {
        if (color_counts[c] > 0) {
            n_ways = color_sets[c][0]->size;
            break;
        }
    }

    u64 cpu_freq_hz = get_cpu_freq_hz();
    u64 cycles_per_us = cpu_freq_hz 
                      ? cpu_freq_hz / 1000000ULL 
                      : 2000ULL;

    tot_line_ev_avg = _calloc(n_colors, sizeof(f64*));
    for (u32 c = 0; c < n_colors; c++)
        tot_line_ev_avg[c] = _calloc(iterations, sizeof(f64));

    f64 est = estimate_l2color_runtime(n_colors, iterations, wait_us);
    printf(INFO "Estimated runtime: %.2fs\n", est);

    i32 n_cores = n_system_cores();
    u32 req_threads = g_config.num_threads ? g_config.num_threads : n_cores;
    if (req_threads > (u32)n_cores)
        req_threads = n_cores;

    u32 n_pairs = req_threads / 2;
    if (n_pairs == 0)
        n_pairs = 1;
    if (n_pairs > n_colors)
        n_pairs = n_colors;

    if (n_cores % 2 != 0 && g_config.num_threads == 0)
        printf(NOTE "odd number of cores (%d) detected. using %u thread pairs\n",
               n_cores, n_pairs);

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        ret = 0;
        goto cleanup;
    }

    pthread_t *threads = _calloc(n_pairs, sizeof(pthread_t));
    l2c_occ_worker_arg *wargs = _calloc(n_pairs, sizeof(l2c_occ_worker_arg));
    if (!threads || !wargs) {
        fprintf(stderr, ERR "failed to allocate worker threads\n");
        goto cleanup;
    }

    u32 base = n_colors / n_pairs;
    u32 extra = n_colors % n_pairs;
    u32 next_start = 0;

    for (u32 i = 0; i < n_pairs; i++) {
        u32 count = base + (i < extra ? 1 : 0);

        wargs[i].wait_us = wait_us;
        wargs[i].cycles_per_us = cycles_per_us;
        wargs[i].iterations = iterations;
        wargs[i].color_sets = color_sets;
        wargs[i].color_counts = color_counts;
        wargs[i].tot_avg = tot_line_ev_avg;
        wargs[i].start_color = next_start;
        wargs[i].num_colors = count;
        wargs[i].core_main = i * 2;
        wargs[i].core_helper = i * 2 + 1;

        if (verbose > 0) {
            u32 start_c = next_start;
            u32 end_c = start_c + count - 1;
            printf(V1 "pair %u -> colors [%u:%u] (cores %d+%d)\n", i, start_c,
                   end_c, wargs[i].core_main, wargs[i].core_helper);
        }

        next_start += count;

        if (pthread_create(&threads[i], NULL, l2c_occ_worker, &wargs[i])) {
            fprintf(stderr, ERR "failed to create worker %u\n", i);
            n_pairs = i;
            for (u32 j = 0; j < n_pairs; j++)
                pthread_cancel(threads[j]);
            for (u32 j = 0; j < n_pairs; j++)
                pthread_join(threads[j], NULL);
            goto cleanup_threads;
        }
    }

    for (u32 i = 0; i < n_pairs; i++)
        pthread_join(threads[i], NULL);

    for (u32 c = 0; c < n_colors; c++) {
        if (color_counts[c] == 0)
            continue;
        f64 total_lines = (f64)n_ways * color_counts[c];
        for (u32 it = 0; it < iterations; it++) {
            tot_line_ev_avg[c][it] = (tot_line_ev_avg[c][it] / total_lines) * 100.0;
        }
    }

    write_l2color_data(tot_line_ev_avg, n_colors, iterations,
                       wait_us, n_ways, color_counts);

cleanup_threads:
    if (threads)
        free(threads);
    if (wargs)
        free(wargs);

cleanup:
    if (color_sets) {
        for (u32 c = 0; c < n_colors; c++) {
            if (color_sets[c]) free(color_sets[c]);
        }
        free(color_sets);
    }
    
    if (tot_line_ev_avg) {
        for (u32 c = 0; c < n_colors; c++) {
            if (tot_line_ev_avg[c]) free(tot_line_ev_avg[c]);
        }
        free(tot_line_ev_avg);
    }
    
    if (color_counts) free(color_counts);

    if (l3_complex) {
        for (u32 off = 0; off < g_config.num_offsets; off++) {
            for (u32 c = 0; c < n_colors; c++) {
                for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                    EvSet *ev = l3_complex[off][c][e];
                    if (ev) {
                        if (ev->addrs) free(ev->addrs);
                        if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                            ev->build_conf != &def_l2_build_conf)
                            free(ev->build_conf);
                        free(ev);
                    }
                }
                free(l3_complex[off][c]);
            }
            free(l3_complex[off]);
        }
        free(l3_complex);
    }

    if (ret == 1)
        printf(SUC "l2color occupancy monitoring completed\n");
    return ret;
}

u32 monitor_l3_occupancy_lcas(void)
{
    if (!vtop) {
        fprintf(stderr, ERR "--lcas requires --vtop to be enabled\n");
        return 0;
    }

    typedef struct { i32 main_vcpu; i32 helper_vcpu; } lcas_pair_t;
    typedef struct {
        i32 socket_id;
        u32 n_colors;
        u32 n_pairs;
        lcas_pair_t *pairs;
        EvSet ***color_sets;
        u32 *color_counts;
        f64 **tot_avg;
        pthread_t *threads;
        l2c_occ_worker_arg *wargs;
        u32 n_ways;
    } lcas_socket_ctx;

    cpu_topology_t *topo = get_vcpu_topo();
    if (!topo) {
        fprintf(stderr, ERR "failed to detect vCPU topology\n");
        return 0;
    }

    multi_socket_info_t sinfo = get_socket_info(topo);

    // this is a dirty workaround: init_def_args_conf sets num_offsets to 1, but in vset's case for 
    // --vtop + --lcas, we need it by default to be 64, so this is a quick fix, but will later make init_def_args_conf program-dependant
    // so setting -o or -f to 1 by user doesn't trigger this path. but for now, selecting -f 1 or -o 1 would be overwritten..
    if (g_config.num_offsets == 1) g_config.num_offsets = 64;
    if (g_config.num_l2_sets == 0) g_config.num_l2_sets = g_n_uncertain_l2_sets;
    if (g_config.evsets_per_l2 == 1) g_config.evsets_per_l2 = 2 * (u32)sinfo.n_sockets;

    if (g_config.evsets_per_l2 < (u32)sinfo.n_sockets) {
        // setting it to double the base -f because it yeilds more accurate representation of L3 occupancy
        fprintf(stdout, WRN "need at least %d eviction sets per L2 set (-f) for %d sockets. Setting -f to %d\n",
                sinfo.n_sockets, sinfo.n_sockets, sinfo.n_sockets * 2);
        g_config.evsets_per_l2 = (u32)sinfo.n_sockets;
        //free(topo);
        //return 0;
    }

    EvSet ****complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                 g_config.num_offsets, NULL);
    if (!complex) {
        free(topo);
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return 0;
    }

    u32 n_sockets = sinfo.n_sockets;
    lcas_socket_ctx *sockets = _calloc(n_sockets, sizeof(lcas_socket_ctx));
    f64 *ewma = _calloc(n_sockets, sizeof(f64));
    if (!sockets || !ewma) {
        free(topo);
        if (sockets) free(sockets);
        if (ewma) free(ewma);
        return 0;
    }

    u32 sets_per_color_socket = (g_config.evsets_per_l2 + n_sockets - 1) / n_sockets;
    u32 max_sets = g_config.num_offsets * sets_per_color_socket;

    u32 n_ways = 0;

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        return EXIT_FAILURE;
    }

    for (u32 s = 0; s < n_sockets; s++) {
        sockets[s].socket_id = sinfo.sockets[s].socket_id;
        sockets[s].n_colors = g_config.num_l2_sets;
        sockets[s].color_sets = _calloc(g_config.num_l2_sets, sizeof(EvSet**));
        sockets[s].color_counts = _calloc(g_config.num_l2_sets, sizeof(u32));
        sockets[s].tot_avg = _calloc(g_config.num_l2_sets, sizeof(f64*));
        if (!sockets[s].color_sets || !sockets[s].color_counts || !sockets[s].tot_avg)
            goto cleanup;
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            sockets[s].color_sets[c] = _calloc(max_sets, sizeof(EvSet*));
            sockets[s].tot_avg[c] = _calloc(1, sizeof(f64));
            if (!sockets[s].color_sets[c] || !sockets[s].tot_avg[c])
                goto cleanup;
        }
    }

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][c][e];
                if (!ev || ev->size == 0)
                    continue;
                if (n_ways == 0)
                    n_ways = ev->size;
                u32 sock = e % n_sockets;
                u32 idx = sockets[sock].color_counts[c]++;
                sockets[sock].color_sets[c][idx] = ev;
            }
        }
    }

    for (u32 s = 0; s < n_sockets; s++)
        sockets[s].n_ways = n_ways;

    u64 freq = get_cpu_freq_hz();
    u64 cycles_per_us = freq ? freq / 1000000ULL : 2000ULL;

    for (u32 s = 0; s < n_sockets; s++) {
        socket_info_t *si = &sinfo.sockets[s];
        u32 max_pairs = si->vcpu_count / 2;
        lcas_pair_t *pairs = _calloc(max_pairs, sizeof(lcas_pair_t));
        bool used[MAX_CPUS] = {0};
        u32 cnt = 0;
        for (i32 i = 0; i < si->vcpu_count && cnt < max_pairs; i++) {
            if (used[i])
                continue;
            for (i32 j = i + 1; j < si->vcpu_count; j++) {
                if (used[j])
                    continue;
                i32 v1 = si->vcpus[i];
                i32 v2 = si->vcpus[j];
                if (topo->relation_matrix[v1][v2] == CPU_RELATION_SMT)
                    continue;
                pairs[cnt].main_vcpu = v1;
                pairs[cnt].helper_vcpu = v2;
                used[i] = used[j] = true;
                cnt++;
                break;
            }
        }

        sockets[s].n_pairs = cnt;
        sockets[s].pairs = pairs;
        if (sockets[s].n_pairs == 0)
            continue;

        if (sockets[s].n_pairs > sockets[s].n_colors)
            sockets[s].n_pairs = sockets[s].n_colors;

        sockets[s].threads = _calloc(sockets[s].n_pairs, sizeof(pthread_t));
        sockets[s].wargs = _calloc(sockets[s].n_pairs,
                                   sizeof(l2c_occ_worker_arg));
        if (!sockets[s].threads || !sockets[s].wargs)
            goto cleanup;

        u32 base = sockets[s].n_colors / sockets[s].n_pairs;
        u32 extra = sockets[s].n_colors % sockets[s].n_pairs;
        u32 next = 0;
        for (u32 p = 0; p < sockets[s].n_pairs; p++) {
            u32 cntc = base + (p < extra ? 1 : 0);
            sockets[s].wargs[p].wait_us = wait_time_us;
            sockets[s].wargs[p].cycles_per_us = cycles_per_us;
            sockets[s].wargs[p].iterations = 1;
            sockets[s].wargs[p].color_sets = sockets[s].color_sets;
            sockets[s].wargs[p].color_counts = sockets[s].color_counts;
            sockets[s].wargs[p].tot_avg = sockets[s].tot_avg;
            sockets[s].wargs[p].start_color = next;
            sockets[s].wargs[p].num_colors = cntc;
            sockets[s].wargs[p].core_main = pairs[p].main_vcpu;
            sockets[s].wargs[p].core_helper = pairs[p].helper_vcpu;
            next += cntc;
        }
    }
    bool first = true;
    u32 consec_shrink_high = 0;

    printf("Per-socket LLC hotness monitoring (Ctrl+C to stop)\n");
    printf("Wait: %u ms\n", (u32)(wait_time_us / 1000));
    for (u32 s = 0; s < n_sockets; s++)
        printf("Socket %d: --\n", sinfo.sockets[s].socket_id);
    printf("LCAS: preferred socket: []\n");

    while (1) {
        printf("\033[%uA", n_sockets + 2);
        printf("\33[2K\rWait: %u ms\n", (u32)(wait_time_us / 1000));
        u32 high = 0;
        for (u32 s = 0; s < n_sockets; s++) {
            if (sockets[s].n_pairs == 0) {
                printf("\33[2K\rSocket %d: N/A\n", sinfo.sockets[s].socket_id);
                continue;
            }

            for (u32 c = 0; c < sockets[s].n_colors; c++)
                sockets[s].tot_avg[c][0] = 0.0;

            for (u32 p = 0; p < sockets[s].n_pairs; p++) {
                sockets[s].wargs[p].wait_us = wait_time_us;
                pthread_create(&sockets[s].threads[p], NULL, l2c_occ_worker,
                               &sockets[s].wargs[p]);
            }
            for (u32 p = 0; p < sockets[s].n_pairs; p++)
                pthread_join(sockets[s].threads[p], NULL);

            f64 total_e = 0.0, total_l = 0.0;
            for (u32 c = 0; c < sockets[s].n_colors; c++) {
                total_e += sockets[s].tot_avg[c][0];
                total_l += (f64)sockets[s].n_ways * sockets[s].color_counts[c];
            }
            f64 hot = total_l ? total_e / total_l : 0.0;
            if (hot < 0.0) hot = 0.0;
            if (hot > 1.0) hot = 1.0;

            if (first)
                ewma[s] = hot;
            else {
                f64 old = ewma[s];
                f64 alpha = hot > old ? lcas_alpha_rise : lcas_alpha_fall;
                ewma[s] = alpha * old + (1.0 - alpha) * hot;
            }

            if (ewma[s] >= 0.95)
                high++;

            printf("\33[2K\rSocket %d: %6.2f%%\n", sinfo.sockets[s].socket_id,
                   ewma[s] * 100.0);
        }

        if (!fix_wait && wait_time_us > 1000) {
            if (high == n_sockets)
                consec_shrink_high++;
            else
                consec_shrink_high = 0;
            if (consec_shrink_high >= 2) {
                wait_time_us -= 1000;
                consec_shrink_high = 0;
            }
        }
        socket_hot_t tmp[LCAS_MAX_SOCKETS];
        u32 order[LCAS_MAX_SOCKETS];
        for (u32 i = 0; i < n_sockets; i++) {
            tmp[i].socket = sinfo.sockets[i].socket_id;
            tmp[i].hot = ewma[i];
        }
        qsort(tmp, n_sockets, sizeof(socket_hot_t), cmp_cool);
        for (u32 i = 0; i < n_sockets; i++)
            order[i] = tmp[i].socket;

        bool all_same = true;
        i32 lvl0 = lcas_level(tmp[0].hot);
        for (u32 i = 1; i < n_sockets; i++)
            if (lcas_level(tmp[i].hot) != lvl0)
                all_same = false;

        static u32 last_order[LCAS_MAX_SOCKETS];
        static i32 change_cnt = 0;
        static u32 coldest = LCAS_MAX_SOCKETS;
        bool no_pref = all_same;

        // one might run vset before running scx_rusty
        // this would write every scan anyway to update rusty
        update_lcas_map(order, n_sockets, no_pref);
        if (first) {
            coldest = no_pref ? LCAS_MAX_SOCKETS : order[0];
            memcpy(last_order, order, sizeof(u32) * n_sockets);
            printf("\33[2K\rLCAS: preferred socket: ");
            if (no_pref) {
                printf("[]\n");
            } else {
                printf("[");
                for (u32 i = 0; i < n_sockets; i++)
                    printf("%s%u", i ? ", " : "", order[i]);
                printf("]\n");
            }
        } else {
            if (no_pref) {
                change_cnt = 0;
                if (coldest != LCAS_MAX_SOCKETS)
                    coldest = LCAS_MAX_SOCKETS;
                printf("\33[2K\rLCAS: preferred socket: []\n");
            } else {
                u32 cand = order[0];
                i32 cand_lvl = lcas_level(tmp[0].hot);
                i32 cold_lvl = coldest == LCAS_MAX_SOCKETS ? 4 : lcas_level(ewma[coldest]);
                if (cand != coldest && cand_lvl < cold_lvl) {
                    change_cnt++;
                } else {
                    change_cnt = 0;
                }
                if (coldest == LCAS_MAX_SOCKETS || change_cnt >= 3) {
                    coldest = cand;
                    change_cnt = 0;
                    memcpy(last_order, order, sizeof(u32) * n_sockets);
                    update_lcas_map(order, n_sockets, false);
                    printf("\33[2K\rLCAS: preferred socket: [");
                    for (u32 i = 0; i < n_sockets; i++)
                        printf("%s%u", i ? ", " : "", order[i]);
                    printf("]\n");
                } else {
                    if (memcmp(order, last_order, sizeof(u32) * n_sockets)) {
                        memcpy(last_order, order, sizeof(u32) * n_sockets);
                        update_lcas_map(order, n_sockets, false);
                    }
                    printf("\33[2K\rLCAS: preferred socket: [");
                    for (u32 i = 0; i < n_sockets; i++)
                        printf("%s%u", i ? ", " : "", last_order[i]);
                    printf("]\n");
                }
            }
        }
        fflush(stdout);
        usleep(lcas_period_ms * 1000);
        first = false;
    }

cleanup:
    for (u32 s = 0; s < n_sockets; s++) {
        if (sockets[s].threads) free(sockets[s].threads);
        if (sockets[s].wargs) free(sockets[s].wargs);
        if (sockets[s].pairs) free(sockets[s].pairs);
        if (sockets[s].color_sets) {
            for (u32 c = 0; c < g_config.num_l2_sets; c++)
                if (sockets[s].color_sets[c]) free(sockets[s].color_sets[c]);
            free(sockets[s].color_sets);
        }
        if (sockets[s].color_counts) free(sockets[s].color_counts);
        if (sockets[s].tot_avg) {
            for (u32 c = 0; c < g_config.num_l2_sets; c++)
                if (sockets[s].tot_avg[c]) free(sockets[s].tot_avg[c]);
            free(sockets[s].tot_avg);
        }
    }
    free(sockets);
    if (ewma) free(ewma);
    free_evset_complex(complex, g_config.num_offsets, g_config.num_l2_sets,
                       g_config.evsets_per_l2);
    free(topo);
    return 0;
}

#if !XP_EV_PCT
static f64 measure_eviction_rate_single(EvSet *ev, helper_thread_ctrl *hctrl,
                                        u64 cycles_per_us)
{
    if (!ev || !hctrl)
        return 0.0;

    ev->build_conf->hctrl = hctrl;

    flush_array(ev->addrs, ev->size);
    _lfence();
    l3_evset_prime(ev, g_lats.l3_thresh);
    _lfence();

    if (wait_time_us) {
        u64 now = _rdtsc();
        u64 wait_cycles = (u64)wait_time_us * cycles_per_us;
        while ((_rdtsc() - now) < wait_cycles)
            ;
    }

    u32 n_evicted = 0;
    for (i32 i = ev->size - 1; i >= 0; i--) {
        bool valid = false;
        u64 lat = 0;
        for (u32 r = 0; r < 5 && !valid; r++) {
            u32 a1, a2;
            _rdtscp_aux(&a1);
            _lfence();
            lat = _time_maccess(ev->addrs[i]);
            _rdtscp_aux(&a2);
            if (a1 == a2)
                valid = true;
        }
        if (valid && lat >= g_lats.l3_thresh)
            n_evicted++;
    }

    return (f64)n_evicted / (f64)ev->size;
}
#endif // !XP_EV_PCT

#if XP_EV_PCT
static f64 measure_eviction_rate_single_xp(EvSet *ev, helper_thread_ctrl *hctrl,
                                           u64 cycles_per_us, u32 wait_us,
                                           u32 manual_evict)
{
    if (!ev || !hctrl)
        return 0.0;

    ev->build_conf->hctrl = hctrl;

    flush_array(ev->addrs, ev->size);
    _lfence();
    l3_evset_prime(ev, g_lats.l3_thresh);
    _lfence();

    if (wait_us) {
        u64 now = _rdtsc();
        u64 wait_cycles = (u64)wait_us * cycles_per_us;
        while ((_rdtsc() - now) < wait_cycles)
            ;
    }

    if (manual_evict) {
        if (manual_evict >= ev->size) {
            flush_array(ev->addrs, ev->size);
        } else {
            for (u32 i = 0; i < manual_evict && i < ev->size; i++)
                _clflushopt(ev->addrs[i]);
            _lfence();
        }
    }

    u32 n_evicted = 0;
    for (i32 i = ev->size - 1; i >= 0; i--) {
        bool valid = false;
        u64 lat = 0;
        for (u32 r = 0; r < 5 && !valid; r++) {
            u32 a1, a2;
            _rdtscp_aux(&a1);
            _lfence();
            lat = _time_maccess(ev->addrs[i]);
            _rdtscp_aux(&a2);
            if (a1 == a2)
                valid = true;
        }
        if (valid && lat >= g_lats.l3_thresh)
            n_evicted++;
    }

    return (f64)n_evicted / (f64)ev->size;
}
#endif // XP_EV_PCT

static void write_evrate_time_data(f64 **rates, f64 **ewmas,
                                    u32 *waits, u32 n_sockets, u32 n_iters,
                                    socket_info_t *sockets)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    for (u32 s = 0; s < n_sockets; s++) {
        char filename[256];
        snprintf(filename, sizeof(filename),
                 "data/evrate-time-%04d-%02d-%02d-%02d-%02d-%02d-s%d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
                 sockets[s].socket_id);
        if (data_append && strlen(data_append) > 0) {
            strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
            strncat(filename, data_append,
                    sizeof(filename) - strlen(filename) - 1);
        }

        FILE *fp = fopen(filename, "w");
        if (!fp) {
            fprintf(stderr, ERR "failed to create evrate data file: %s\n", filename);
            continue;
        }

        fprintf(fp, "# eviction rate over time data\n");
        fprintf(fp, "# generated by vset on %04d-%02d-%02d %02d:%02d:%02d\n",
                tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        fprintf(fp, "# iterations: %u\n", n_iters);
        fprintf(fp, "# columns: iteration evpct ewma wait_us evpct_per_ms ewma_per_ms\n");

        for (u32 i = 0; i < n_iters; i++) {
            u32 w = waits ? waits[i] : wait_time_us;
            f64 evpct = rates[s][i];
            f64 ew = ewmas[s][i];
            f64 evpct_ms = w ? evpct * 100000.0 / w : 0.0;
            f64 ewma_ms = w ? ew * 100000.0 / w : 0.0;
            fprintf(fp, "%u %.2f %.2f %u %.2f %.2f\n", i, evpct * 100.0,
                    ew * 100.0, w, evpct_ms, ewma_ms);
        }

        fclose(fp);
        printf(INFO "evrate data written to: %s\n", filename);
#if XP_EV_PCT
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "python3 ../scripts/plot_eviction_pct_xp.py %s", filename);
        i32 rc = system(cmd);
        if (rc == -1)
            fprintf(stderr, ERR "failed to run plot script for %s\n", filename);
#endif
    }
}

#if XP_EV_PCT
u32 monitor_eviction_pct_single(u32 iterations)
{
    if (!vtop) {
        fprintf(stderr, ERR "--lcas requires --vtop to be enabled\n");
        return 0;
    }

    cpu_topology_t *topo = get_vcpu_topo();
    if (!topo)
        return 0;

    multi_socket_info_t sinfo = get_socket_info(topo);

    EvSet **ev_arr = build_single_l3_evset();
    if (!ev_arr) {
        free(topo);
        return 0;
    }

    EvSet *ev = ev_arr[0];
    if (!ev || ev->size == 0) {
        free(ev_arr);
        free(topo);
        return 0;
    }

    u32 n_sockets = sinfo.n_sockets;
    helper_thread_ctrl *hctrls = _calloc(n_sockets, sizeof(*hctrls));
    i32 *main_vcpu = _calloc(n_sockets, sizeof(i32));
    if (!hctrls || !main_vcpu) {
        free(ev_arr);
        free(topo);
        if (hctrls) free(hctrls);
        if (main_vcpu) free(main_vcpu);
        return 0;
    }

    for (u32 s = 0; s < n_sockets; s++) {
        socket_info_t *si = &sinfo.sockets[s];
        i32 mv = -1, hv = -1;
        select_vcpu_pair(si, topo, &mv, &hv);
        main_vcpu[s] = mv;
        if (mv != -1 && hv != -1) {
            start_helper_thread_pinned(&hctrls[s], hv);
        }
    }

    u64 freq = get_cpu_freq_hz();
    u64 cycles_per_us = freq ? freq / 1000000ULL : 2000ULL;
    const u32 total_secs = 90;

    f64 *ewma = _calloc(n_sockets, sizeof(f64));
    f64 **hist = NULL, **raw = NULL;
    if (graph_mode && graph_type == GRAPH_EVRATE_TIME) {
        hist = _calloc(n_sockets, sizeof(f64 *));
        raw = _calloc(n_sockets, sizeof(f64 *));
        for (u32 s = 0; s < n_sockets; s++) {
            hist[s] = _calloc(total_secs, sizeof(f64));
            raw[s] = _calloc(total_secs, sizeof(f64));
        }
    }

    const char *stage_names[3] = {"Manual", "Idle", "nginx"};

    printf("Per-socket LLC eviction rate monitoring (experiment)\n");
    for (u32 s = 0; s < n_sockets; s++)
        printf("Socket %d: --\n", sinfo.sockets[s].socket_id);
    printf("0/90s [%s]\n", stage_names[0]);

    for (u32 sec = 0; sec < total_secs; sec++) {
        printf("\033[%uA", n_sockets + 1);
        u32 stage = sec / 30;
        u32 wait_us = (stage == 0) ? 0 : 7000; // windowless and controlled eviction for first stage
        u32 manual = 0;
        if (stage == 0) {
            u32 sub = sec % 30;
            if (sub < 6) manual = 2;
            else if (sub < 12) manual = 4;
            else if (sub < 18) manual = 6;
            else if (sub < 24) manual = 8;
            else manual = ev->size;
        }

        for (u32 s = 0; s < n_sockets; s++) {
            if (main_vcpu[s] == -1) {
                printf("\33[2K\rSocket %d: N/A\n", sinfo.sockets[s].socket_id);
                continue;
            }
            set_cpu_affinity(main_vcpu[s]);
            f64 rate = measure_eviction_rate_single_xp(ev, &hctrls[s], cycles_per_us, wait_us, manual);
            if (sec == 0)
                ewma[s] = rate;
            else {
                f64 old = ewma[s];
                f64 alpha = rate > old ? lcas_alpha_rise : lcas_alpha_fall;
                ewma[s] = alpha * old + (1.0 - alpha) * rate;
            }
            if (raw) {
                raw[s][sec] = rate;
                hist[s][sec] = ewma[s];
            }
            printf("\33[2K\rSocket %d: %6.2f%%\n", sinfo.sockets[s].socket_id,
                   ewma[s] * 100.0);
        }
        printf("\33[2K\r%u/90s [%s]\n", sec + 1, stage_names[stage]);
        fflush(stdout);
        sleep(1);
    }

    for (u32 s = 0; s < n_sockets; s++)
        stop_helper_thread(&hctrls[s]);

    if (raw && hist)
        write_evrate_time_data(raw, hist, NULL, n_sockets, total_secs,
                               sinfo.sockets);

    for (u32 s = 0; s < n_sockets; s++) {
        if (raw) free(raw[s]);
        if (hist) free(hist[s]);
    }
    if (raw) free(raw);
    if (hist) free(hist);
    if (ewma) free(ewma);
    if (hctrls) free(hctrls);
    if (main_vcpu) free(main_vcpu);

    free(ev->addrs);
    free(ev);
    free(ev_arr);
    free(topo);

    return 1;
}
#else
u32 monitor_eviction_pct_single(u32 iterations)
{
    if (!vtop) {
        fprintf(stderr, ERR "--lcas requires --vtop to be enabled\n");
        return 0;
    }

    cpu_topology_t *topo = get_vcpu_topo();
    if (!topo)
        return 0;

    multi_socket_info_t sinfo = get_socket_info(topo);

    EvSet **ev_arr = build_single_l3_evset();
    if (!ev_arr) {
        free(topo);
        return 0;
    }

    EvSet *ev = ev_arr[0];
    if (!ev || ev->size == 0) {
        free(ev_arr);
        free(topo);
        return 0;
    }

    u32 n_sockets = sinfo.n_sockets;
    helper_thread_ctrl *hctrls = _calloc(n_sockets, sizeof(*hctrls));
    i32 *main_vcpu = _calloc(n_sockets, sizeof(i32));
    if (!hctrls || !main_vcpu) {
        free(ev_arr);
        free(topo);
        if (hctrls) free(hctrls);
        if (main_vcpu) free(main_vcpu);
        return 0;
    }

    for (u32 s = 0; s < n_sockets; s++) {
        socket_info_t *si = &sinfo.sockets[s];
        i32 mv = -1, hv = -1;
        select_vcpu_pair(si, topo, &mv, &hv);
        main_vcpu[s] = mv;
        if (mv != -1 && hv != -1) {
            start_helper_thread_pinned(&hctrls[s], hv);
        }
    }

    u64 freq = get_cpu_freq_hz();
    u64 cycles_per_us = freq ? freq / 1000000ULL : 2000ULL;

    f64 *ewma = _calloc(n_sockets, sizeof(f64));
    f64 **hist = NULL, **raw = NULL;
    if (graph_mode && graph_type == GRAPH_EVRATE_TIME && iterations > 0) {
        hist = _calloc(n_sockets, sizeof(f64 *));
        raw = _calloc(n_sockets, sizeof(f64 *));
        for (u32 s = 0; s < n_sockets; s++) {
            hist[s] = _calloc(iterations, sizeof(f64));
            raw[s] = _calloc(iterations, sizeof(f64));
        }
    }

    printf("Per-socket LLC eviction rate monitoring (Ctrl+C to stop)\n");
    for (u32 s = 0; s < n_sockets; s++)
        printf("Socket %d: --\n", sinfo.sockets[s].socket_id);

    bool first = true;
    u32 it = 0;
    while (iterations == 0 || it < iterations) {
        printf("\033[%uA", n_sockets);
        for (u32 s = 0; s < n_sockets; s++) {
            if (main_vcpu[s] == -1) {
                printf("\33[2K\rSocket %d: N/A\n", sinfo.sockets[s].socket_id);
                continue;
            }

            set_cpu_affinity(main_vcpu[s]);
            f64 rate = measure_eviction_rate_single(ev, &hctrls[s], cycles_per_us);

            if (first)
                ewma[s] = rate;
            else {
                f64 old = ewma[s];
                f64 alpha = rate > old ? lcas_alpha_rise : lcas_alpha_fall;
                ewma[s] = alpha * old + (1.0 - alpha) * rate;
            }

            if (raw) {
                raw[s][it] = rate;
                hist[s][it] = ewma[s];
            }

            printf("\33[2K\rSocket %d: %6.2f%%\n", sinfo.sockets[s].socket_id,
                   ewma[s] * 100.0);
        }
        fflush(stdout);

        if (iterations && it + 1 >= iterations)
            break;

        usleep(lcas_period_ms * 1000);
        first = false;
        it++;
    }

    for (u32 s = 0; s < n_sockets; s++)
        stop_helper_thread(&hctrls[s]);

    if (raw && hist)
        write_evrate_time_data(raw, hist, NULL, n_sockets, iterations,
                               sinfo.sockets);

    for (u32 s = 0; s < n_sockets; s++) {
        if (raw) free(raw[s]);
        if (hist) free(hist[s]);
    }
    if (raw) free(raw);
    if (hist) free(hist);
    if (ewma) free(ewma);
    if (hctrls) free(hctrls);
    if (main_vcpu) free(main_vcpu);

    free(ev->addrs);
    free(ev);
    free(ev_arr);
    free(topo);

    return 1;
}
#endif // XP_EV_PCT

u32 monitor_eviction_rate_multi(u32 iterations)
{
    if (!vtop) {
        fprintf(stderr, ERR "--lcas requires --vtop to be enabled\n");
        return 0;
    }

    cpu_topology_t *topo = get_vcpu_topo();
    if (!topo)
        return 0;

    multi_socket_info_t sinfo = get_socket_info(topo);

    EvSet ****complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                 g_config.num_offsets, NULL);
    if (!complex) {
        free(topo);
        return 0;
    }

    typedef struct { i32 main_vcpu; i32 helper_vcpu; } lcas_pair_t;
    typedef struct {
        i32 socket_id;
        u32 n_colors;
        u32 n_pairs;
        lcas_pair_t *pairs;
        EvSet ***color_sets;
        u32 *color_counts;
        f64 **tot_avg;
        pthread_t *threads;
        l2c_occ_worker_arg *wargs;
        u32 n_ways;
    } cas_socket_ctx;

    u32 n_sockets = sinfo.n_sockets;
    cas_socket_ctx *sockets = _calloc(n_sockets, sizeof(cas_socket_ctx));
    f64 *ewma = _calloc(n_sockets, sizeof(f64));
    f64 **raw = NULL, **hist = NULL;
    u32 *wait_hist = NULL;
    if (!sockets || !ewma) {
        if (sockets) free(sockets);
        if (ewma) free(ewma);
        free(topo);
        return 0;
    }

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        free(ewma);
        free(sockets);
        free(topo);
        return 0;
    }

    if (graph_mode && graph_type == GRAPH_EVRATE_TIME && iterations > 0) {
        raw = _calloc(n_sockets, sizeof(f64*));
        hist = _calloc(n_sockets, sizeof(f64*));
        wait_hist = _calloc(iterations, sizeof(u32));
        if (!raw || !hist || !wait_hist) {
            if (raw) free(raw);
            if (hist) free(hist);
            if (wait_hist) free(wait_hist);
            free(ewma);
            free(sockets);
            free(topo);
            return 0;
        }
        for (u32 s = 0; s < n_sockets; s++) {
            raw[s] = _calloc(iterations, sizeof(f64));
            hist[s] = _calloc(iterations, sizeof(f64));
        }
    }

    u32 sets_per_color_socket = (g_config.evsets_per_l2 + n_sockets - 1) / n_sockets;
    u32 max_sets = g_config.num_offsets * sets_per_color_socket;

    u32 n_ways = 0;

    for (u32 s = 0; s < n_sockets; s++) {
        sockets[s].socket_id = sinfo.sockets[s].socket_id;
        sockets[s].n_colors = g_config.num_l2_sets;
        sockets[s].color_sets = _calloc(g_config.num_l2_sets, sizeof(EvSet**));
        sockets[s].color_counts = _calloc(g_config.num_l2_sets, sizeof(u32));
        sockets[s].tot_avg = _calloc(g_config.num_l2_sets, sizeof(f64*));
        if (!sockets[s].color_sets || !sockets[s].color_counts || !sockets[s].tot_avg)
            goto cleanup;
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            sockets[s].color_sets[c] = _calloc(max_sets, sizeof(EvSet*));
            sockets[s].tot_avg[c] = _calloc(1, sizeof(f64));
            if (!sockets[s].color_sets[c] || !sockets[s].tot_avg[c])
                goto cleanup;
        }
    }

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][c][e];
                if (!ev || ev->size == 0)
                    continue;
                if (n_ways == 0)
                    n_ways = ev->size;
                u32 sock = e % n_sockets;
                u32 idx = sockets[sock].color_counts[c]++;
                sockets[sock].color_sets[c][idx] = ev;
            }
        }
    }

    for (u32 s = 0; s < n_sockets; s++)
        sockets[s].n_ways = n_ways;

    u64 freq = get_cpu_freq_hz();
    u64 cycles_per_us = freq ? freq / 1000000ULL : 2000ULL;

    for (u32 s = 0; s < n_sockets; s++) {
        socket_info_t *si = &sinfo.sockets[s];
        u32 max_pairs = si->vcpu_count / 2;
        lcas_pair_t *pairs = _calloc(max_pairs, sizeof(lcas_pair_t));
        bool used[MAX_CPUS] = {0};
        u32 cnt = 0;
        for (i32 i = 0; i < si->vcpu_count && cnt < max_pairs; i++) {
            if (used[i])
                continue;
            for (i32 j = i + 1; j < si->vcpu_count; j++) {
                if (used[j])
                    continue;
                i32 v1 = si->vcpus[i];
                i32 v2 = si->vcpus[j];
                if (topo->relation_matrix[v1][v2] == CPU_RELATION_SMT)
                    continue;
                pairs[cnt].main_vcpu = v1;
                pairs[cnt].helper_vcpu = v2;
                used[i] = used[j] = true;
                cnt++;
                break;
            }
        }

        sockets[s].n_pairs = cnt;
        sockets[s].pairs = pairs;
        if (sockets[s].n_pairs == 0)
            continue;

        sockets[s].threads = _calloc(sockets[s].n_pairs, sizeof(pthread_t));
        sockets[s].wargs = _calloc(sockets[s].n_pairs,
                                   sizeof(l2c_occ_worker_arg));
        if (!sockets[s].threads || !sockets[s].wargs)
            goto cleanup;

        u32 base = sockets[s].n_colors / sockets[s].n_pairs;
        u32 extra = sockets[s].n_colors % sockets[s].n_pairs;
        u32 next = 0;
        for (u32 p = 0; p < sockets[s].n_pairs; p++) {
            u32 cntc = base + (p < extra ? 1 : 0);
            sockets[s].wargs[p].wait_us = wait_time_us;
            sockets[s].wargs[p].cycles_per_us = cycles_per_us;
            sockets[s].wargs[p].iterations = 1;
            sockets[s].wargs[p].color_sets = sockets[s].color_sets;
            sockets[s].wargs[p].color_counts = sockets[s].color_counts;
            sockets[s].wargs[p].tot_avg = sockets[s].tot_avg;
            sockets[s].wargs[p].start_color = next;
            sockets[s].wargs[p].num_colors = cntc;
            sockets[s].wargs[p].core_main = pairs[p].main_vcpu;
            sockets[s].wargs[p].core_helper = pairs[p].helper_vcpu;
            next += cntc;
        }
    }
    bool first = true;
    u32 consec_shrink_high = 0;
    for (u32 it = 0; iterations == 0 || it < iterations; it++) {
        u32 rows = n_sockets * 2 + 1;
        if (iterations > 0)
            rows++;
        printf("\033[%uA", rows);
        printf("\33[2K\rWait: %u ms\n", (u32)(wait_time_us / 1000));
        if (iterations > 0) {
            u32 remaining_ms = (iterations - it) * lcas_period_ms;
            printf("\33[2K\rRemaining: %u s\n", remaining_ms / 1000);
        }
        u32 n_hot_socket = 0;
        u32 n_cold_socket = 0;
        for (u32 s = 0; s < n_sockets; s++) {
            if (sockets[s].n_pairs == 0) {
                printf("\33[2K\rSocket %d: N/A\n", sinfo.sockets[s].socket_id);
                printf("\33[2K\rRate/ms: N/A\n");
                continue;
            }

            for (u32 c = 0; c < sockets[s].n_colors; c++)
                sockets[s].tot_avg[c][0] = 0.0;

            for (u32 p = 0; p < sockets[s].n_pairs; p++) {
                sockets[s].wargs[p].wait_us = wait_time_us;
                pthread_create(&sockets[s].threads[p], NULL, l2c_occ_worker,
                               &sockets[s].wargs[p]);
            }
            for (u32 p = 0; p < sockets[s].n_pairs; p++)
                pthread_join(sockets[s].threads[p], NULL);

            f64 total_e = 0.0, total_l = 0.0;
            for (u32 c = 0; c < sockets[s].n_colors; c++) {
                total_e += sockets[s].tot_avg[c][0];
                total_l += (f64)sockets[s].n_ways * sockets[s].color_counts[c];
            }
            f64 evpct = total_l ? total_e / total_l : 0.0;
            if (evpct < 0.0) evpct = 0.0;
            if (evpct > 1.0) evpct = 1.0;

            if (first)
                ewma[s] = evpct;
            else {
                f64 old = ewma[s];
                f64 alpha = evpct > old ? lcas_alpha_rise : lcas_alpha_fall;
                ewma[s] = alpha * old + (1.0 - alpha) * evpct;
            }

            if (raw) {
                raw[s][it] = evpct;
                hist[s][it] = ewma[s];
            }

            if (ewma[s] >= 0.95)
                n_hot_socket++;

            if (ewma[s] <= 0.20)
                n_cold_socket++;

            printf("\33[2K\rSocket %d: %6.2f%%\n", sinfo.sockets[s].socket_id,
                   ewma[s] * 100.0);
            f64 rate_ms = wait_time_us ? ewma[s] * 100000.0 / wait_time_us : 0.0;
            printf("\33[2K\rRate/ms: %6.2f%%\n", rate_ms);
        }
        if (wait_hist)
            wait_hist[it] = (u32)wait_time_us;
        fflush(stdout);

        if (!fix_wait && wait_time_us > 1000) {
            if (n_hot_socket == n_sockets)
                consec_shrink_high++;
            else if (n_cold_socket == n_sockets) // once is enough
                wait_time_us = og_wait_time_us; // to bounce back to original -w
            else
                consec_shrink_high = 0;
            if (consec_shrink_high >= 2) { // twice
                wait_time_us -= 1000;
                consec_shrink_high = 0;
            }
        }

        if (iterations && it + 1 >= iterations)
            break;

        usleep(lcas_period_ms * 1000);
        first = false;
    }

    if (raw && hist)
        write_evrate_time_data(raw, hist, wait_hist, n_sockets, iterations,
                               sinfo.sockets);

cleanup:
    if (raw) {
        for (u32 s = 0; s < n_sockets; s++) {
            if (raw[s]) free(raw[s]);
            if (hist && hist[s]) free(hist[s]);
        }
        free(raw);
        if (hist) free(hist);
    }
    if (wait_hist) free(wait_hist);
    if (ewma) free(ewma);
    if (sockets) {
        for (u32 s = 0; s < n_sockets; s++) {
            if (sockets[s].threads) free(sockets[s].threads);
            if (sockets[s].wargs) free(sockets[s].wargs);
            if (sockets[s].pairs) free(sockets[s].pairs);
            if (sockets[s].color_sets) {
                for (u32 c = 0; c < g_config.num_l2_sets; c++)
                    if (sockets[s].color_sets[c]) free(sockets[s].color_sets[c]);
                free(sockets[s].color_sets);
            }
            if (sockets[s].color_counts) free(sockets[s].color_counts);
            if (sockets[s].tot_avg) {
                for (u32 c = 0; c < g_config.num_l2_sets; c++)
                    if (sockets[s].tot_avg[c]) free(sockets[s].tot_avg[c]);
                free(sockets[s].tot_avg);
            }
        }
        free(sockets);
    }

    free_evset_complex(complex, g_config.num_offsets, g_config.num_l2_sets,
                       g_config.evsets_per_l2);

    free(topo);
    return 1;
}

f64 estimate_heatmap_wait_time(void)
{
    u32 n_time_slots = (heatmap_max_time_us / heatmap_time_step_us) + 1;
    
    // n*(n-1)/2
    // since we go from slot 0 to slot (n_time_slots-1)
    u64 sum_of_slots = (u64)(n_time_slots - 1) * n_time_slots / 2;
    
    u64 total_wait_us = (u64)heatmap_time_step_us * HEATMAP_SAMPLES_PER_SLOT * sum_of_slots;
    
    // to seconds
    f64 total_wait_seconds = (f64)total_wait_us / 1000000.0;
    
    if (verbose > 1) {
        printf(V2 "time calculation details:\n");
        printf(V2 "  n_time_slots: %u (0 to %u us in %u us steps)\n",
               n_time_slots, heatmap_max_time_us, heatmap_time_step_us);
        printf(V2 "  samples per slot: %u\n", HEATMAP_SAMPLES_PER_SLOT);
        printf(V2 "  sum of slot numbers: %lu\n", sum_of_slots);
        printf(V2 "  total wait time: %lu microseconds\n", total_wait_us);
    }
    
    return total_wait_seconds;
}

f64 estimate_heatmap_runtime(u32 n_time_slots, u32 batch_prime_us,
                             u32 single_prime_us)
{
    u64 total_us = 0;

    for (u32 t = 0; t < n_time_slots; t++) {
        u32 current_wait_us = t * heatmap_time_step_us;
        if (current_wait_us < batch_prime_us) {
            total_us += HEATMAP_SAMPLES_PER_SLOT *
                        (single_prime_us + current_wait_us + single_prime_us);
        } else {
            u32 wait_us = (current_wait_us > batch_prime_us) ?
                          (current_wait_us - batch_prime_us) : 0;
            total_us += batch_prime_us + wait_us + batch_prime_us;
        }
    }

    return (f64)total_us / 1000000.0;
}

f64 estimate_l2color_runtime(u32 n_colors, u32 iterations, u32 wait_us)
{
    f64 per_iteration_seconds = (f64)wait_us / 1000000.0;
    f64 sleep_seconds = (f64)scan_period_ms / 1000.0;
    f64 prime_probe_overhead = 0.001 * n_colors;
    f64 total_per_iteration = per_iteration_seconds + prime_probe_overhead + sleep_seconds;
    f64 total_seconds = total_per_iteration * iterations;
    return total_seconds;
}

static void write_l2dist_data(f64 **pct, u32 host_colors, u32 guest_colors)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "data/l2dist-%04d-%02d-%02d-%02d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append, sizeof(filename) - strlen(filename) - 1);
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create l2dist data file: %s\n", filename);
        return;
    }

    fprintf(fp, "# l2 guest color distribution\n");
    fprintf(fp, "# generated by vset on %04d-%02d-%02d %02d:%02d:%02d\n",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    fprintf(fp, "# host_colors: %u\n", host_colors);
    fprintf(fp, "# guest_colors: %u\n", guest_colors);
    fprintf(fp, "# columns: host_color");
    for (u32 g = 0; g < guest_colors; g++)
        fprintf(fp, " g%u_pct", g);
    fprintf(fp, "\n");

    for (u32 h = 0; h < host_colors; h++) {
        fprintf(fp, "%u", h);
        for (u32 g = 0; g < guest_colors; g++)
            fprintf(fp, " %.2f", pct[h][g]);
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf(INFO "l2dist data written to: %s\n", filename);
    char command[512];
    sprintf(command, "python3 ../scripts/plot_l2color_distribution.py ./%s", filename);
    i32 r = system(command);
    if (r < 0) fprintf(stderr, ERR "failed to execute command!\n");
}

void graph_l2color_distribution(void)
{
    u32 max_unc_sets = g_n_uncertain_l2_sets;

    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = 16;
    if (g_config.evsets_per_l2 == 1)
        g_config.evsets_per_l2 = 4;
    if (g_config.num_offsets == 1)
        g_config.num_offsets = 64;

    if (g_config.num_l2_sets > max_unc_sets)
        g_config.num_l2_sets = max_unc_sets;

    EvSet ****complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                 g_config.num_offsets, NULL);
    if (!complex) {
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return;
    }

    u32 host_colors = g_config.num_l2_sets;
    u32 guest_colors = max_unc_sets;

    u64 **counts = _calloc(host_colors, sizeof(u64*));
    u64 *totals = _calloc(host_colors, sizeof(u64));
    for (u32 h = 0; h < host_colors; h++)
        counts[h] = _calloc(guest_colors, sizeof(u64));

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 h = 0; h < host_colors; h++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][h][e];
                if (!ev || ev->size == 0)
                    continue;
                for (u32 i = 0; i < ev->size; i++) {
                    u32 gc = va_to_l2color(ev->addrs[i]);
                    if (gc < guest_colors)
                        counts[h][gc]++;
                    totals[h]++;
                }
            }
        }
    }

    f64 **pct = _calloc(host_colors, sizeof(f64*));
    for (u32 h = 0; h < host_colors; h++) {
        pct[h] = _calloc(guest_colors, sizeof(f64));
        for (u32 g = 0; g < guest_colors; g++) {
            if (totals[h])
                pct[h][g] = (f64)counts[h][g] / (f64)totals[h] * 100.0;
            else
                pct[h][g] = 0.0;
        }
    }

    write_l2dist_data(pct, host_colors, guest_colors);

    for (u32 h = 0; h < host_colors; h++) {
        if (counts[h]) free(counts[h]);
        if (pct[h]) free(pct[h]);
    }
    free(counts);
    free(pct);
    if (totals) free(totals);

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 h = 0; h < host_colors; h++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][h][e];
                if (ev) {
                    if (ev->addrs) free(ev->addrs);
                    if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                        ev->build_conf != &def_l2_build_conf)
                        free(ev->build_conf);
                    free(ev);
                }
            }
            free(complex[off][h]);
        }
        free(complex[off]);
    }
    free(complex);
}

i32 check_mem_remap_cheat(void)
{ 
    u64 start_time = time(NULL);
    u32 n_remaped = 0,
        n_check = 0,
        check_wait = 5; // wait between checks in sec

    EvSet*** l2evsets = NULL;
    u32 n_ret = 5;
    for (u32 r = 0; r < n_ret; r++) {
        l2evsets = build_l2_evset(1);
        if (!l2evsets) {
            if (verbose) {
                printf(WRN "Failed try %u/%u. Retrying.\n", r + 1, n_ret);
            }
            sleep(2);
            continue;
        }
        break;
    }

    if (!l2evsets) return -1;

    EvSet* l2ev = l2evsets[0][0];

    while (time(NULL) - start_time <= check_remap) {
        n_check += 1;

        printf(INFO "Check number %u | Num remaps: %u | Next check in %u seconds\n", 
               n_check, n_remaped, check_wait);
        
        u32 t_hpa = va_to_hpa(l2ev->target_addr); // target addr HPA
        u32 t_hpa_l2_sib = cache_get_sib(t_hpa, &l2_info);
        u32 w = n_digits(l2ev->size); // width
        printf("  Target VA: %p -> HPA: 0x%lx (L2 SIB 0x%x)\n", 
                l2ev->target_addr, va_to_hpa(l2ev->target_addr), t_hpa_l2_sib);

        bool remap_detected = false;
        bool check_remap = false;
        for (u16 i = 0; i < 2; i++) {
            if (test_eviction(l2ev->target_addr, l2ev->addrs, 
                            l2ev->size, l2ev->build_conf) == FAIL) {

                check_remap = true;
            } else { // both cases need to succeed (double-check)
                check_remap = false;
                break;
            }
        }

        if (check_remap) {
            for (u32 i = 0; i < l2ev->size; i++)  {
                u64 hpa = va_to_hpa(l2ev->addrs[i]);
                u32 hpa_l2_sib = cache_get_sib(hpa, &l2_info);

                if (hpa_l2_sib != t_hpa_l2_sib) {
                    n_remaped += 1;
                    remap_detected = true;
                    printf(WRN "Remap occured. Reconstructing evset.\n");
                    printf("  [%*u]: %p -> HPA 0x%lx [%s]\n", 
                            w, i, l2ev->addrs[i], hpa,
                            RED "Bad L2 SIB " RST);
                    break;
                }

                printf("  [%*u]: %p -> HPA 0x%lx [%s]\n", 
                        w, i, l2ev->addrs[i], hpa,
                        (hpa_l2_sib == t_hpa_l2_sib) ?
                        GRN "L2 SIB match" RST:
                        RED "Bad L2 SIB " RST);
            }
            if (!remap_detected) 
                puts(ERR "False alarm by test_eviction");
            else
                puts(SUC "Correct detection by test_eviction");
        } 

        if (remap_detected) {
            // cleanup prev evsets
            if (l2evsets) {
                for (u32 o = 0; o < PAGE_SIZE / l2_info.cl_size; o++) {
                    if (l2evsets[o] && l2evsets[o][0]) {
                        if (l2evsets[o][0]->addrs) {
                            free(l2evsets[o][0]->addrs);
                        }
                        free(l2evsets[o][0]);
                    }
                    free(l2evsets[o]);
                }
                free(l2evsets);
                l2evsets = NULL;
                l2ev = NULL;
            }

            // new evset
            for (u32 r = 0; r < n_ret; r++) {
                l2evsets = build_l2_evset(1);
                if (!l2evsets) {
                    if (verbose) {
                        printf(WRN "Failed try %u/%u. Retrying.\n", r + 1, n_ret);
                    }
                    sleep(2);
                    continue;
                }
                l2ev = l2evsets[0][0];
                printf(INFO "Waiting for next check.\n");
                break;
            }
            
            if (!l2evsets) {
                printf(ERR "Failed to rebuild evset after remap\n");
                return -1;
            }
        }

        sleep(check_wait);
    }

    // final cleanup
    if (l2evsets) {
        for (u32 o = 0; o < PAGE_SIZE / l2_info.cl_size; o++) {
            if (l2evsets[o] && l2evsets[o][0]) {
                if (l2evsets[o][0]->addrs) {
                    free(l2evsets[o][0]->addrs);
                }
                free(l2evsets[o][0]);
            }
            free(l2evsets[o]);
        }
        free(l2evsets);
    }

    return n_remaped;
}

void perf_prime_probe(void)
{
    u32 iterations = PERF_PP_ITERS;
    // we want perf mode should run continuously w/o additional waits
    // used for live/graph modes. temp disabling the global scan period
    // so l2c_occ_worker doesn't sleep between iterations.
    u32 saved_period = scan_period_ms;
    scan_period_ms = 0;

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        scan_period_ms = saved_period;
        return;
    }

    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = g_n_uncertain_l2_sets;
    if (g_config.num_offsets == 0)
        g_config.num_offsets = 64;
    if (g_config.evsets_per_l2 == 0)
        g_config.evsets_per_l2 = 2;

    EvSet ****l3_complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                    g_config.num_offsets, NULL);
    if (!l3_complex) {
        fprintf(stderr, ERR "failed to build eviction sets\n");
        scan_period_ms = saved_period;
        return;
    }

    u32 n_colors = g_config.num_l2_sets;
    u32 max_sets_per_color = g_config.num_offsets * g_config.evsets_per_l2;

    EvSet ***color_sets = _calloc(n_colors, sizeof(EvSet**));
    u32 *color_counts = _calloc(n_colors, sizeof(u32));
    for (u32 c = 0; c < n_colors; c++)
        color_sets[c] = _calloc(max_sets_per_color, sizeof(EvSet*));

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < n_colors; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = l3_complex[off][c][e];
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

    f64 **dummy_avg = _calloc(n_colors, sizeof(f64*));
    for (u32 c = 0; c < n_colors; c++)
        dummy_avg[c] = _calloc(iterations, sizeof(f64));

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
    l2c_occ_worker_arg *wargs = _calloc(n_pairs, sizeof(l2c_occ_worker_arg));
    u64 **prime_data = _calloc(n_pairs, sizeof(u64*));
    u64 **probe_data = _calloc(n_pairs, sizeof(u64*));

    for (u32 i = 0; i < n_pairs; i++) {
        prime_data[i] = _calloc(iterations, sizeof(u64));
        probe_data[i] = _calloc(iterations, sizeof(u64));
    }

    u32 base = n_colors / n_pairs;
    u32 extra = n_colors % n_pairs;
    u32 next_start = 0;

    for (u32 i = 0; i < n_pairs; i++) {
        u32 count = base + (i < extra ? 1 : 0);

        wargs[i].wait_us = wait_time_us;
        wargs[i].cycles_per_us = cycles_per_us;
        wargs[i].iterations = iterations;
        wargs[i].color_sets = color_sets;
        wargs[i].color_counts = color_counts;
        wargs[i].tot_avg = dummy_avg;
        wargs[i].start_color = next_start;
        wargs[i].num_colors = count;
        wargs[i].core_main = i * 2;
        wargs[i].core_helper = i * 2 + 1;
        wargs[i].prime_times = prime_data[i];
        wargs[i].probe_times = probe_data[i];

        next_start += count;

        pthread_create(&threads[i], NULL, l2c_occ_worker, &wargs[i]);
    }

    for (u32 i = 0; i < n_pairs; i++)
        pthread_join(threads[i], NULL);

    // print vals with comma as delimiter
    // comma-separated vals could easily be plugged into
    // online calcs to get mean or median + the stddev
    f64 tot_p = 0.0, tot_pr = 0.0;

    puts("Prime times (ms):");
    for (u32 it = 0; it < iterations; it++) {
        f64 prime = 0.0;
        for (u32 w = 0; w < n_pairs; w++)
            prime += prime_data[w][it];
        prime /= n_pairs;
        tot_p += prime;
        printf("%.3f, ", prime / 1000.0);
    }
    puts("");

    puts("Probes times (ms):");
    for (u32 it = 0; it < iterations; it++) {
        f64 probe = 0.0;
        for (u32 w = 0; w < n_pairs; w++)
            probe += probe_data[w][it];
        probe /= n_pairs;
        tot_pr += probe;
        printf("%.3f, ", probe / 1000.0);
    }
    puts("");

    printf("Avg prime %.3f ms | Avg probe %.3f ms\n",
           tot_p / iterations / 1000.0, tot_pr / iterations / 1000.0);

    if (threads) free(threads);
    if (wargs) free(wargs);
    if (prime_data) {
        for (u32 i = 0; i < n_pairs; i++)
            if (prime_data[i]) free(prime_data[i]);
        free(prime_data);
    }
    if (probe_data) {
        for (u32 i = 0; i < n_pairs; i++)
            if (probe_data[i]) free(probe_data[i]);
        free(probe_data);
    }

    if (dummy_avg) {
        for (u32 c = 0; c < n_colors; c++)
            if (dummy_avg[c]) free(dummy_avg[c]);
        free(dummy_avg);
    }

    if (color_sets) {
        for (u32 c = 0; c < n_colors; c++)
            if (color_sets[c]) free(color_sets[c]);
        free(color_sets);
    }
    if (color_counts) free(color_counts);

    if (l3_complex) {
        for (u32 off = 0; off < g_config.num_offsets; off++) {
            for (u32 c = 0; c < n_colors; c++) {
                for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                    EvSet *ev = l3_complex[off][c][e];
                    if (ev) {
                        if (ev->addrs) free(ev->addrs);
                        if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                            ev->build_conf != &def_l2_build_conf)
                            free(ev->build_conf);
                        free(ev);
                    }
                }
                free(l3_complex[off][c]);
            }
            free(l3_complex[off]);
        }
        free(l3_complex);
    }

    /* restore original scan period */
    scan_period_ms = saved_period;
}

i32 fraction_check(void)
{
    u32 total_l3_colors = 1u << l3_info.unknown_sib;
    u32 per_l2_colors = 1u;
    if (l3_info.unknown_sib > l2_info.unknown_sib)
        per_l2_colors = 1u << (l3_info.unknown_sib - l2_info.unknown_sib);

    EvSet ****complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                                  g_config.num_offsets, NULL);
    if (!complex) {
        fprintf(stderr, ERR "failed to build eviction sets\n");
        return EXIT_FAILURE;
    }

    //  e.g. in illustration below, len(a) == unq_l3sib
    u32 unq_l3sib = l3_info.n_set_idx_bits - l2_info.n_set_idx_bits;

    // number of L3 sets at each page offset x L2 color combination
    // on our system with 1024 L2 set index bits, 10 bits index L2 cache,
    // and LLC has 2048 sets per slice, so 11 bits index the LLC per slice.
    //
    // (SIB = Set Index Bit)
    //      abbbbbbbbbbcccccc
    //      ||         |-----> `c`s are for cache line offset indexing
    //      ||
    //      ||-> `b`s are L2 SIB (and are a subset of the L3 set indexing also)
    //      |
    //      |-> `a` is used in indexing LLC (e.g. 11th bit in our system)
    //      (aka "separate" L3 SIB, which is not for the L2 SIB also)
    //
    // if we have evset at a given `b`s combination (l2 set), then 2^a * n_slice is 
    // number of possible LLC evsets at that L2 set bit combination x page-offset bits.
    // for instance our local system has 20 slices, so that's 40 LLC evsets
    // per each L2 color x page offset.

    //  l3_cnt =    2 ^ (len(abbbbbbbbbb) - len(bbbbbbbbbb)) * 20 = 40 on our system for exmpl
    u64 l3_cnt = g_l3_cnt;

    // x is possible bits the separate L3 SIBs could form. 
    // E.g. with 1 separate L3 SIB, we could have bit 0 or 1, so that's 2 combos (2^1)
    f64 x = (f64)pow(2, unq_l3sib);
    u32 samples = g_config.num_l2_sets * g_config.evsets_per_l2;
    f64 p1 = x * (f64)binomial(l3_cnt / x, g_config.evsets_per_l2) / binomial(l3_cnt, g_config.evsets_per_l2);
    f64 expected = (pow(x, unq_l3sib) - p1) * g_config.num_l2_sets / total_l3_colors;
    printf(INFO "Theoretical expected coverage with %u samples: %.2f%%\n",
        samples, expected * 100.0);


    bool *total_covered = _calloc(total_l3_colors, sizeof(bool));
    if (!total_covered) {
        fprintf(stderr, ERR "failed to allocate coverage array\n");
        return EXIT_FAILURE;
    }

    for (u32 c = 0; c < g_config.num_l2_sets; c++) {
        bool *covered = _calloc(per_l2_colors, sizeof(bool));
        if (!covered) {
            fprintf(stderr, ERR "failed to allocate coverage array\n");
            free(total_covered);
            return EXIT_FAILURE;
        }

        for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
            EvSet *ev = complex[0][c][e];
            if (ev && ev->size > 0) {
                u64 hpa = va_to_hpa(ev->addrs[0]);
                u32 l3_col = cache_get_color(hpa, &l3_info);
                u32 extra = l3_col >> l2_info.unknown_sib;
                if (extra < per_l2_colors)
                    covered[extra] = true;
                if (l3_col < total_l3_colors)
                    total_covered[l3_col] = true;
            }
        }

        u32 covered_cnt = 0;
        for (u32 i = 0; i < per_l2_colors; i++)
            if (covered[i]) covered_cnt++;
        printf("L2 color %u: %u/%u L3 colors covered (%.2f%%)\n", c, covered_cnt,
               per_l2_colors,
               (f64)covered_cnt * 100.0 / per_l2_colors);
        free(covered);
    }

    u32 total_cnt = 0;
    for (u32 i = 0; i < total_l3_colors; i++)
        if (total_covered[i]) total_cnt++;
    printf("Overall: %u/%u L3 colors covered (%.2f%%)\n", total_cnt,
           total_l3_colors, (f64)total_cnt * 100.0 / total_l3_colors);
    free(total_covered);

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < g_config.num_l2_sets; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = complex[off][c][e];
                if (ev) {
                    if (ev->addrs) free(ev->addrs);
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

    return EXIT_SUCCESS;
}

typedef struct {
    EvSet ****complex;
    EvSet ***color_sets;
    u32 *color_counts;
    u32 n_colors;
    u32 n_ways;
    u32 total_sets;
    u32 n_pairs;
    f64 **tot_avg;
    pthread_t *threads;
    l2c_occ_worker_arg *wargs;
    u64 cycles_per_us;
} evrate_ctx_t;

static bool init_evrate_ctx(evrate_ctx_t *ctx)
{
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));

    u32 max_unc_sets = g_n_uncertain_l2_sets;
    if (g_config.num_l2_sets == 0)
        g_config.num_l2_sets = max_unc_sets;
    else if (g_config.num_l2_sets > max_unc_sets)
        g_config.num_l2_sets = max_unc_sets;
    if (g_config.evsets_per_l2 == 0)
        g_config.evsets_per_l2 = 1;
    if (g_config.num_offsets == 0)
        g_config.num_offsets = 1;

    ctx->complex = build_l3_evsets_para_gran(g_config.num_l2_sets,
                                             g_config.num_offsets, NULL);
    if (!ctx->complex)
        return false;

    ctx->n_colors = g_config.num_l2_sets;
    u32 max_sets = g_config.num_offsets * g_config.evsets_per_l2;
    ctx->color_sets = _calloc(ctx->n_colors, sizeof(EvSet **));
    ctx->color_counts = _calloc(ctx->n_colors, sizeof(u32));
    if (!ctx->color_sets || !ctx->color_counts)
        return false;

    for (u32 c = 0; c < ctx->n_colors; c++)
        ctx->color_sets[c] = _calloc(max_sets, sizeof(EvSet *));

    for (u32 off = 0; off < g_config.num_offsets; off++) {
        for (u32 c = 0; c < ctx->n_colors; c++) {
            for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                EvSet *ev = ctx->complex[off][c][e];
                if (ev && ev->size > 0) {
                    u32 idx = ctx->color_counts[c]++;
                    ctx->color_sets[c][idx] = ev;
                    if (ctx->n_ways == 0)
                        ctx->n_ways = ev->size;
                    ctx->total_sets++;
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

    ctx->tot_avg = _calloc(ctx->n_colors, sizeof(f64 *));
    ctx->threads = _calloc(ctx->n_pairs, sizeof(pthread_t));
    ctx->wargs = _calloc(ctx->n_pairs, sizeof(l2c_occ_worker_arg));
    if (!ctx->tot_avg || !ctx->threads || !ctx->wargs)
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
        u32 cnt = base + (i < extra ? 1 : 0);
        ctx->wargs[i].wait_us = 0;
        ctx->wargs[i].cycles_per_us = ctx->cycles_per_us;
        ctx->wargs[i].iterations = 1;
        ctx->wargs[i].color_sets = ctx->color_sets;
        ctx->wargs[i].color_counts = ctx->color_counts;
        ctx->wargs[i].tot_avg = ctx->tot_avg;
        ctx->wargs[i].start_color = next;
        ctx->wargs[i].num_colors = cnt;
        ctx->wargs[i].core_main = i * 2;
        ctx->wargs[i].core_helper = i * 2 + 1;
        ctx->wargs[i].prime_times = _calloc(1, sizeof(u64));
        next += cnt;
    }

    return true;
}

static void free_evrate_ctx(evrate_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->complex) {
        for (u32 off = 0; off < g_config.num_offsets; off++) {
            for (u32 c = 0; c < ctx->n_colors; c++) {
                for (u32 e = 0; e < g_config.evsets_per_l2; e++) {
                    EvSet *ev = ctx->complex[off][c][e];
                    if (ev) {
                        if (ev->addrs) free(ev->addrs);
                        if (ev->build_conf && ev->build_conf != &def_l3_build_conf &&
                            ev->build_conf != &def_l2_build_conf)
                            free(ev->build_conf);
                        free(ev);
                    }
                }
                free(ctx->complex[off][c]);
            }
            free(ctx->complex[off]);
        }
        free(ctx->complex);
    }

    if (ctx->color_sets) {
        for (u32 c = 0; c < ctx->n_colors; c++)
            if (ctx->color_sets[c])
                free(ctx->color_sets[c]);
        free(ctx->color_sets);
    }
    if (ctx->color_counts)
        free(ctx->color_counts);
    if (ctx->tot_avg) {
        for (u32 c = 0; c < ctx->n_colors; c++)
            if (ctx->tot_avg[c])
                free(ctx->tot_avg[c]);
        free(ctx->tot_avg);
    }
    if (ctx->threads)
        free(ctx->threads);
    if (ctx->wargs) {
        for (u32 i = 0; i < ctx->n_pairs; i++)
            if (ctx->wargs[i].prime_times)
                free(ctx->wargs[i].prime_times);
        free(ctx->wargs);
    }
}

void write_evrate_wait_data(u32 *times_us, f64 *rates, u32 n_points,
                            u32 prime_time_us)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "data/evrate-wait-%04d-%02d-%02d-%02d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    if (data_append && strlen(data_append) > 0) {
        strncat(filename, "-", sizeof(filename) - strlen(filename) - 1);
        strncat(filename, data_append,
                sizeof(filename) - strlen(filename) - 1);
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, ERR "failed to create evrate-wait data file: %s\n", filename);
        return;
    }

    fprintf(fp, "# eviction rate vs wait time data\n");
    fprintf(fp, "# time step: %u microseconds\n", heatmap_time_step_us);
    fprintf(fp, "# max time: %u microseconds\n", heatmap_max_time_us);
    fprintf(fp, "# prime time: %u microseconds\n", prime_time_us);
    fprintf(fp, "# columns: wait_us eviction_rate_percent\n");
    for (u32 i = 0; i < n_points; i++)
        fprintf(fp, "%u %.2f\n", times_us[i], rates[i] * 100.0);
    fclose(fp);
    printf(INFO "eviction rate data written to: %s\n", filename);
    char command[512];
    sprintf(command, "python3 ../scripts/plot_eviction_rate_wait.py ./%s", filename);
    i32 r = system(command);
    if (r < 0)
        fprintf(stderr, ERR "failed to execute command!\n");
}

u32 monitor_eviction_rate_wait(void)
{
    evrate_ctx_t ctx;
    if (!init_evrate_ctx(&ctx))
        return 0;

    if (move_cgroup_hi() == -1) {
        fprintf(stderr, ERR "Could not move vset to high-priority cgroup.\n"
                            "1) Running as root?\n"
                            "2) Ran scripts/setup_vset.sh ?\n");
        //return EXIT_FAILURE;
        exit(EXIT_FAILURE);
    }


    u32 prime_time_us = 0;
    for (u32 i = 0; i < ctx.n_pairs; i++)
        if (ctx.wargs[i].prime_times[0] > prime_time_us)
            prime_time_us = ctx.wargs[i].prime_times[0];
    for (u32 c = 0; c < ctx.n_colors; c++)
        ctx.tot_avg[c][0] = 0.0;

    u32 n_slots = (heatmap_max_time_us / heatmap_time_step_us) + 1;
    u32 start_slot = prime_time_us / heatmap_time_step_us + 1;
    if (start_slot >= n_slots) {
        free_evrate_ctx(&ctx);
        return 0;
    }

    u32 valid = n_slots - start_slot;
    u32 *times = _calloc(valid, sizeof(u32));
    f64 *rates = _calloc(valid, sizeof(f64));
    if (!times || !rates) {
        free(times);
        free(rates);
        free_evrate_ctx(&ctx);
        return 0;
    }

    for (u32 slot = start_slot, idx = 0; slot < n_slots; slot++, idx++) {
        u32 wait_us = slot * heatmap_time_step_us;
        for (u32 i = 0; i < ctx.n_pairs; i++)
            ctx.wargs[i].wait_us = wait_us;
        for (u32 i = 0; i < ctx.n_pairs; i++)
            pthread_create(&ctx.threads[i], NULL, l2c_occ_worker, &ctx.wargs[i]);
        for (u32 i = 0; i < ctx.n_pairs; i++)
            pthread_join(ctx.threads[i], NULL);

        f64 total_evicted = 0.0;
        for (u32 c = 0; c < ctx.n_colors; c++)
            total_evicted += ctx.tot_avg[c][0];
        rates[idx] = total_evicted / (ctx.n_ways * ctx.total_sets);
        times[idx] = wait_us;
        for (u32 c = 0; c < ctx.n_colors; c++)
            ctx.tot_avg[c][0] = 0.0;
    }

    write_evrate_wait_data(times, rates, valid, prime_time_us);

    free(times);
    free(rates);
    free_evrate_ctx(&ctx);
    return 1;
}

