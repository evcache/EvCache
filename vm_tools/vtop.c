/*
 * Copyright (C) 2018-2019 VMware, Inc.
 * SPDX-License-Identifier: GPL-2.0
 *
 * and vTopology:
 * https://github.com/vSched/vtopology/tree/33626d01bd519771ed801f7c9ed586cbce58c9fb
 * Rewritten version in C
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "../include/common.h"
#include "vtop.h"

#define PROBE_MODE (0)
#define DIRECT_MODE (1)

#define GROUP_LOCAL (0)
#define GROUP_NONLOCAL (1)
#define GROUP_GLOBAL (2)

#define NUMA_GROUP (0)
#define PAIR_GROUP (1)
#define THREAD_GROUP (2)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define LAST_CPU_ID (min(nr_cpus, MAX_CPUS))

typedef unsigned atomic_t;

extern i32 verbose;
// global parameters
static int nr_cpus;
static int NR_SAMPLES = 10;
static int SAMPLE_US = 10000;
static int act_sample = 10;

static int all_samples_found = 0;
static bool first_measurement = false;
static int nr_numa_groups = 0;
static int nr_pair_groups = 0;
static int nr_tt_groups = 0;
static double threefour_latency_class = 8500;
static int cpu_group_id[MAX_CPUS];
static int cpu_pair_id[MAX_CPUS];
static int cpu_tt_id[MAX_CPUS];

static bool failed_test = false;
static int latency_valid = -1;
static int nr_param = 150;
static int **numa_to_pair_arr = NULL;
static int **pair_to_thread_arr = NULL;
static int **thread_to_cpu_arr = NULL;
static int *numas_to_cpu = NULL;
static int *pairs_to_cpu = NULL;
static int *threads_to_cpu = NULL;
static int **top_stack = NULL;
static pthread_mutex_t top_stack_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef union {
    atomic_t x;
    char pad[1024];
} big_atomic_t __attribute__((aligned(1024)));

struct thread_args_t {
    cpu_set_t cpus;
    atomic_t me;
    atomic_t buddy;
    big_atomic_t *nr_pingpongs;
    atomic_t **pingpong_mutex;
    int *stoploops;
    uint64_t *timestamps;
    int timestamps_size;
    int timestamps_capacity;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    int *flag;
    bool *prepared;
    int *max_loops;
};

typedef struct {
    int *pairs_to_test;
    int size;
} worker_thread_args;

static inline uint64_t now_nsec(void) 
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * ((uint64_t)1000 * 1000 * 1000) + ts.tv_nsec;
}

static void common_setup(struct thread_args_t *args) 
{
    if (sched_setaffinity(0, sizeof(cpu_set_t), &args->cpus)) {
        perror("sched_setaffinity");
        exit(1);
    }

    if (args->me == 0) {
        *(args->pingpong_mutex) =
            (atomic_t *)mmap(0, getpagesize(), PROT_READ | PROT_WRITE,
                            MAP_ANON | MAP_PRIVATE, -1, 0);
        if (*(args->pingpong_mutex) == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
        *(*(args->pingpong_mutex)) = args->me;
    }

    pthread_mutex_lock(args->mutex);
    if (*(args->flag)) {
        *(args->flag) = 0;
        pthread_cond_wait(args->cond, args->mutex);
    } else {
        *(args->flag) = 1;
        pthread_cond_broadcast(args->cond);
    }
    pthread_mutex_unlock(args->mutex);
    *(args->prepared) = true;
}

static void *thread_fn(void *data) 
{
    int amount_of_loops = 0;
    struct thread_args_t *args = (struct thread_args_t *)data;
    common_setup(args);
    atomic_t nr = 0;
    atomic_t sample_size = (atomic_t)nr_param;
    atomic_t me = args->me;
    atomic_t buddy = args->buddy;
    int *stop_loops = args->stoploops;
    int *max_loops = args->max_loops;
    atomic_t *cache_pingpong_mutex = *(args->pingpong_mutex);

    //add_thread_to_vtop_cgroup();
    
    while (1) {
        if (amount_of_loops++ > *max_loops ||
            args->timestamps_size > act_sample) {
            if (*stop_loops == 1) {
                *stop_loops += 3;
                *max_loops = amount_of_loops;
                pthread_exit(0);
            } else {
                *stop_loops += 1;
            }
        }
        if (*stop_loops > 2) {
            *max_loops = amount_of_loops;
            pthread_exit(0);
        }

        if (__sync_bool_compare_and_swap(cache_pingpong_mutex, me, buddy)) {
            ++nr;
            if ((nr > sample_size) && me == 0) {
                // ensure we have space for the timestamp
                if (args->timestamps_size >= args->timestamps_capacity) {
                    args->timestamps_capacity *= 2;
                    args->timestamps = realloc(args->timestamps, 
                                              args->timestamps_capacity * sizeof(uint64_t));
                }
                args->timestamps[args->timestamps_size++] = now_nsec();
                nr = 0;
            }
        }
    }
    return NULL;
}

// pins calling thread to two cores
static int pin_threads_to_core(int core_id, int core_id2) 
{
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (core_id < 0 || core_id >= num_cores)
        return EINVAL;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    CPU_SET(core_id2, &cpuset);
    pthread_t current_thread = pthread_self();
    return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

static int get_latency_class(int latency) 
{
    if (latency < 0 || latency > 90000) {
        return 1;
    }

    if (latency < 2000) {
        return 2;
    }
    if (latency < threefour_latency_class) {
        return 3;
    }

    return 4;
}

static int measure_latency_pair(int i, int j) 
{
    int amount_of_times = 0;
    if (latency_valid != -1 && latency_valid != 1) {
        amount_of_times = -2;
    }
    if (latency_valid == 1) {
        amount_of_times = 2;
    }
    int max_loops = SAMPLE_US;
    if (first_measurement) {
        amount_of_times = -2;
        max_loops = SAMPLE_US * 10;
        first_measurement = false;
    }

    while (1) {
        pin_threads_to_core(i, j);
        atomic_t *pingpong_mutex = NULL;
        void *mmapped_memory = NULL;
        pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
        big_atomic_t nr_pingpongs;
        int stop_loops = 0;
        bool prepared = false;
        int wait_for_buddy = 1;
        
        struct thread_args_t even;
        struct thread_args_t odd;
        
        // initialize thread args
        CPU_ZERO(&even.cpus);
        CPU_SET(i, &even.cpus);
        even.me = 0;
        even.buddy = 1;
        even.pingpong_mutex = &pingpong_mutex;
        even.nr_pingpongs = &nr_pingpongs;
        even.stoploops = &stop_loops;
        even.mutex = &wait_mutex;
        even.cond = &wait_cond;
        even.flag = &wait_for_buddy;
        even.prepared = &prepared;
        even.max_loops = &max_loops;
        even.timestamps = (uint64_t*)malloc(20 * sizeof(uint64_t)); // initial capacity
        even.timestamps_size = 0;
        even.timestamps_capacity = 20;
        
        CPU_ZERO(&odd.cpus);
        CPU_SET(j, &odd.cpus);
        odd.me = 1;
        odd.buddy = 0;
        odd.pingpong_mutex = &pingpong_mutex;
        odd.nr_pingpongs = &nr_pingpongs;
        odd.stoploops = &stop_loops;
        odd.mutex = &wait_mutex;
        odd.cond = &wait_cond;
        odd.flag = &wait_for_buddy;
        odd.prepared = &prepared;
        odd.max_loops = &max_loops;
        odd.timestamps = (uint64_t*)malloc(20 * sizeof(uint64_t)); // initial capacity
        odd.timestamps_size = 0;
        odd.timestamps_capacity = 20;
        
        __sync_lock_test_and_set(&nr_pingpongs.x, 0);

        pthread_t t_odd;
        pthread_t t_even;

        if (pthread_create(&t_odd, NULL, thread_fn, &odd)) {
            printf("ERROR creating odd thread\n");
            exit(1);
        }
        if (pthread_create(&t_even, NULL, thread_fn, &even)) {
            printf("ERROR creating even thread\n");
            exit(1);
        }

        double best_sample = 1.0 / 0.0;

        pthread_join(t_odd, NULL);
        pthread_join(t_even, NULL);
        
        mmapped_memory = (void*)pingpong_mutex;
        
        if (even.timestamps_size == 1) {
            free(even.timestamps);
            free(odd.timestamps);
            if (mmapped_memory) {
                munmap(mmapped_memory, getpagesize());
            }
            continue;
        }
        
        if (mmapped_memory) {
            munmap(mmapped_memory, getpagesize());
        }

        if (even.timestamps_size < 3) {
            free(even.timestamps);
            free(odd.timestamps);
            if (amount_of_times < NR_SAMPLES) {
                amount_of_times++;
                continue;
            } else {
                if (verbose > 2) {
                    printf(V3 "Times around:%d I:%d J:%d Sample passed %d next.\n", 
                           amount_of_times, i, j, -1);
                }
                return -1;
            }
        }
        
        if (even.timestamps_size < (unsigned int)(act_sample - 2) &&
            even.timestamps_size > 3) {
            if (SAMPLE_US > 100000) {
                SAMPLE_US += 100000;
            } else {
                SAMPLE_US = (int)(SAMPLE_US * 2);
            }
            if (verbose > 1) {
                printf("Samples moved up\n");
            }
        }

        for (unsigned int z = 0; z < even.timestamps_size - 1; z++) {
            double sample = (even.timestamps[z + 1] - even.timestamps[z]) /
                            (double)(nr_param * 2);
            if (sample < best_sample) {
                best_sample = sample;
            }
        }

        if (verbose > 2) {
            printf(V3 "Times around:%d I:%d J:%d Sample passed %d next.\n", 
                  amount_of_times, i, j, (int)(best_sample * 100));
        }
        
        free(even.timestamps);
        free(odd.timestamps);
        return (int)(best_sample * 100);
    }
}

static void set_latency_pair(int x, int y, int latency_class) 
{
    top_stack[x][y] = latency_class;
    top_stack[y][x] = latency_class;
}

/* naive
static void apply_optimization(void) 
{
    int sub_rel;
    for (int x = 0; x < LAST_CPU_ID; x++) {
        for (int y = 0; y < LAST_CPU_ID; y++) {
            sub_rel = top_stack[y][x];
            for (int z = 0; z < LAST_CPU_ID; z++) {
                if ((top_stack[y][z] < sub_rel && top_stack[y][z] != 0)) {
                    if (top_stack[x][z] == 0) {
                        set_latency_pair(x, z, sub_rel);
                    } else if (top_stack[x][z] != sub_rel) {
                        failed_test = true;
                        if (top_stack[y][z] == 1) {
                            // try harder if stacking is involved in failure
                            if (verbose > 1) {
                                printf("Adjusted upwards due to stacking\n");
                            }
                            if (SAMPLE_US > 100000) {
                                SAMPLE_US += 100000;
                            } else {
                                SAMPLE_US = (int)(SAMPLE_US * 2);
                            }
                            return;
                        }
                        failed_test = true;
                    }
                }
            }
        }
    }
}
*/

/* floyd warshall based propogation */
void apply_optimization(void) 
{
    i32 n = LAST_CPU_ID;
    for (i32 k = 0; k < n; k++) {
        for (i32 i = 0; i < n; i++) {
            if (top_stack[i][k] == 0) continue;
            for (i32 j = 0; j < n; j++) {
                if (top_stack[k][j] == 0) continue;
                i32 via = (top_stack[i][k] > top_stack[k][j]) 
                          ? top_stack[i][k] : top_stack[k][j];
                if (top_stack[i][j] == 0 || top_stack[i][j] > via) {
                    set_latency_pair(i, j, via);
                }
            }
        }
    }
}

static void print_population_matrix(void) 
{
    int i, j;

    for (i = 0; i < LAST_CPU_ID; i++) {
        for (j = 0; j < LAST_CPU_ID; j++)
            if (top_stack[i][j] == -1) {
                printf("%7s", "INF");
            } else {
                printf("%7d", top_stack[i][j]);
            }
        printf("\n");
    }
}

static int find_numa_groups(void) 
{
    nr_numa_groups = 0;
    for (int i = 0; i < LAST_CPU_ID; i++) {
        cpu_group_id[i] = -1;
    }
    
    // reinitialize numa_to_pair_arr
    if (numa_to_pair_arr) {
        for (int i = 0; i < nr_numa_groups; i++) {
            free(numa_to_pair_arr[i]);
        }
        free(numa_to_pair_arr);
    }
    numa_to_pair_arr = NULL;
    
    // reinitialize numas_to_cpu
    free(numas_to_cpu);
    numas_to_cpu = NULL;
    
    first_measurement = true;
    
    for (int i = 0; i < LAST_CPU_ID; i++) {
        if (cpu_group_id[i] != -1) {
            continue;
        }
        cpu_group_id[i] = nr_numa_groups;
        for (int j = 0; j < LAST_CPU_ID; j++) {
            if (cpu_group_id[j] != -1) {
                continue;
            }
            if (top_stack[i][j] == 0) {
                int latency = measure_latency_pair(i, j);
                set_latency_pair(i, j, get_latency_class(latency));
            }
            if (top_stack[i][j] < 4) {
                cpu_group_id[j] = nr_numa_groups;
            }
        }
        nr_numa_groups++;
        
        // reallocate arrays for the new group
        numa_to_pair_arr = (int **)realloc(numa_to_pair_arr, nr_numa_groups * sizeof(int *));
        if (!numa_to_pair_arr) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
        
        numa_to_pair_arr[nr_numa_groups-1] = (int *)calloc(LAST_CPU_ID, sizeof(int));
        if (!numa_to_pair_arr[nr_numa_groups-1]) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
        
        numas_to_cpu = (int *)realloc(numas_to_cpu, nr_numa_groups * sizeof(int));
        if (!numas_to_cpu) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
        
        numas_to_cpu[nr_numa_groups-1] = i;
    }

    apply_optimization();
    return nr_numa_groups;
}

static void ST_find_topology(int *input, int input_size) 
{
    for (int x = 0; x < input_size; x++) {
        int j = input[x] % LAST_CPU_ID;
        int i = (input[x] - (input[x] % LAST_CPU_ID)) / LAST_CPU_ID;

        if (top_stack[i][j] == 0) {
            int latency = measure_latency_pair(i, j);
            pthread_mutex_lock(&top_stack_mutex);
            set_latency_pair(i, j, get_latency_class(latency));
            if (latency_valid == -1) {
                apply_optimization();
            }
            pthread_mutex_unlock(&top_stack_mutex);
        }
        if (failed_test ||
            (latency_valid != -1 && latency_valid != top_stack[i][j])) {
            failed_test = true;
            return;
        }
    }
    return;
}

static void *thread_fn2(void *data) 
{
    worker_thread_args *args = (worker_thread_args *)data;

    //add_thread_to_vtop_cgroup();

    ST_find_topology(args->pairs_to_test, args->size);
    return NULL;
}

static void MT_find_topology(int **all_pairs_to_test, int *sizes, int num_groups) 
{
    if (!all_pairs_to_test || !sizes || num_groups <= 0) {
        return;
    }
    
    worker_thread_args *worker_args = (worker_thread_args *)malloc(num_groups * sizeof(worker_thread_args));
    pthread_t *worker_tasks = (pthread_t *)malloc(num_groups * sizeof(pthread_t));
    
    if (!worker_args || !worker_tasks) {
        fprintf(stderr, "Memory allocation failed\n");
        free(worker_args);
        free(worker_tasks);
        return;
    }

    for (int i = 0; i < num_groups; i++) {
        // skip empty groups
        if (!all_pairs_to_test[i] || sizes[i] <= 0) {
            worker_args[i].pairs_to_test = NULL;
            worker_args[i].size = 0;
            continue;
        }
        
        worker_args[i].pairs_to_test = all_pairs_to_test[i];
        worker_args[i].size = sizes[i];
        pthread_create(&worker_tasks[i], NULL, thread_fn2, &worker_args[i]);
    }
    
    for (int i = 0; i < num_groups; i++) {
        // skip empty groups
        if (!worker_args[i].pairs_to_test || worker_args[i].size <= 0) {
            continue;
        }
        pthread_join(worker_tasks[i], NULL);
    }
    
    free(worker_args);
    free(worker_tasks);
}

static void perform_probing() 
{
    failed_test = false;
    all_samples_found = true;
    
    find_numa_groups();
    apply_optimization();
    
    // skip probing if numa groups detection failed
    if (nr_numa_groups <= 0) {
        return;
    }
    
    int **all_pairs_to_test = (int **)malloc(nr_numa_groups * sizeof(int *));
    int *sizes = (int *)calloc(nr_numa_groups, sizeof(int));
    
    if (!all_pairs_to_test || !sizes) {
        fprintf(stderr, "Memory allocation failed\n");
        free(all_pairs_to_test);
        free(sizes);
        return;
    }
    
    // init with default capacity
    for (int i = 0; i < nr_numa_groups; i++) {
        all_pairs_to_test[i] = NULL;
    }
    
    // count pairs first before allocation
    for (int i = 0; i < LAST_CPU_ID; i++) {
        for (int j = i + 1; j < LAST_CPU_ID; j++) {
            if (top_stack[i][j] == 0) {
                if (cpu_group_id[i] >= 0 && cpu_group_id[i] < nr_numa_groups && 
                    cpu_group_id[j] >= 0 && cpu_group_id[j] < nr_numa_groups &&
                    cpu_group_id[i] == cpu_group_id[j]) {
                    sizes[cpu_group_id[i]]++;
                }
            }
        }
    }
    
    // allocate per group
    for (int i = 0; i < nr_numa_groups; i++) {
        if (sizes[i] > 0) {
            all_pairs_to_test[i] = (int *)malloc(sizes[i] * sizeof(int));
            if (!all_pairs_to_test[i]) {
                fprintf(stderr, "Memory allocation failed\n");
                for (int j = 0; j < i; j++) {
                    free(all_pairs_to_test[j]);
                }
                free(all_pairs_to_test);
                free(sizes);
                return;
            }
        }
        sizes[i] = 0; // reset for filling
    }
    
    // fill groups
    for (int i = 0; i < LAST_CPU_ID; i++) {
        for (int j = i + 1; j < LAST_CPU_ID; j++) {
            if (top_stack[i][j] == 0) {
                if (cpu_group_id[i] >= 0 && cpu_group_id[i] < nr_numa_groups && 
                    cpu_group_id[j] >= 0 && cpu_group_id[j] < nr_numa_groups &&
                    cpu_group_id[i] == cpu_group_id[j]) {
                    all_pairs_to_test[cpu_group_id[i]][sizes[cpu_group_id[i]]++] = i * LAST_CPU_ID + j;
                }
            }
        }
    }
    
    MT_find_topology(all_pairs_to_test, sizes, nr_numa_groups);
    
    // cleanup
    for (int i = 0; i < nr_numa_groups; i++) {
        free(all_pairs_to_test[i]);
    }
    free(all_pairs_to_test);
    free(sizes);
}

static void parse_topology(void) 
{
    int i, j = 0;
    nr_pair_groups = 0;
    nr_tt_groups = 0;
    nr_cpus = get_nprocs();

    // clear all previous topology data (excluding numa level)
    for (i = 0; i < LAST_CPU_ID; i++) {
        cpu_pair_id[i] = -1;
        cpu_tt_id[i] = -1;
    }
    
    // free previous arrays if they exist
    if (pair_to_thread_arr) {
        for (i = 0; i < nr_pair_groups; i++) {
            free(pair_to_thread_arr[i]);
        }
        free(pair_to_thread_arr);
    }
    
    if (thread_to_cpu_arr) {
        for (i = 0; i < nr_tt_groups; i++) {
            free(thread_to_cpu_arr[i]);
        }
        free(thread_to_cpu_arr);
    }
    
    free(pairs_to_cpu);
    free(threads_to_cpu);
    
    pair_to_thread_arr = NULL;
    thread_to_cpu_arr = NULL;
    pairs_to_cpu = NULL;
    threads_to_cpu = NULL;

    for (i = 0; i < LAST_CPU_ID; i++) {
        if (cpu_pair_id[i] == -1) {
            cpu_pair_id[i] = nr_pair_groups;
            
            // allocate for new pair group
            pair_to_thread_arr = (int **)realloc(pair_to_thread_arr, (nr_pair_groups + 1) * sizeof(int *));
            if (!pair_to_thread_arr) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            pair_to_thread_arr[nr_pair_groups] = (int *)calloc(LAST_CPU_ID, sizeof(int));
            if (!pair_to_thread_arr[nr_pair_groups]) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            pairs_to_cpu = (int *)realloc(pairs_to_cpu, (nr_pair_groups + 1) * sizeof(int));
            if (!pairs_to_cpu) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            pairs_to_cpu[nr_pair_groups] = i;
            nr_pair_groups++;
        }

        if (cpu_tt_id[i] == -1) {
            cpu_tt_id[i] = nr_tt_groups;
            
            // allocate for new thread group
            thread_to_cpu_arr = (int **)realloc(thread_to_cpu_arr, (nr_tt_groups + 1) * sizeof(int *));
            if (!thread_to_cpu_arr) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            thread_to_cpu_arr[nr_tt_groups] = (int *)calloc(LAST_CPU_ID, sizeof(int));
            if (!thread_to_cpu_arr[nr_tt_groups]) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            threads_to_cpu = (int *)realloc(threads_to_cpu, (nr_tt_groups + 1) * sizeof(int));
            if (!threads_to_cpu) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            
            threads_to_cpu[nr_tt_groups] = i;
            nr_tt_groups++;
        }

        for (j = 0; j < LAST_CPU_ID; j++) {
            if (top_stack[i][j] < 3 && cpu_pair_id[i] != -1) {
                cpu_pair_id[j] = cpu_pair_id[i];
            }
            if (top_stack[i][j] < 2 && cpu_tt_id[i] != -1) {
                cpu_tt_id[j] = cpu_tt_id[i];
            }
        }
        numa_to_pair_arr[cpu_group_id[i]][cpu_pair_id[i]] = 1;
        pair_to_thread_arr[cpu_pair_id[i]][cpu_tt_id[i]] = 1;
        thread_to_cpu_arr[cpu_tt_id[i]][i] = 1;
    }
}

cpu_topology_t *detect_vcpu_topology(void *custom_params) 
{
    // topology structure
    cpu_topology_t *topology = (cpu_topology_t *)calloc(1, sizeof(cpu_topology_t));
    if (!topology) {
        fprintf(stderr, "Memory allocation failed for topology structure\n");
        return NULL;
    }
    
    nr_cpus = get_nprocs();
    
    // init topo matrix
    top_stack = (int **)malloc(LAST_CPU_ID * sizeof(int *));
    if (!top_stack) {
        fprintf(stderr, "Memory allocation failed\n");
        free(topology);
        return NULL;
    }
    
    for (int i = 0; i < LAST_CPU_ID; i++) {
        top_stack[i] = (int *)calloc(LAST_CPU_ID, sizeof(int));
        if (!top_stack[i]) {
            fprintf(stderr, "Memory allocation failed\n");
            for (int j = 0; j < i; j++) {
                free(top_stack[j]);
            }
            free(top_stack);
            top_stack = NULL;
            free(topology);
            return NULL;
        }
    }
    
    // init diagonal (same-core relation)
    for (int p = 0; p < LAST_CPU_ID; p++) {
        top_stack[p][p] = 1;
    }
    
    perform_probing();
    
    bool success = !failed_test;
    
    if (success) {
        parse_topology();
        
        topology->nr_cpus = nr_cpus;
        topology->nr_sockets = nr_numa_groups;
        topology->nr_cores = nr_tt_groups;
        
        // map each vCPU to its socket and core
        for (int i = 0; i < LAST_CPU_ID; i++) {
            topology->cpu_to_socket[i] = cpu_group_id[i];
            topology->cpu_to_core[i] = cpu_tt_id[i];
        }
        
        // relation matrix
        for (int i = 0; i < LAST_CPU_ID; i++) {
            for (int j = 0; j < LAST_CPU_ID; j++) {
                if (cpu_tt_id[i] == cpu_tt_id[j]) {
                    // SMT siblings (threads on same core)
                    topology->relation_matrix[i][j] = CPU_RELATION_SMT;
                } else if (cpu_group_id[i] == cpu_group_id[j]) {
                    // same socket
                    topology->relation_matrix[i][j] = CPU_RELATION_SOCKET;
                } else {
                    // different sockets
                    topology->relation_matrix[i][j] = CPU_RELATION_REMOTE;
                }
            }
        }
        
        if (verbose > 2) {
            print_population_matrix();
        }
    } else {
        // already reported by caller if failed
        if (verbose > 1) {
            fprintf(stderr, ERR "Topology detection failed\n");
        }
        free(topology);
        topology = NULL;
    }
    
    // cleanup
    for (int i = 0; i < LAST_CPU_ID; i++) {
        free(top_stack[i]);
    }
    free(top_stack);
    top_stack = NULL;
    
    if (numa_to_pair_arr) {
        for (int i = 0; i < nr_numa_groups; i++) {
            free(numa_to_pair_arr[i]);
        }
        free(numa_to_pair_arr);
        numa_to_pair_arr = NULL;
    }
    
    if (pair_to_thread_arr) {
        for (int i = 0; i < nr_pair_groups; i++) {
            free(pair_to_thread_arr[i]);
        }
        free(pair_to_thread_arr);
        pair_to_thread_arr = NULL;
    }
    
    if (thread_to_cpu_arr) {
        for (int i = 0; i < nr_tt_groups; i++) {
            free(thread_to_cpu_arr[i]);
        }
        free(thread_to_cpu_arr);
        thread_to_cpu_arr = NULL;
    }
    
    free(numas_to_cpu);
    free(pairs_to_cpu);
    free(threads_to_cpu);
    numas_to_cpu = NULL;
    pairs_to_cpu = NULL;
    threads_to_cpu = NULL;
    
    return topology;
}

// human readable format
void print_cpu_topology(const cpu_topology_t *topology) 
{
    if (!topology) {
        printf("Invalid topology pointer\n");
        return;
    }
    
    if (verbose) {
        printf(V1 "vCPU Topology Information:\n");
        printf("     Total vCPUs: %d\n", topology->nr_cpus);
        printf("     Number of Host Physical Sockets: %d\n", topology->nr_sockets);
        printf("     Number of Host Physical Cores: %d\n", topology->nr_cores);
        printf("\n");
    }
    
    // organize vCPUs by socket
    int socket_cpu_count[MAX_CPUS] = {0};
    int total_cpu_count = 0;
    
    for (int i = 0; i < topology->nr_cpus; i++) {
        int socket = topology->cpu_to_socket[i];
        if (socket >= 0 && socket < MAX_CPUS) {
            socket_cpu_count[socket]++;
            total_cpu_count++;
        }
    }
    
    // arrs of vCPUs by socket
    int **socket_cpus = (int **)malloc(topology->nr_sockets * sizeof(int *));
    int *socket_index = (int *)calloc(topology->nr_sockets, sizeof(int));
    
    if (!socket_cpus || !socket_index) {
        fprintf(stderr, ERR "Memory allocation failed\n");
        free(socket_cpus);
        free(socket_index);
        return;
    }
    
    for (int s = 0; s < topology->nr_sockets; s++) {
        if (socket_cpu_count[s] > 0) {
            socket_cpus[s] = (int *)malloc(socket_cpu_count[s] * sizeof(int));
            if (!socket_cpus[s]) {
                fprintf(stderr, ERR "Memory allocation failed\n");
                for (int j = 0; j < s; j++) {
                    free(socket_cpus[j]);
                }
                free(socket_cpus);
                free(socket_index);
                return;
            }
        } else {
            socket_cpus[s] = NULL;
        }
    }
    
    // fill the socket arrays
    for (int i = 0; i < topology->nr_cpus; i++) {
        int socket = topology->cpu_to_socket[i];
        if (socket >= 0 && socket < topology->nr_sockets && socket_cpus[socket]) {
            socket_cpus[socket][socket_index[socket]++] = i;
        }
    }
    
    const int cpu_width = 5;
    
    const int row_width = total_cpu_count * cpu_width + 100;
    
    // rows
    char *socket_row = (char *)calloc(row_width, sizeof(char));
    char *core_row = (char *)calloc(row_width, sizeof(char));
    char *cpu_row = (char *)calloc(row_width, sizeof(char));
    
    if (!socket_row || !core_row || !cpu_row) {
        fprintf(stderr, "Memory allocation failed\n");
        free(socket_row);
        free(core_row);
        free(cpu_row);
        for (int s = 0; s < topology->nr_sockets; s++) {
            if (socket_cpus[s]) free(socket_cpus[s]);
        }
        free(socket_cpus);
        free(socket_index);
        return;
    }
    
    // display socket by socket
    for (int s = 0; s < topology->nr_sockets; s++) {
        if (socket_cpu_count[s] == 0) continue;
        
        // socket width in characters
        int socket_width = socket_cpu_count[s] * cpu_width;
        
        // socket header
        char socket_label[32];
        sprintf(socket_label, "S%d", s);
        int label_len = strlen(socket_label);
        
        int left_pad = (socket_width - label_len - 2) / 2;
        int right_pad = socket_width - label_len - 2 - left_pad;
        
        // add socket header
        strcat(socket_row, "[");
        for (int i = 0; i < left_pad; i++) {
            strcat(socket_row, " ");
        }
        strcat(socket_row, socket_label);
        for (int i = 0; i < right_pad; i++) {
            strcat(socket_row, " ");
        }
        strcat(socket_row, "]");
        
        // vCPU IDs
        for (int i = 0; i < socket_cpu_count[s]; i++) {
            char temp[10];
            sprintf(temp, "[%3d]", socket_cpus[s][i]);
            strcat(cpu_row, temp);
        }
        
        // core/SMT markers
        int i = 0;
        while (i < socket_cpu_count[s]) {
            int current_cpu = socket_cpus[s][i];
            int current_core = topology->cpu_to_core[current_cpu];
            
            // how many vCPUs share this core?
            int smt_count = 0;
            for (int j = i; j < socket_cpu_count[s]; j++) {
                int cpu_j = socket_cpus[s][j];
                if (topology->cpu_to_core[cpu_j] == current_core) {
                    smt_count++;
                } else {
                    break;
                }
            }
            
            if (smt_count > 1) {
                // this is an SMT core with multiple CPUs
                char smt_bracket[100] = {0};
                
                // total width of the SMT bracket
                int total_bracket_width = smt_count * cpu_width;
                const char *smt_text = "SMT";
                int smt_text_len = strlen(smt_text);
                
                // padding to center "SMT"
                int left_spaces = (total_bracket_width - smt_text_len - 2) / 2; // -2 for brackets
                int right_spaces = total_bracket_width - smt_text_len - 2 - left_spaces;
                
                smt_bracket[0] = '[';
                
                // left padding
                for (int sp = 0; sp < left_spaces; sp++) {
                    strcat(smt_bracket, " ");
                }
                
                // SMT text
                strcat(smt_bracket, smt_text);
                
                // add right padding
                for (int sp = 0; sp < right_spaces; sp++) {
                    strcat(smt_bracket, " ");
                }
                
                strcat(smt_bracket, "]");
                
                // add to core row
                strcat(core_row, smt_bracket);
                
                // skip to the next core
                i += smt_count;
            } else {
                // Non-SMT vCPUs
                strcat(core_row, "[   ]");
                i++;
            }
        }
    }
    
    // print rows
    printf("%s\n", socket_row);
    printf("%s\n", core_row);
    printf("%s\n", cpu_row);
    //printf("\n");
    
    free(socket_row);
    free(core_row);
    free(cpu_row);
    
    for (int s = 0; s < topology->nr_sockets; s++) {
        if (socket_cpus[s]) free(socket_cpus[s]);
    }
    
    free(socket_cpus);
    free(socket_index);
    
    // vCPU mapping tables
    if (verbose > 2) {
        printf("vCPU to Socket Map:\n");
        for (int i = 0; i < topology->nr_cpus; i++) {
            printf("vCPU %3d -> Socket %d\n", i, topology->cpu_to_socket[i]);
        }
        printf("\n");
        
        printf("vCPU to Core Map:\n");
        for (int i = 0; i < topology->nr_cpus; i++) {
            printf("vCPU %3d -> Core %d\n", i, topology->cpu_to_core[i]);
        }
        printf("\n");
        
        printf("SMT Siblings:\n");
        for (int i = 0; i < topology->nr_cpus; i++) {
            printf("vCPU %3d siblings: ", i);
            int found = 0;
            for (int j = 0; j < topology->nr_cpus; j++) {
                if (i != j && topology->relation_matrix[i][j] == CPU_RELATION_SMT) {
                    if (found) printf(", ");
                    printf("%d", j);
                    found = 1;
                }
            }
            if (!found) printf("none");
            printf("\n");
        }
    }
}
