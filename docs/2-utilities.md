# EvCache utilities

After running `scripts/build.sh`, the tools are available under `build/`.
Change into that directory before running the commands below:

```bash
cd build
```

For using any `-d/--debug` option, set up the GPA/HPA translation path as
described in [GPA/HPA debug support](./4-gpa-hpa-debug.md).

The parameters `-n`, `-u`, `-f`, `-o`, and `-c` are used across multiple tools.
Their practical use is described below with each tool. More detail on how these
values relate to cache geometry is in [Cache parameters](./3-cache-parameters.md).

## `vev` (vEvict)

`vev` constructs eviction sets for the L2 cache or the LLC. It can operate in
regular mode, where `-n` requests sets across page offsets, or in granular mode,
where sets are grouped by uncertain L2 sets, also referred to as L2 colors in
the code and output.

The common form is:

```bash
./vev <L2|L3> [options]
```

For the full option list:

```bash
./vev -h
```

### Regular L3 construction

```bash
./vev L3 -n 10 -c 6
```

`-n 10` builds eviction sets at 10 page offsets. `-c 6` limits parallel
construction to 6 physical cores. If `-c` is larger than the number of physical
cores available to the VM, the tool exits with a short error.

Example output:

```text
[NOTE] Auto detected 20 LLC slices for physical socket. Double check this value.
[+] latencies: L1d: 42 | L2: 50 | L3: 72 | DRAM: 231
[+] threshold: L1d: 49 | L2: 63 | L3: 147 | interrupt: 1155

[NOTE] Requested sets: 1024; this would build a total of 1024 sets after shifting
[+] Built 1024/1024 total sets (16/16 uncertain sets; rest are shifted)
[INFO] Completed L2 evsets build | 17.567ms
[INFO] Using 10 cores to construct EvCands complex in parallel
... truncated ...
[+] Parallel evset construction completed | 1840.773ms
[+] Total built: 3840/3840 L3 evsets (100.00%)
[INFO] Minimal eviction set size: 11
[INFO] Program completed | 1.997s
```

Each thread pair works on addresses from different offsets to speed up evset
construction.

### L2 construction with debug checks

```bash
sudo ./vev L2 -n 128 -d 1
```

`-n 128` requests 128 L2 evsets. From one constructed L2 evset, more can be
constructed by shifting page offsets, which is why the output reports both
uncertain sets and total sets. `-d 1` sanity checks the constructed L2 evsets
using the GPA/HPA debug path. Run with `sudo` for valid reads and writes to the
`gpa_hpa` kernel module.

Example output:

```text
[+] Built 128/128 total sets (2/2 uncertain sets; rest are shifted)
[D1] L2 evset 0 address mapping sanity check:
  T_ad: 0x555c4ce72c40 -> HPA 0xa02a2fc40 (L2 SIB 0x3f1)
  [ 0]: 0x555c4ceffc40 -> HPA 0x588cafc40 [L2 SIB match]
  ... (15 more addresses) | show with -d 2

[D1] L2 colors: 0xf 0x8
[D1] [+] All L2 colors of uncertain L2 evsets are unique.
[INFO] Completed L2 evsets build | 17.405ms
[INFO] Program completed | 0.064s
```

### Granular L3 construction

```bash
./vev L3 -u 16 -f 4 -o 64
```

`-u`, `-f`, and `-o` guide granular evset construction. `-u` selects how many
uncertain L2 sets to use. `-f` controls how many LLC eviction sets are built for
each page-offset x L2-color combination. `-o` controls how many page offsets are
covered.

Example output:

```text
[INFO] Completed L2 evsets build | 16.058ms
[INFO] Using 10 cores to construct EvCands complex in parallel
... truncated ...
[INFO] Starting 5 thread pairs for granular L3 evset construction
[INFO] Requested: 4 eviction set(s) per L2 uncertain set (16 sets) across 64 offsets
... truncated ...
[+] granular construction completed | 1462.417ms
[+] Total evsets built: 4093/4096 (99.93%)
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
[INFO] Program completed | 1.615s
```

Without specifying `-c`, the program tries to utilize as many cores as possible.

### Topology-aware construction

```bash
./vev L3 -n 0 --vtop
```

`--vtop` lets the program use the detected vCPU topology to pin main and helper
threads to non-SMT same-socket vCPUs during evset construction.

Example output:

```text
[INFO] vTop Done | 8.041ms
[               S0                ][     S1      ]
[   ][   ][   ][   ][   ][   ][   ][   ][   ][   ]
[  0][  3][  5][  6][  7][  8][  9][  1][  2][  4]
[INFO] Pinning main to vCPU 0; helper to vCPU 3

[INFO] Filtered 21120 candidate lines to 1224 | 21.764ms
[+] L3 evset built | size: 11 | 1.870ms pruning
[+] Built 1/1 L3 eviction sets | 71.704ms
[INFO] Program completed | 0.128s
```

With an asymmetric topology and no `--vtop`, evset construction is more likely
to fail.

## `vset`

`vset` monitors LLC activity using eviction sets. It can report live hotness,
measure eviction rates, write graph data, and run the LCAS per-socket monitoring
mode.

Run the cgroup setup script from the repository root before using modes that
move `vset` to the high-priority cgroup:

```bash
sudo ./scripts/setup_vset.sh
```

The cgroups are cleared on reboot, so rerun the script after each restart.

For the full option list:

```bash
./vset -h
```

### Live LLC color hotness

```bash
sudo ./vset -u 16 -f 4 -o 64 --live
```

As with `vev`, `-u`, `-f`, and `-o` control how many granular eviction sets are
built and used for monitoring.

Example output:

```text
LLC color hotness
Wait: 7 ms
Color  0:  26.00%
Color  1:  22.93%
Color  2:  25.03%
... truncated ...
Color 15:  22.27%
```

The wait period between prime and probe defaults to 7ms. In some modes this
value can automatically shrink when visibility is lost and reset when
contention drops low. Pass `--fix-wait` to keep the waiting period fixed.

### LCAS monitor mode

```bash
sudo ./vset -u 16 -f 4 -o 64 --vtop --lcas
```

`--vtop --lcas` can be used without a custom scheduler to measure per-socket
contention. To bias scheduling toward the least polluted socket via
`scx_rusty`, see [LCAS](./6-case-studies/1-lcas.md).

Example output:

```text
[INFO] vTop: vCPU topology layout:
[             S0             ][        S1        ]
[   ][   ][   ][   ][   ][   ][   ][   ][   ][   ]
[  0][  5][  6][  7][  8][  9][  1][  2][  3][  4]
... truncated ...
Per-socket LLC hotness monitoring (Ctrl+C to stop)
Wait: 7 ms
Socket 0:  20.24%
Socket 1:  18.20%
LCAS: preferred socket: []
```

If the contention level in both detected sockets is similar, there is no
reported preference.

### Graph modes

Many monitoring behaviors can be graphed using `vset -G`. The program writes
the data and calls the graphing command, which generates figures and data under
the `data/` directory inside `build/`.

If using graphing modes with `sudo`, see
[Graphing and data](./5-graphing-and-data.md) for the virtual environment and
`--preserve-env=HOME,PATH` notes.

## `vpo` (poisoner)

`vpo` is referred to as `poisoner` in the paper. It continuously thrashes the
LLC and can target finer-grained LLC zones.

For the full option list:

```bash
./vpo -h
```

The intensity can be tuned with `-u`, `-f`, and `-o`. `--use-gpa` and
`--use-hpa` select GPA- or host-physical-filtered pages. These modes were used
to generate the GPA/HPA pollution slowdown figures in the paper.

`--use-gpa` scales a 10 MiB buffer with `-z`. It does not combine with `-u`,
`-f`, or `-o`, and does not further filter sets by LLC mapping. `--use-hpa`
supports `-u`, `-f`, and `-o` to further filter by L2 colors and offsets.

### HPA-filtered poisoning

```bash
sudo ./vpo -u 1 -f 40 -o 64 --use-hpa
```

Example output:

```text
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
[INFO] Host LLC color picked: 0x1a
[INFO] Filtered 1283/2558 eviction sets to host LLC color 0x1a (5.01 MiB)
[+] Started poisoning on 1 colors. Ctrl-C to cancel.
```

`--use-hpa` requires the GPA/HPA debug setup. Use `sudo` for proper reads and
writes to the module.

### GPA-filtered poisoning

```bash
sudo ./vpo --use-gpa -z 15 -c 10
```

Example output:

```text
[INFO] Allocated 150.00 MiB (38400 pages)
[INFO] Filtering pages for GPA LLC color 0...
[INFO] Filtered 1206/38400 pages to GPA LLC color 0 (4.71 MiB)
[+] Thrashing LLC with 1206 pages across 10 threads. Ctrl-C to cancel.
```

`-z 15` scales the default 10 MiB region to 150 MiB. `-c 10` distributes the
continuous access of the filtered pages among 10 threads. Use `sudo` to read
`/proc/self/pagemap` for GPA filtering.

## `vcolor`

`vcolor` is the userspace tool used by the VCPC case study. It writes to
`vcolor_km`, which uses the custom vColor kernel changes. See
[VCPC](./6-case-studies/2-vcpc.md) for the kernel and module setup path.

For the full option list:

```bash
./vcolor -h
```

### Fill colored page pools

```bash
sudo ./vcolor -s 10
```

This filters pages based on their L2 colors in parallel and stores them in the
kernel's per-color page buckets. The pages can later be allocated as page cache
pages.

Example output:

```text
[INFO] Completed L2 evsets build | 16.762ms
├─ Thread   1: Filtered 70400 candidate lines to  4305 | 73.218ms
... truncated ...
[INFO] Program completed | 0.769s
```

Checking `/proc/vcolor_km` shows the status and per-color counts:

```bash
sudo cat /proc/vcolor_km
```

### Hotness-aware allocation

```bash
sudo ./vcolor -s 10 --vset -C 3
```

`--vset` enables hotness-aware allocation. `-s 10` is the candidate scaling
factor for pages written to the module. `-C 3` is the scaling factor used for
candidate addresses during L2 evset construction.

## `vtest`

`vtest` validates filtered page allocation. It requires the `gpa_hpa` debug
module and the `vcolor` module to be loaded. The only option is `-n`, the
number of pages to test:

```bash
sudo ./vtest -n 4
```

See [VCPC](./6-case-studies/2-vcpc.md) for more context on how `vtest` is used.
