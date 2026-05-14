#!/usr/bin/env python3
import os
import sys
import time
import argparse
import re
from collections import defaultdict

''' 
chmod +x ./thread_cpu_profile.py ./thread_cpu_profile.py test -n 10 -t 100 
it also counts occurrences of threads spawned from the process's name 
(which is mainly why we use it) -n is how many times we sample /proc's 
data to see which core the process and its threads are running on -t is 
how frequently we make those -n many checks (in milliseconds)
'''

def parse_args():
    p = argparse.ArgumentParser(description="Sample per-thread CPU usage over time for a given process.")
    p.add_argument("name", help="Name of the target process (matches /proc/[pid]/comm)")
    p.add_argument("-n", "--samples", type=int, required=True, help="Number of times to sample (in addition to the initial sample)")
    p.add_argument("-t", "--interval", type=int, required=True, help="Interval between samples, in milliseconds")
    return p.parse_args()

def find_pid_by_name(name):
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm", "r") as f:
                comm = f.read().strip()
            if comm == name:
                return int(pid)
        except IOError:
            continue
    return None

def list_threads(pid):
    task_dir = f"/proc/{pid}/task"
    tids = []
    for tid in os.listdir(task_dir):
        if tid.isdigit():
            tids.append(int(tid))
    return tids

def read_thread_state_and_cpu(pid, tid):
    stat_path = f"/proc/{pid}/task/{tid}/stat"
    with open(stat_path, "r") as f:
        data = f.read()
    right_paren_pos = data.rfind(')')
    if right_paren_pos == -1:
        raise ValueError("Invalid stat file format")
    remaining = data[right_paren_pos + 2:]
    fields = remaining.split()
    state = fields[0]
    cpu = int(fields[36])
    return state, cpu

def get_cpu_topology():
    cpu_dirs = []
    sys_cpu_dir = "/sys/devices/system/cpu"
    for name in os.listdir(sys_cpu_dir):
        if re.fullmatch(r"cpu\d+", name):
            cpu_dirs.append(int(name[3:]))
    cpu_dirs.sort()
    cpu_to_socket = {}
    socket_to_coreset = defaultdict(set)
    for cpu in cpu_dirs:
        topo = f"{sys_cpu_dir}/cpu{cpu}/topology"
        try:
            with open(f"{topo}/physical_package_id", "r") as f:
                socket_id = int(f.read().strip())
        except Exception:
            socket_id = 0
        try:
            with open(f"{topo}/core_id", "r") as f:
                core_id = int(f.read().strip())
        except Exception:
            core_id = cpu
        cpu_to_socket[cpu] = socket_id
        socket_to_coreset[socket_id].add(core_id)
    sockets = sorted(socket_to_coreset.keys())
    cores_per_socket = {s: len(socket_to_coreset[s]) for s in sockets}
    return cpu_dirs, cpu_to_socket, cores_per_socket

def main():
    args = parse_args()
    pid = find_pid_by_name(args.name)
    if pid is None:
        sys.exit(f"No process named '{args.name}' found.")
    tids = list_threads(pid)
    if not tids:
        sys.exit(f"No threads found for PID {pid}.")
    cpu_list, cpu_to_socket, cores_per_socket = get_cpu_topology()
    if not cpu_list:
        cpu_count = os.cpu_count() or 1
        cpu_list = list(range(cpu_count))
        cpu_to_socket = {c: 0 for c in cpu_list}
        cores_per_socket = {0: cpu_count}
    counts = {cpu: 0 for cpu in cpu_list}
    total_running_checks = 0
    for tid in tids:
        try:
            state, cpu = read_thread_state_and_cpu(pid, tid)
            if state == 'R' and cpu in counts:
                counts[cpu] = counts.get(cpu, 0) + 1
                total_running_checks += 1
        except (FileNotFoundError, ValueError, IndexError):
            continue
    for _ in range(args.samples):
        time.sleep(args.interval / 1000.0)
        for tid in tids:
            try:
                state, cpu = read_thread_state_and_cpu(pid, tid)
                if state == 'R' and cpu in counts:
                    counts[cpu] = counts.get(cpu, 0) + 1
                    total_running_checks += 1
            except (FileNotFoundError, ValueError, IndexError):
                continue
    print(f"Process: {args.name} (PID {pid}), Threads: {len(tids)}")
    print(f"Samples: {args.samples + 1} × {len(tids)} = {(args.samples + 1) * len(tids)} total checks")
    print(f"Running state checks: {total_running_checks}")
    print()
    if total_running_checks == 0:
        print("No threads found in running state during sampling.")
        return
    print("CPU  % Time")
    for cpu in cpu_list:
        pct = (counts.get(cpu, 0) / total_running_checks) * 100
        print(f"{cpu:>3}  {pct:6.2f}%")
    print()
    socket_counts = defaultdict(int)
    for cpu, c in counts.items():
        socket_counts[cpu_to_socket.get(cpu, 0)] += c
    sockets = sorted(socket_counts.keys() | cores_per_socket.keys())
    total_cores = sum(cores_per_socket.values())
    print(f"Sockets: {len(cores_per_socket)}")
    print("Socket  Cores  % Time")
    for s in sockets:
        cores = cores_per_socket.get(s, 0)
        pct = (socket_counts.get(s, 0) / total_running_checks) * 100 if total_running_checks else 0.0
        print(f"{s:>6}  {cores:>5}  {pct:6.2f}%")
    if len(cores_per_socket) > 0:
        uniform = len(set(cores_per_socket.values())) == 1
        if uniform:
            any_cores = next(iter(cores_per_socket.values()))
            print()
            print(f"Total cores: {total_cores}, Sockets: {len(cores_per_socket)}, Cores per socket: {any_cores}")

if __name__ == "__main__":
    main()
