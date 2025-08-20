#!/usr/bin/env python3
import sys, os
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <datafile>")
    sys.exit(1)

fname = sys.argv[1]

host_colors = []
matrix = []
guest_colors = 0
with open(fname) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            if line.startswith('# guest_colors:'):
                guest_colors = int(line.split(':')[1].strip())
            continue
        parts = line.split()
        host_colors.append(int(parts[0]))
        row = [float(x) for x in parts[1:]]
        matrix.append(row)
if guest_colors == 0 and matrix:
    guest_colors = len(matrix[0])

matrix = np.array(matrix)

cmap = LinearSegmentedColormap.from_list('dist', ['#E6F3FF', '#4169E1'])

plt.rcParams['font.family'] = 'serif'

fig, ax = plt.subplots(figsize=(12,8))
im = ax.imshow(matrix, cmap=cmap, vmin=0, vmax=100, aspect='auto')

for i in range(len(host_colors)):
    for j in range(guest_colors):
        value = matrix[i, j]
        color = 'white' if value > 50 else 'black'
        ax.text(j, i, f'{value:.0f}%', ha='center', va='center', color=color, fontsize=12, weight='bold')

ax.set_xticks(np.arange(guest_colors))
ax.set_yticks(np.arange(len(host_colors)))
ax.tick_params(axis='both', which='major', labelsize=10)
ax.set_xlabel('Guest L2 Color', fontsize=12)
ax.set_ylabel('Host L2 Color (non-canonical)', fontsize=12)

for i in range(len(host_colors) + 1):
    ax.axhline(i - 0.5, color='black', linewidth=0.5)
for j in range(guest_colors + 1):
    ax.axvline(j - 0.5, color='black', linewidth=0.5)

plt.colorbar(im, ax=ax, label='Percentage (%)')

plt.tight_layout()
base = os.path.splitext(os.path.basename(fname))[0]
os.makedirs('data', exist_ok=True)
outfile = f"data/{base}.pdf"
plt.savefig(outfile)
print(f"Heatmap saved to {outfile}")
