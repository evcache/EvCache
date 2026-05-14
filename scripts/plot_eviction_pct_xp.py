#!/usr/bin/env python3
"""Plot eviction percentage experiment with stage annotations."""

import argparse
import os
import matplotlib.pyplot as plt


def read_data(path):
    secs = []
    rates = []
    ewmas = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) >= 3:
                secs.append(int(parts[0]))
                rates.append(float(parts[1]))
                ewmas.append(float(parts[2]))
    return secs, rates, ewmas


def main():
    p = argparse.ArgumentParser(description='Plot custom eviction percentage experiment')
    p.add_argument('data', help='data file produced by vset -G 3')
    args = p.parse_args()

    secs, rates, ewmas = read_data(args.data)
    x = [s + 1 for s in secs]

    base = os.path.splitext(os.path.basename(args.data))[0]
    out_dir = os.path.dirname(args.data)
    output = os.path.join(out_dir, base + '-ev-pct-xp.pdf')

    plt.rcParams['font.family'] = 'serif'
    fig, ax = plt.subplots(figsize=(12, 6))

    ax.step(x, rates, where='post', color='#5dade2', label='Eviction %')
    ax.plot(x, ewmas, linestyle=':', color='#8e44ad', label='EWMA')

    ax.set_xlim(0, 90)
    ax.set_ylim(0, 110)
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Eviction (%)')
    ax.grid(True)

    # stage separators
    ax.axvline(30, linestyle='--', color='gray')
    ax.axvline(60, linestyle='--', color='gray')

    ymax = ax.get_ylim()[1]
    stages = [(15, '1ms', 'Manual'), (45, '7ms', 'Idle'), (75, '7ms', 'nginx')]
    for xpos, wait, label in stages:
        ax.text(xpos, ymax - 5, f'{wait}\n{label}', ha='center', va='top',
                bbox=dict(boxstyle='square', facecolor='white', edgecolor='black'))

    ax.legend()
    plt.tight_layout()
    fig.savefig(output)
    print(f'Graph saved to {output}')


if __name__ == '__main__':
    main()
