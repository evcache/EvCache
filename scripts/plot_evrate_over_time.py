#!/usr/bin/env python3
"""Plot EWMA eviction rates over time from multiple vset -G 3 outputs."""

import argparse
import os
from typing import List, Tuple

import matplotlib.pyplot as plt
import numpy as np


def parse_input_specs(spec_args: List[str]) -> List[Tuple[str, str]]:
    """Parse comma-separated "path label" pairs from -i arguments."""
    joined = " ".join(spec_args)
    pairs = [p.strip() for p in joined.split(',') if p.strip()]
    if not pairs:
        raise ValueError("no input files provided")
    out: List[Tuple[str, str]] = []
    for pair in pairs:
        parts = pair.split()
        if len(parts) < 2:
            raise ValueError(f"invalid input spec '{pair}'")
        path = parts[0]
        label = " ".join(parts[1:])
        out.append((path, label))
    return out


def read_ewma_values(path: str) -> List[float]:
    vals: List[float] = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            idx = 5 if len(parts) >= 6 else 2 if len(parts) >= 3 else -1
            if idx != -1:
                try:
                    vals.append(float(parts[idx]))
                except ValueError:
                    pass
    return vals


def determine_axis(max_seconds: int) -> Tuple[int, str, List[float]]:
    """Choose time unit and tick positions based on total duration."""
    if max_seconds >= 4 * 3600:
        unit = 3600
        label = 'Time (hours)'
        intervals = 4
    elif max_seconds >= 60:
        unit = 60
        label = 'Time (minutes)'
        intervals = 3
    else:
        unit = 1
        label = 'Time (seconds)'
        intervals = 3

    import math
    step = max_seconds / intervals
    step = int(math.ceil(step / unit)) * unit
    ticks_sec = list(range(0, max_seconds, step))
    if not ticks_sec or ticks_sec[-1] != max_seconds:
        ticks_sec.append(max_seconds)
    ticks = [t / unit for t in ticks_sec]
    return unit, label, ticks


def main():
    parser = argparse.ArgumentParser(description='Plot EWMA eviction rates over time')
    parser.add_argument('-i', nargs='+', required=True,
                        help='comma-separated list of "path label" pairs')
    parser.add_argument('-o', type=float, metavar='OPACITY',
                        help='line opacity percentage (0-100)')
    parser.add_argument('--separate', action='store_true',
                        help='generate individual PDFs for each input file')
    args = parser.parse_args()

    try:
        specs = parse_input_specs(args.i)
    except ValueError as e:
        parser.error(str(e))

    if args.o is not None:
        if args.o < 0 or args.o > 100:
            parser.error("opacity must be between 0 and 100")
        opacity = args.o / 100.0
    else:
        opacity = 1.0

    datasets = []
    max_len = 0

    abs_paths = []
    for rel_path, label in specs:
        abs_path = os.path.abspath(rel_path)
        if not os.path.isfile(abs_path):
            parser.error(f"input file not found: {rel_path}")
        values = read_ewma_values(abs_path)
        if not values:
            parser.error(f"no data in file: {rel_path}")
        datasets.append((label, values))
        abs_paths.append(abs_path)
        if len(values) > max_len:
            max_len = len(values)

    unit, xlabel, xticks = determine_axis(max_len)

    plt.rcParams['font.family'] = 'serif'
    fig, ax = plt.subplots(figsize=(12, 6))

    prop_cycle = plt.rcParams['axes.prop_cycle']
    colors = prop_cycle.by_key()['color']

    for i, (label, values) in enumerate(datasets):
        x = np.arange(len(values)) / unit
        color = colors[i % len(colors)]
        ax.plot(x, values, label=label, linewidth=1, alpha=opacity, color=color)

    ax.set_xlabel(xlabel)
    ax.set_ylabel('EWMA Eviction Rate (%/ms)')
    ax.set_xticks(xticks)
    ax.legend()
    ax.grid(True)
    plt.tight_layout()

    first_path = abs_paths[0]
    outdir = os.path.dirname(first_path) or os.getcwd()
    os.makedirs(outdir, exist_ok=True)
    if len(datasets) > 1:
        first_base = os.path.basename(first_path)
        base = f"evrate-time-all{len(datasets)}-{first_base}"
    else:
        base = os.path.basename(first_path)
    outfile = os.path.join(outdir, f"{base}_evrate_over_time.pdf")
    plt.savefig(outfile)
    print(f'Graph saved to {outfile}')

    if args.separate and len(datasets) > 1:
        for i, ((label, values), abs_path) in enumerate(zip(datasets, abs_paths)):
            fig_single, ax_single = plt.subplots(figsize=(12, 6))
            
            x = np.arange(len(values)) / unit
            color = colors[i % len(colors)]
            ax_single.plot(x, values, label=label, linewidth=1, alpha=1.0, color=color)
            
            ax_single.set_xlabel(xlabel)
            ax_single.set_ylabel('EWMA Eviction Rate (%/ms)')
            ax_single.set_xticks(xticks)
            ax_single.set_ylim(0, 100)
            ax_single.legend()
            ax_single.grid(True)
            plt.tight_layout()
            
            single_outdir = os.path.dirname(abs_path) or os.getcwd()
            os.makedirs(single_outdir, exist_ok=True)
            single_base = os.path.basename(abs_path)
            single_outfile = os.path.join(single_outdir, f"{single_base}_evrate_over_time.pdf")
            plt.savefig(single_outfile)
            print(f'Separate graph saved to {single_outfile}')
            plt.close(fig_single)


if __name__ == '__main__':
    main()
