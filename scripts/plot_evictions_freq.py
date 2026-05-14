#!/usr/bin/env python3

import argparse
import os
from typing import List, Tuple

import matplotlib.pyplot as plt
import numpy as np


def read_eviction_freq_data(path: str) -> Tuple[List[float], List[int]]:
    times: List[float] = []
    evicted: List[int] = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                times.append(float(parts[0]))
                evicted.append(int(parts[1]))
            except ValueError:
                continue
    return times, evicted


def plot_eviction_freq(datafile: str, show: bool) -> None:
    times, evicted = read_eviction_freq_data(datafile)
    if not times:
        print(f'No data points in {datafile}')
        return

    total_points = len(times)
    total_evictions = sum(evicted)
    timeline = times[-1] if times else 0

    plt.rcParams['font.family'] = 'serif'
    fig, ax = plt.subplots(figsize=(12, 4))

    times_arr = np.array(times)
    evicted_arr = np.array(evicted)
    
    if total_points > 10000:
        ax.plot(times_arr, evicted_arr, 'k-', linewidth=0.5, alpha=0.8)
        ax.fill_between(times_arr, 0, evicted_arr, color='black', alpha=0.3)
    elif total_points > 1000:
        eviction_times = times_arr[evicted_arr == 1]
        no_eviction_times = times_arr[evicted_arr == 0]
        
        if len(no_eviction_times) > 0:
            ax.scatter(no_eviction_times, [0.5] * len(no_eviction_times), 
                      c='#DDDDDD', s=1, alpha=0.5, label='No Eviction')
        if len(eviction_times) > 0:
            ax.scatter(eviction_times, [0.5] * len(eviction_times), 
                      c='black', s=2, alpha=0.8, label='Eviction')
    else:
        width = timeline / total_points if total_points > 0 else 1
        
        eviction_mask = evicted_arr == 1
        eviction_times = times_arr[eviction_mask]
        no_eviction_times = times_arr[~eviction_mask]
        
        if len(no_eviction_times) > 0:
            ax.bar(no_eviction_times, [1] * len(no_eviction_times), 
                  width=width, align='edge', color='#DDDDDD', 
                  edgecolor='none', label='No Eviction')
        if len(eviction_times) > 0:
            ax.bar(eviction_times, [1] * len(eviction_times), 
                  width=width, align='edge', color='black', 
                  edgecolor='none', label='Eviction')

    ax.set_xlim(0, timeline * 1.05 if timeline > 0 else 1)
    ax.set_ylim(0, 1)
    ax.set_yticks([])
    ax.set_xlabel('Time (ms)')
    ax.set_title(f'L3 Cache Eviction Activity (Total Events: {total_evictions}, Timeline: {timeline:.1f} ms)')
    ax.grid(axis='x', color='#E0E0E0', alpha=0.5)
    
    if total_points <= 1000:
        ax.legend()
    
    plt.tight_layout()

    out_pdf = f"{datafile}.pdf"
    plt.savefig(out_pdf, bbox_inches='tight')
    print(f'Graph saved to {out_pdf}')

    if show:
        plt.show()
    else:
        plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('datafile')
    parser.add_argument('-a', action='store_true')
    args = parser.parse_args()

    datafile = os.path.abspath(args.datafile)
    plot_eviction_freq(datafile, args.a)


if __name__ == '__main__':
    main()
