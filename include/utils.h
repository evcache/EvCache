#ifndef UTILS_H
#define UTILS_H

#include "common.h"
#include "../include/cache_info.h"
#include "../include/mem.h"
#include "../vm_tools/vtop.h"
#include "asm.h"

#ifdef __cplusplus
extern "C" {
#endif

// _sep_arate prints
#define sep() puts("----");

typedef struct {
    i32 socket_id;
    i32 vcpu_count;
    i32 vcpus[MAX_CPUS];  // arr of vCPUs on this socket
} socket_info_t;

typedef struct {
    i32 n_sockets;
    socket_info_t sockets[MAX_CPUS];  // assuming max sockets = max cpus
} multi_socket_info_t;

i32 compare_u64(const void* a, const void* b);

void print_cache_lats(void);

void print_stats(u64 measurements[], i32 n);

i32 print_usage_vev(char* progname);

i32 print_usage_vset(char* progname);

i32 print_usage_vcolor(char* progname);

i32 print_usage_vpo(char* progname);

u64 get_cpu_freq_hz(void);

i32 set_cpu_affinity(u16 core_id);

i32 ensure_data_dir(void);

i32 pin_helper_by_ctrl(helper_thread_ctrl *ctrl, i32 core_id);

i32 pin_thread_by_pid(pthread_t pid, i32 core_id);

cpu_topology_t* get_vcpu_topo(void);

// find same-socket, non-SMT vCPU pair for thread pair pinning
// returns boolean indicating success + stores selected vCPUs in out params
bool find_same_socket_nonSMT_vcpu_pair(cpu_topology_t *topo, i32 *main_vcpu, i32 *helper_vcpu);

multi_socket_info_t get_socket_info(cpu_topology_t *topology);

bool is_topo_change_harmless(cpu_topology_t *old_topo, cpu_topology_t *new_topo,
                             i32 main_vcpu, i32 helper_vcpu);

void cleanup_mem(void* base_addr, EvCands* cands, u64 region_size);

void free_cache_info(CacheInfo* c_info);

u64 va_to_pa(void* va);

u32 va_to_l2color(void *va);

u64 time_us(void);

u32 log2_ceil(u64 v);

u32 n_digits(u32 v);

#define _swap(X, Y)               \
    do {                          \
        typeof(X) _tmp = (X);     \
        (X) = (Y);                \
        (Y) = _tmp;               \
    } while (0);

#define _min(x, y) ({(x) > (y) ? (y) : (x);})

#define _max(x, y) ({(x) > (y) ? (x) : (y);})

static ALWAYS_INLINE u64 _time_maccess(u8 *line)
{
    u64 start, end;
    _mfence();

    // warm up TLB
    _force_addr_calc(line);
    _timer_warmup();
    start = timer_start();
    maccess(line);
    end = timer_stop();
    _mfence();

    return end - start;
}

void shuffle_index(u32 *idxs, u32 sz);

void setup_segv_handler(void);

#ifdef __cplusplus
}
#endif
#endif // UTILS_H
