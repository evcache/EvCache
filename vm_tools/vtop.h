/*
 * Copyright (C) 2018-2019 VMware, Inc.
 * SPDX-License-Identifier: GPL-2.0
 *
 * and vSched->vTopology:
 * https://github.com/vSched/vtopology/tree/33626d01bd519771ed801f7c9ed586cbce58c9fb
 */
#ifndef VTOP_H
#define VTOP_H

#include <sched.h>
#include <stdbool.h>

#define MAX_CPUS (192)

typedef enum {
    CPU_RELATION_SMT,    // SMT siblings (stacking on the same core)
    CPU_RELATION_CORE,   // same core cluster/CCX
    CPU_RELATION_SOCKET, // same socket/package
    CPU_RELATION_REMOTE  // different sockets
} cpu_relation_t;

/*
 * vCPU topology struct
 * 
 * nr_cpus: Total num of vCPU(s) in the VM
 * nr_sockets: num of underlying physical CPU packages/sockets
 * nr_cores: num of physical cores backing vCPUs
 * 
 * mapping relations:  
 * cpu_to_socket[cpu_id]: returns which socket a vCPU belongs to
 * cpu_to_core[cpu_id]: returns which physical core a vCPU belongs to
 * (vCPUs with the same core ID are SMT siblings/hyperthreads on underlying hw core)
 */
typedef struct {
    int nr_cpus;                             // num of vCPUs
    int nr_sockets;                          // num of underlying sockets detected
    int nr_cores;                            // Number of physical cores
    int cpu_to_socket[MAX_CPUS];             // Maps CPU to its socket
    int cpu_to_core[MAX_CPUS];               // Maps CPU to its physical core
    int relation_matrix[MAX_CPUS][MAX_CPUS]; // relation matrix between CPUs
} cpu_topology_t;

/*
 * custom_params: Custom parameters for the detection algorithm, NULL for defaults
 *
 * Returns:
 *   Pointer to cpu_topology_t structure containing the detected topology, or NULL if detection failed
 *   The caller is responsible for freeing this memory when done
 */
cpu_topology_t *detect_vcpu_topology(void *custom_params);

void print_cpu_topology(const cpu_topology_t *topology);

//bool add_thread_to_vtop_cgroup(void);

//bool add_tid_to_vtop_cgroup(pid_t tid);

#endif /* VTOP_H */
