#include "../include/common.h"
#include "../include/utils.h"
#include "../vm_tools/vtop.h"
#include "../include/evset_para.h"
#include "../include/vset_ops.h"
#include "sys/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <sys/stat.h>

#include <string.h>
#include <signal.h>
#include <execinfo.h>

#include <unistd.h>

extern CacheLats g_lats;

static void segv_handler(i32 sig, siginfo_t *si, void *unused)
{
    void *array[20];
    i32 size = backtrace(array, 20);

    fprintf(stderr, "Error: signal %d:\n", sig);
    fprintf(stderr, "Segfault at address: %p\n", si->si_addr);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

void setup_segv_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = segv_handler;
    sa.sa_flags   = SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

i32 compare_u64(const void* a, const void* b)
{
    return (*(u64*)a > *(u64*)b) - (*(u64*)a < *(u64*)b);
}

i32 print_usage_vev(char *progname) 
{
    printf("Usage: %s <cache_level> [options]\n"
           "\n"
           "Arguments:\n"
           "  cache_level             Target cache level: L2, L3/LLC (required)\n"
           "\n"
           "Options:\n"
           "  -s, --cands-scaling N   Specify candidate scaling factor [default: 3]\n"
           "  -v, --verbose LEVEL     Show statistics and logs (1=basic, 2=detailed, 3=very detailed) [default: 0]\n"
           "  -d, --debug LEVEL       Show debugging information (1-3) [default: 0]\n"
           "                          Requires gpa_hpa tool to be set up on both the guest and host.\n"
           "  -n, --num-sets N        Number of page offsets to build evsets for (regular mode)\n"
           "  -u, --uncertain-sets N  Granular mode: number of L2 uncertain sets per offset [default: all]\n"
           "  -f, --evsets-per-l2 N   Number of eviction sets to build per L2 uncertain set [default: 1]\n"
           "  --vtop                  Topology-aware evset construction, by integrating with vTopology\n"
           "\n"
           "Parallelization options:\n"
           "  -c, --num-core N        N number of cores to leverage in core-parallelism for L3/LLC evset construction\n"
           "                          N must be even (each main thread paired with a helper thread)\n"
           "                          N=0: Auto-utilize maximum available cores (default if not specified)\n"
           "  -u, --uncertain-sets N  Enables granular mode with N L2 uncertain sets per offset\n"
           "  -f, --evsets-per-l2 N   Build N eviction sets at each L2 uncertain set\n"
           "  -o, --num-offsets N     Requires -u. Number of page offsets to work on [default: 1]\n"
           "\n"
           "Help:\n"
           "  -h, --help              Display this help message and exit\n", progname);
    return EXIT_SUCCESS;
}

i32 print_usage_vset(char *progname)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  -v, --verbose LEVEL     Show statistics (1=basic, 2=detailed, 3=full) [default: 0]\n"
           "  -d, --debug LEVEL       Show debugging information (1-3) [default: 0]\n"
           "  -c, --num-core N        Number of cores to use for monitoring threads\n"
           "  --activity-freq         With -G 0, plot L3 eviction activity frequency\n"
           "  -m, --max-records N     Activity frequency: number of records to collect [default: %lu]\n"
           "  -G, --graph TYPE        Generate graph data (see types below)\n"
           "  -a, --append NAME       Append NAME to generated plot filenames\n"
           "  -u, --uncertain-sets N  Granular mode: number of L2 uncertain sets per offset\n"
           "  -f, --evsets-per-l2 N   Eviction sets to use per L2 set [default: 1]\n"
           "  -o, --num-offsets N     Number of page offsets to cover [default: 1]\n"
           "  -w, --wait-time N       Wait time in microseconds between prime and probe [default: 7000]\n"
           "  -M, --max-time N        For evrate-wait: max wait time in microseconds.\n"
           "                          For occ-heatmap-l2color: number of iterations.\n"
           "                          [default: %u]\n"
           "  -t, --time-step N       For evrate-wait: time step in microseconds.\n"
           "                          For occ-heatmap-l2color: wait between iterations in milliseconds.\n"
           "                          [default: %u]\n"
           "  --live                  Live hotness monitoring mode\n"
           "                           -t sets print interval in milliseconds\n"
           "  --vtop                  Topology-aware mode with vTopology integration\n"
           "  --lcas                  Multi-socket LLC occupancy monitoring\n"
           "                           -t sets update interval in milliseconds\n"
           "  --fix-wait              Disable automatic wait-time adjustments\n"
           "  --perf                  Measure prime/probe performance\n"
           "  --fraction-check        Report L3 color coverage for generated eviction sets\n"
           "  --alpha-rise A          EWMA rise alpha [default: 0.85]\n"
           "  --alpha-fall A          EWMA fall alpha [default: 0.85]\n"
           "\n"
           "Graph Types (for -G):\n"
           "  0, eviction-freq        L3 eviction activity over time\n"
           "  1, evrate-wait          Eviction rate vs wait time\n"
           "  2, occ-heatmap-l2color  Occupancy grouped by L2 color\n"
           "  3, evrate-time          Eviction rate over time graph\n"
           "  4, l2color-dist         Host/guest L2 color distribution\n"
           "\n"
           "Other:\n"
           "  -r, --remap             Requires debug module (-d). See how often host page remapping breaks evset.\n"
           "\n"
           "Help:\n"
           "  -h, --help              Display this help message and exit\n",
           progname, max_num_recs, DEFAULT_HEATMAP_MAX_TIME_US, DEFAULT_HEATMAP_TIME_STEP_US);
    return EXIT_SUCCESS;
}

i32 print_usage_vcolor(char *progname)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  -s, --cands-scaling N   Scaling factor for filtering pages [default: 3]\n"
           "  -C, --vset-scale N      Scaling factor when building vset sets [default: 3]\n"
           "  -t, --sleep-time SEC    Sleep SEC seconds between insertions [default: 0]\n"
           "  -c, --num-cores N       Number of threads to use\n"
           "  -v, --verbose LEVEL     Verbosity level [default: 0]\n"
           "  -d, --debug LEVEL       Debug level (prints host colors)\n"
           "  --vset                  After insertion, monitor color hotness live\n"
           "    --scan-period MS      Print interval in milliseconds [default: 1000]\n"
           "    --scan-wait US        Wait time during prime+probe [default: 7000]\n"
           "    --alpha-rise A        EWMA rise alpha [default: 0.85]\n"
           "    --alpha-fall A        EWMA fall alpha [default: 0.85]\n"
           "\n"
           "Examples:\n"
           "  %s -s 10\n"
           "  %s -s 10 --vset -C 3 --scan-period 1000 --scan-wait 7000\n"
           "\n"
           "  -h, --help              Display this help and exit\n",
           progname, progname, progname);
    return EXIT_SUCCESS;
}

i32 print_usage_vpo(char *progname)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --use-gpa               Thrash the LLC using pages filtered using GPA LLC color bits\n"
           "  --use-hpa               Thrash the LLC using pages filtered using Host LLC color bits (requires custom hypercall).\n"
           "  --dist-only             With --use-gpa, show host color distribution\n"
           "  -z, --scale N           Scale 10MiB buffer for GPA/HPA modes by N\n"
           "  -a, --append NAME       Append NAME to generated plot filenames\n"
           "  -u, --uncertain-sets N  Number of L2 colors to use\n"
           "  -f, --evsets-per-l2 N   Eviction sets per color [default: 1]\n"
           "  -o, --num-offsets N     Number of offsets [default: 1]\n"
           "  -t, --wait-time US      Wait time between prime and thrash [default: 300]\n"
           "  -c, --num-core N        Number of cores (even value)\n"
           "  -v, --verbose LEVEL     Verbosity level\n"
           "  -d, --debug LEVEL       Debug level (prints host colors)\n"
           "\n"
           "Examples:\n"
           "  %s -u 16 -f 2 -o 64 -t 7000\n"
           "  %s -c 4 -u 8 -f 1 -o 32\n"
           "\n"
           "  -h, --help              Display this help and exit\n",
           progname, progname, progname);
    return EXIT_SUCCESS;
}

void print_cache_lats()
{
    printf(SUC "latencies: L1d: %zu | L2: %zu | L3: %zu | DRAM: %zu\n", 
            g_lats.l1d, g_lats.l2, g_lats.l3, g_lats.dram);

    printf(SUC "threshold: L1d: %zu | L2: %zu | L3: %zu | interrupt: %zu\n%s", 
           g_lats.l1d_thresh, g_lats.l2_thresh, g_lats.l3_thresh, g_lats.interrupt_thresh,
           (verbose) ? "" : "\n"); // extra space to separate from rest
           // if verbose is on, that itself would add the extra \n instead
}

void print_arg_conf(void)
{
    printf(V3 "Ran with configuration settings:\n");

    printf("  cache level: %s\n", 
           g_config.cache_level == L2 ? "L2" : "L3/LLC");
    
    printf("  verbose level: %u\n", g_config.verbose_level);
    printf("  debug level: %u\n", g_config.debug_level);
    
    if (granular) {
        printf("  granular mode: enabled\n");
        printf("  L2 uncertain sets per offset: %u\n", g_config.num_l2_sets);
        printf("  eviction sets per L2 set: %u\n", g_config.evsets_per_l2);
        printf("  number of page offsets: %u\n", g_config.num_offsets);
    } else {
        printf("  number of sets/offsets to generate evsets for: %u\n", g_config.num_sets);
    }
    
    printf("  candidate scaling factor: %u\n", g_config.cand_scaling);
}

u64 get_cpu_freq_hz() {
    // primary method: read from /proc/cpuinfo
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[512];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strstr(line, "cpu MHz")) {
                char *ptr = strchr(line, ':');
                if (ptr) {
                    f64 mhz = strtod(ptr + 1, NULL);
                    fclose(cpuinfo);
                    if (verbose > 1) printf(V2 "Retrieved CPU freq from /proc/cpuinfo\n");
                    return (u64)(mhz * 1000000); // covert MHz to Hz
                }
            }
        }
        fclose(cpuinfo); // failed
    }
    
    // backup 1 method: read from scaling_cur_freq
    FILE *scaling = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (scaling) {
        char line[64];
        if (fgets(line, sizeof(line), scaling)) {
            // this file reports kHz
            u64 khz = strtoull(line, NULL, 10);
            fclose(scaling);
            if (verbose > 1) printf(V2 "Retrieved CPU freq from scaling_cur_freq\n");
            return khz * 1000; // kHz to Hz
        }
        fclose(scaling);
    }
    
    // backup 2: read from cpuinfo_max_freq
    FILE *max_freq = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    if (max_freq) {
        char line[64];
        if (fgets(line, sizeof(line), max_freq)) {
            // this file reports kHz
            u64 khz = strtoull(line, NULL, 10);
            fclose(max_freq);
            if (verbose > 1) printf(V2 "Retrieved CPU freq from cpu_max_freq\n");
            return khz * 1000;
        }
        fclose(max_freq);
    }

    return 0; // failed
}

i32 set_cpu_affinity(u16 core_id)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    return (sched_setaffinity(0, sizeof(mask), &mask));
}

i32 ensure_data_dir(void)
{
    struct stat st;
    if (stat("data", &st) == -1) {
        printf(NOTE "build/data directory does not exist. Trying to create it.\n");
        if (mkdir("./data", S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
            fprintf(stderr, ERR "Failed creating build/data directory. Please create it manually.\n");
            return -1;
        } else {
            printf(SUC "Created data directory.\n");
        }
    }
    return 0;
}

i32 pin_thread_by_pid(pthread_t pid, i32 core_id) 
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    i32 ret = pthread_setaffinity_np(pid, sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        fprintf(stderr, ERR "failed to pin thread to vCPU %d (error: %d)\n", core_id, ret);
        return -1;
    }
    
    if (verbose > 1) {
        printf(V2 "directly pinned thread to vCPU %d\n", core_id);
    }
    
    return 0;
}

i32 pin_helper_by_ctrl(helper_thread_ctrl *ctrl, i32 core_id)
{
    if (!ctrl || !ctrl->running) {
        return -1;
    }
    
    // update helper thread's core ID
    ctrl->core_id = core_id;
    
    // pin actual thread
    i32 ret = pin_thread_by_pid(ctrl->pid, core_id);
    
    if (ret == 0 && verbose > 1) {
        printf(V2 "directly pinned helper thread to vCPU %d\n", core_id);
    }
    
    return ret;
}

cpu_topology_t* get_vcpu_topo(void)
{
    cpu_topology_t* topo = NULL;
    u16 ret_vtop = 3;
    for (u16 r = 0; r < ret_vtop; r++) {
        topo = detect_vcpu_topology(NULL);
        if (topo) break;
        usleep(10000);
        printf(WRN "vTop: Failed topology detection try %u/%u\n", r + 1, ret_vtop);
    }
    return topo;
}

bool find_same_socket_nonSMT_vcpu_pair(cpu_topology_t *topo, i32 *main_vcpu, i32 *helper_vcpu)
{
    if (!topo || !main_vcpu || !helper_vcpu) {
        return false;
    }

    *main_vcpu = -1; // unpinned default
    *helper_vcpu = -1;
    
    bool found_pair = false;
    for (i32 i = 0; i < topo->nr_cpus && !found_pair; i++) {
        for (i32 j = 0; j < topo->nr_cpus; j++) {
            // skip if same vcpu or SMT siblings
            if (i == j || topo->relation_matrix[i][j] == CPU_RELATION_SMT) {
                continue;
            }
            
            // check if on same socket but different cores
            if (topo->relation_matrix[i][j] == CPU_RELATION_SOCKET) {
                *main_vcpu = i;
                *helper_vcpu = j;
                found_pair = true;
                break;
            }
        }
    }
    
    if (!found_pair) {
        printf(WRN "vTop: couldn't find optimal pair; leaving up to OS scheduler.\n");
    }
    
    return found_pair;
}

multi_socket_info_t get_socket_info(cpu_topology_t *topology)
{
    multi_socket_info_t info = {0};
    
    if (!topology) {
        return info;
    }
    
    // find unique sockets and count vCPUs per socket
    for (i32 i = 0; i < topology->nr_cpus; i++) {
        i32 socket = topology->cpu_to_socket[i];
        bool found = false;
        
        // check if socket already exists in our list
        for (i32 s = 0; s < info.n_sockets; s++) {
            if (info.sockets[s].socket_id == socket) {
                // add vCPU to this socket's list
                info.sockets[s].vcpus[info.sockets[s].vcpu_count++] = i;
                found = true;
                break;
            }
        }
        
        if (!found) {
            // new socket found
            i32 idx = info.n_sockets++;
            info.sockets[idx].socket_id = socket;
            info.sockets[idx].vcpus[0] = i;
            info.sockets[idx].vcpu_count = 1;
        }
    }
    
    if (verbose) {
        printf(V1 "Detected %d sockets:\n", info.n_sockets);
        for (i32 s = 0; s < info.n_sockets; s++) {
            printf(V1 "  Socket %d: %d vCPUs (", 
                   info.sockets[s].socket_id, info.sockets[s].vcpu_count);
            for (i32 v = 0; v < info.sockets[s].vcpu_count; v++) {
                printf("%d%s", info.sockets[s].vcpus[v], 
                       (v < info.sockets[s].vcpu_count - 1) ? ", " : "");
            }
            printf(")\n");
        }
    }
    
    return info;
}

bool is_topo_change_harmless(cpu_topology_t *prev_topo, cpu_topology_t *curr_topo, 
                            i32 main_vcpu, i32 helper_vcpu)
{
    if (!prev_topo || !curr_topo)
        return true;
        
    // check if still on same socket
    if (curr_topo->cpu_to_socket[main_vcpu] != curr_topo->cpu_to_socket[helper_vcpu])
        return false;
        
    // check if became SMT siblings
    if (curr_topo->relation_matrix[main_vcpu][helper_vcpu] == CPU_RELATION_SMT)
        return false;
        
    return true;
}

void cleanup_mem(void* base_addr, EvCands* cands, u64 region_size)
{
    (void)region_size;
    if (base_addr)
        free(base_addr);

    if (cands)
        evcands_free(cands);
}

u64 va_to_pa(void* va)
{
    FILE *pagemap = fopen("/proc/self/pagemap", "rb");
    if (!pagemap) {
        perror("fopen");
        return 0;
    }
    u64 page_size = getpagesize();
    u64 offset = ((u64)va / page_size) * sizeof(u64);
    if (fseek(pagemap, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(pagemap);
        return 0;
    }
    u64 entry = 0;
    if (fread(&entry, sizeof(u64), 1, pagemap) != 1) {
        perror("fread");
        fclose(pagemap);
        return 0;
    }
    fclose(pagemap);
    if (!(entry & (1ULL << 63))) {
        return 0;
    }
    u64 pfn = entry & ((1ULL << 55) - 1);
    u64 pa = (pfn << 12) | ((u64)va & (page_size - 1));
    return pa;
}

u32 va_to_l2color(void *va)
{
    u64 gpa = va_to_pa(va);
    return cache_get_color(gpa, &l2_info);
}

// in microseconds | µs
u64 time_us(void) 
{ 
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000000LL + (u64)tv.tv_usec;
}

inline u32 log2_ceil(u64 v) 
{
    u32 cnt = 0;
    while ((1ull << cnt) < v) {
        cnt += 1;
    }
    return cnt;
}

inline u32 n_digits(u32 n)
{
    u32 count = 0;
    do {
        count++;
        n /= 10;
    } while (n != 0);
    return count;
}

void shuffle_index(u32 *idxs, u32 sz)
{
    srand(time(NULL));
    for (u32 tail = sz - 1; tail > 0; tail--) {
        u32 n_choice = tail + 1;
        u32 choice = rand() % n_choice;
        _swap(idxs[choice], idxs[tail]);
    }
}

i32 n_system_cores(void)
{
    i32 n_cores = get_nprocs();
    if (n_cores <= 0) {
        fprintf(stderr, ERR "failed to detect the number of cores. assuming 2 cores.\n");
        return 2;
    }
    return n_cores;
}
