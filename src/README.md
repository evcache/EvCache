# EvCache utilities

After running `scripts/build.sh` the following tools are available in `build/`.
Change into that directory before running the commands below:

```bash
cd build
```

For using any `-d/--debug` options, set up the GPA<->HPA translation module as
described in [the gpa_hpa_km README](../kern_mods/gpa_hpa_km/README.md).

## `vev` (vEvict)

Help menu:

```bash
./vev -h
Usage: ./vev <cache_level> [options]

Arguments:
  cache_level             Target cache level: L2, L3/LLC (required)

Options:
  -s, --cands-scaling N   Specify candidate scaling factor [default: 3]
  -v, --verbose LEVEL     Show statistics and logs (1=basic, 2=detailed, 3=very detailed) [default: 0]
  -d, --debug LEVEL       Show debugging information (1-3) [default: 0]
                          Requires gpa_hpa tool to be set up on both the guest and host.
  -n, --num-sets N        Number of page offsets to build evsets for (regular mode)
  -u, --uncertain-sets N  Granular mode: number of L2 uncertain sets per offset [default: all]
  -f, --evsets-per-l2 N   Number of eviction sets to build per L2 uncertain set [default: 1]
  --vtop                  Topology-aware evset construction, by integrating with vTopology

Parallelization options:
  -c, --num-core N        N number of cores to leverage in core-parallelism for L3/LLC evset construction
                          N must be even (each main thread paired with a helper thread)
                          N=0: Auto-utilize maximum available cores (default if not specified)
  -u, --uncertain-sets N  Enables granular mode with N L2 uncertain sets per offset
  -f, --evsets-per-l2 N   Build N eviction sets at each L2 uncertain set
  -o, --num-offsets N     Requires -u. Number of page offsets to work on [default: 1]

Help:
  -h, --help              Display this help message and exit
```

vEvict constructs eviction sets for the L2 cache or the LLC. It can operate in **granular** mode
where sets are grouped by uncertain L2 sets (AKA L2 colors).
When an eviction set maps to a certain L2 color zone on the host we do not know 
the exact host color and the mapping may change between runs, but the sets remain 
distinct (AKA non-canonical or virtual L2 colors). 
The `-f` option specifies how many eviction sets are built for 
each page‑offset × L2‑color combination.

1. Example input:
```bash
./vev L3 -n 10 -c 6
```

* `-n 10` simply builds all evsets at 10 page offsets (similar concept from the LLCFeasible codebase).
* `-c 6` limits vevict's parallelization to utilization of 6 cores.

Ouput:
```bash
[NOTE] Auto detected 20 LLC slices for physical socket. Double check this value.
[+] latencies: L1d: 42 | L2: 50 | L3: 72 | DRAM: 231
[+] threshold: L1d: 49 | L2: 63 | L3: 147 | interrupt: 1155

[NOTE] Requested sets: 1024; this would build a total of 1024 sets after shifting
[+] Built 1024/1024 total sets (16/16 uncertain sets; rest are shifted)
[INFO] Completed L2 evsets build | 17.567ms
[INFO] Using 10 cores to construct EvCands complex in parallel
├─ Thread   2: Filtered 21120 candidate lines to  1298 | 23.624ms
├─ Thread   1: Filtered 21120 candidate lines to  1349 | 22.511ms
├─ ... truncated ...
├─ Thread   5: Filtered 21120 candidate lines to  1301 | 22.293ms
├─ Thread   3: Filtered 21120 candidate lines to  1336 | 23.059ms
[INFO] Built EvCands Complex in Parallel | 69.457ms
[INFO] Starting 5 thread pairs for L3 evset construction
├─ Thread pair 4: working on cores 8 (main) and 9 (helper)
├─ ... truncated ...
├─ Thread pair 2: working on cores 4 (main) and 5 (helper)
[INFO] Progress:
├─ Pair 3: offset 0xc0 Done
├─ Pair 1: offset 0x80 Done
[+] Thread pair 1 done: 640/640 evsets successfully built (1 offsets)
├─ Pair 2: offset 0x100 Done
[+] Thread pair 2 done: 640/640 evsets successfully built (1 offsets)
├─ Pair 4: offset 0x0 Done
[+] Thread pair 4 done: 640/640 evsets successfully built (1 offsets)
├─ Pair 0: offset 0x40 Done
[+] Thread pair 0 done: 640/640 evsets successfully built (1 offsets)
├─ Pair 3: offset 0x140 Done
[+] Thread pair 3 done: 1280/1280 evsets successfully built (2 offsets)
[+] Parallel evset construction completed | 1840.773ms
[+] Total built: 3840/3840 L3 evsets (100.00%)
[INFO] Minimal eviction set size: 11
[INFO] Program completed | 1.997s
```

Evidently, each pair works on a different addresses from different offsets to further 
speed up evset construction.


2. Example input:
```bash
sudo ./vev L2 -n 128 -d 1
```

Output:
```bash
... truncated ...

[+] Built 128/128 total sets (2/2 uncertain sets; rest are shifted)
[D1] L2 evset 0 address mapping sanity check:
  T_ad: 0x555c4ce72c40 -> HPA 0xa02a2fc40 (L2 SIB 0x3f1)
  [ 0]: 0x555c4ceffc40 -> HPA 0x588cafc40 [L2 SIB match]
  ... (15 more addresses) | show with -d 2

[D1] L2 evset 1 address mapping sanity check:
  T_ad: 0x555c4ce83c40 -> HPA 0xa0b158c40 (L2 SIB 0x231)
  [ 0]: 0x555c4cf42c40 -> HPA 0x3587258c40 [L2 SIB match]
  ... (15 more addresses) | show with -d 2

[D1] L2 colors: 0xf 0x8
[D1] [+] All L2 colors of uncertain L2 evsets are unique.
[INFO] Completed L2 evsets build | 17.405ms
[INFO] Program completed | 0.064s
```

* `-n 128` Requests construction of 128 L2 evsets. Notably, from one constructed L2 evsets, 63 more can be 
constructed by simply shifting their page offsets, thus the ending message on the first output line.
* `-d 1` sanity checks the validity of constructed L2 evsets.
* Run with `sudo` for valid read(s)/write(s) to the `gpa_hpa` kernel module to make the sanity check hypercalls.


3. Example input:
```bash
./vev l3 -u 16 -f 4 -o 64
```

Output:
```bash
... truncated ...
[INFO] Completed L2 evsets build | 16.058ms
[INFO] Using 10 cores to construct EvCands complex in parallel
... truncated ...
[INFO] Built EvCands Complex in Parallel | 66.977ms
[INFO] Starting 5 thread pairs for granular L3 evset construction
[INFO] Expected: 4 eviction set(s) per L2 uncertain set (16 sets) across 64 offsets
├─ Thread pair 2: working on cores 4 (main) and 5 (helper) - 205 assignments (820 evsets)
├─ Thread pair 0: working on cores 0 (main) and 1 (helper) - 205 assignments (820 evsets)
├─ Thread pair 4: working on cores 8 (main) and 9 (helper) - 204 assignments (816 evsets)
├─ Thread pair 3: working on cores 6 (main) and 7 (helper) - 205 assignments (820 evsets)
├─ Thread pair 1: working on cores 2 (main) and 3 (helper) - 205 assignments (820 evsets)
[INFO] Progress:
[+] Thread pair 0 completed: 820/820 evsets built
[+] Thread pair 2 completed: 818/820 evsets built
[+] Thread pair 3 completed: 820/820 evsets built
[+] Thread pair 1 completed: 820/820 evsets built
[+] Thread pair 4 completed: 815/816 evsets built
[+] granular construction completed | 1462.417ms
[+] Total evsets built: 4093/4096 (99.93%)
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
[INFO] Program completed | 1.615s
```

This example was ran on a 10-core VM. Without specifying `-c`, the program tries to utilize 
as many cores as possbile.
* `-u, -f, -o` are the parameters used in *granular* evset construction.
* `-u` is the number of filtered addresses based on L2 colors from which L3 evsets are further constructed.
* `-f` option controls how many evsets are built for each page‑offset × L2‑color bit combination. 


4. Example input:
```bash
./vev L3 -n 0 --vtop
```

Output:
```bash
... truncated ...
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

In this example, `--vtop` is passed in which allows the program to know the vCPUs topology
on the host which guides pinning of main and helper threads to properly be placed on 
non-SMT same-socket vCPUs during evset construction.

With an asymmetric topology and no-vtop approach, evset construction would highly likely
fail in such scenarios:
```bash
./vev L3 -n 0
```

Output:
```
... truncated ...

[INFO] Built L2 evset | 3.25ms
[INFO] Filtered 21120 candidate lines to 1318 | 19.760ms
[-] failed to build L3 eviction set (evset size: 22)
[-] No evset returned!
[INFO] Program completed | 0.200s
```

## vset

Help menu:

```bash
./vset -h
Usage: ./vset [options]

Options:
  -v, --verbose LEVEL     Show statistics (1=basic, 2=detailed, 3=full) [default: 0]
  -d, --debug LEVEL       Show debugging information (1-3) [default: 0]
  -c, --num-core N        Number of cores to use for monitoring threads
  --activity-freq         With -G 0, plot L3 eviction activity frequency
  -m, --max-records N     Activity frequency: number of records to collect [default: 300]
  -G, --graph TYPE        Generate graph data (see types below)
  -a, --append NAME       Append NAME to generated plot filenames
  -u, --uncertain-sets N  Granular mode: number of L2 uncertain sets per offset
  -f, --evsets-per-l2 N   Eviction sets to use per L2 set [default: 1]
  -o, --num-offsets N     Number of page offsets to cover [default: 1]
  -w, --wait-time N       Wait time in microseconds between prime and probe [default: 7000]
  -M, --max-time N        For evrate-wait: max wait time in microseconds.
                          For occ-heatmap-l2color: number of iterations.
                          [default: 7000]
  -t, --time-step N       For evrate-wait: time step in microseconds.
                          For occ-heatmap-l2color: wait between iterations in milliseconds.
                          [default: 25]
  --live                  Live hotness monitoring mode
                           -t sets print interval in milliseconds
  --vtop                  Topology-aware mode with vTopology integration
  --lcas                  Multi-socket LLC occupancy monitoring
                           -t sets update interval in milliseconds
  --fix-wait              Disable automatic wait-time adjustments
  --perf                  Measure prime/probe performance
  --fraction-check        Report L3 color coverage for generated eviction sets
  --alpha-rise A          EWMA rise alpha [default: 0.85]
  --alpha-fall A          EWMA fall alpha [default: 0.85]

Graph Types (for -G):
  0, eviction-freq        L3 eviction activity over time
  1, evrate-wait          Eviction rate vs wait time
  2, occ-heatmap-l2color  Occupancy grouped by L2 color
  3, evrate-time          Eviction rate over time graph
  4, l2color-dist         Host/guest L2 color distribution

Other:
  -r, --remap             Requires debug module (-d). See how often host page remapping breaks evset.

Help:
  -h, --help              Display this help message and exit
```

vSet can monitor LLC sets, report eviction rate (normalized to per ms), page color distribution, and more (refer to help menu). 
It must run as `sudo` because it moves itself to a high‑priority
cgroup to not allow inner-VM workloads introduce noise during its probing phase. 
Run `../scripts/setup_vset.sh` (assuming ran inside the `build/` dir), to setup the cgroups that
allow this; rebooting removes them so the script must be **re‑run**.

1. Example input
```bash
sudo ./vset -u 16 -f 4 -o 64 --live
```

Output:
```bash
... truncated ...
LLC color hotness
Wait: 7 ms
Color  0:  26.00%
Color  1:  22.93%
Color  2:  25.03%
Color  3:  18.49%
Color  4:  15.75%
Color  5:  14.52%
Color  6:  15.44%
Color  7:  17.49%
Color  8:  21.52%
Color  9:  20.80%
Color 10:  18.28%
Color 11:  15.22%
Color 12:  15.48%
Color 13:  27.78%
Color 14:  21.73%
Color 15:  22.27%
```

similar to `vev`'s examples, `-u`, `-f`, `-o` are parameters to guide number of evsets 
to construct, to further utilize in color hotness monitoring.

Notes on usage of `-u`:
* A system with 1 MiB L2 cache per core has 1024 sets (log2(1024)=10 index bits). 6 bits come from the
page offset, leaving 4 L2 color bits, so 2^4 = 16 is the maximum `u` to choose for such system. 
A CPU with a 2 MiB L2 would have 2048 sets, yielding 5 color bits and up to 32
possible colors, so `-u 1-32` can be appropriate in that case. 


Notes on usage of `-f`:
* The `-f` option controls how many eviction sets are built for each page‑offset x L2‑color combination. 
Our system has 2048 L3 sets per slice, and 20 slices. Considering the 5 color bits for the 
LLC, we would have 1 bit which is unknown from filtered L2 candidates, and 20x slice possibility, 
so 2^1 * 20 = 40 is the choice of `f` on our system. Different number of cores (thus LLC slices) can 
allow different ranges of selection for `-f`.


The following layout visualizes the choice of these parameters on our system:
```text
our system has 1024 L2 sets: thus 10 bits to index the L2 cache,
and the LLC has 2048 sets per slice, so 11 bits to index the LLC per slice.
(SIB = Set Index Bit)
     abbbbbbbbbbcccccc
     ||         |-----> `c`s are for cache line offset indexing
     ||
     ||-> `b`s are L2 SIB (subset of the L3 set indexing)
     |
     |-> `a` is used only in LLC indexing (i.e. 11th bit in our system)

If we have an eviction set at a given `b` combination (an L2 set), then
2^a * n_slices is the number of possible LLC eviction sets at that L2
color x page‑offset. Our system has 20 slices -> 40 LLC sets per combo.
```


2. Example input:
```bash
sudo ./vset -u 16 -f 4 -o 64 --vtop --lcas
```
Note: Per-socket LLC occupancy monitoring is possible without setting up the custom scheduler.


`--vtop --lcas` can be used without a custom scheduler to measure
per‑socket contention.  To bias scheduling toward the least polluted socket via
`scx_rusty`, see the setup instructions in [this README](../scx/README.md).


Output:
```bash
... truncated ...
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

In this case the contention level in both detected sockets are the same, so there would 
be no reported preference.

By default, the `-w` period between prime and probe is 7ms. This value automatically shrinks 
when we lose visibility in the contention levels (e.g. both sockets hitting 100%), and expands 
(resets back to 7ms) when contention drops low (e.g. below 20%). If you want the waiting period to 
be fixed however, pass the `--fix-wait` parameter.


Many different monitoring behaviors can be **graphed** using vset. See `-G` options.
The program would automatically both write the data, and run the graphing command, which 
will generate the graph (alongside the data) under the `data/` directory inside the `build/` dir.


If you want to use a graphing mode of vset (`-G <0|1|2|3|4>`), **make sure** to activate the 
virtual environment set up by the `build.sh` script by doing:
```bash
source venv/bin/activate
```
under the `scripts/` directory.


And prepend your `vset` command with `--preserve-env=HOME,PATH`, such as:
```bash
sudo --preserve-env=HOME,PATH ./vset --activity-freq -G 0
```

in order to prevent errors like `unable to find xyz` libraries used to generate the graphs (e.g. matplotlib),
as they are accessible through the user's PATH, but not found when running `vset` with `sudo`.

## `vpo` (poisoner)

Refered to as `poisoner` in the paper. Can target fine-grained poisoning in specific LLC zones.

Help menu:

```bash
./vpo -h
Usage: ./vpo [options]

Options:
  --use-gpa               Thrash the LLC using pages filtered using GPA LLC color bits
  --use-hpa               Thrash the LLC using pages filtered using Host LLC color bits (requires custom hypercall).
  --dist-only             With --use-gpa, show host color distribution
  -z, --scale N           Scale 10MiB buffer for GPA/HPA modes by N
  -a, --append NAME       Append NAME to generated plot filenames
  -u, --uncertain-sets N  Number of L2 colors to use
  -f, --evsets-per-l2 N   Eviction sets per color [default: 1]
  -o, --num-offsets N     Number of offsets [default: 1]
  -t, --wait-time US      Wait time between prime and thrash [default: 300]
  -c, --num-core N        Number of cores (even value)
  -v, --verbose LEVEL     Verbosity level
  -d, --debug LEVEL       Debug level (prints host colors)

Examples:
  ./vpo -u 16 -f 2 -o 64 -t 7000
  ./vpo -c 4 -u 8 -f 1 -o 32

  -h, --help              Display this help and exit
```

Thrashes the LLC with eviction sets. The intensity can be tuned using `-u, -f, -o`.

The `--use-gpa` and `--use-hpa` select GPA or
host‑physical filtered pages; these modes were used to generate the GPA/HPA
pollution slowdown figures in the paper. `--use-gpa` scales a 10 MiB buffer with
`-z`, does not combine with `-u`, `-f` or `-o`, and does not further filter sets
by LLC mapping. `--use-hpa` supports those options to further filter by L2
colors and offsets.


Input example:
```bash
sudo ./vpo -u 1 -f 40 -o 64 --use-hpa
```

Output:
```
... truncated ...
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
[INFO] Host LLC color picked: 0x1a
[INFO] Filtered 1283/2558 eviction sets to host LLC color 0x1a (5.01 MiB)
[+] Started poisoning on 1 colors. Ctrl-C to cancel.
```
Note that the `--use-hpa` mode requires the `gpa_hpa` module to be set up. Refer to [this README](../kern_mods/gpa_hpa_km/README.md).

* Use `sudo` for proper read(s)/write(s) to the module for the hypercall.


Example input:
```bash
sudo ./vpo --use-gpa -z 15 -c 10
```

Output:
```bash
... truncated ...
[INFO] Allocated 150.00 MiB (38400 pages)
[INFO] Filtering pages for GPA LLC color 0...
[INFO] Filtered 1206/38400 pages to GPA LLC color 0 (4.71 MiB)
[+] Thrashing LLC with 1206 pages across 1 threads. Ctrl-C to cancel.
```

* `-c 10` will further distribute the continuous access of the filtered pages among 10 threads to increase intensity.
* `-z 15` is the scaling factor for the default 10MiB region (10 * 15 = 150MiB region in this example).
* Use `sudo` to read `/proc/self/pagemap/<pid>` of the process to filter based on GPA.

## vtest

Tests allocation of pages filtered by `vcolor`.  Requires the `gpa_hpa` debug
module to be loaded.  The only option is `-n` for the number of pages to test:

```bash
sudo ./vtest -n 4
```

## vcolor

Help menu:
```bash
Usage: ./vcolor [options]

Options:
  -s, --cands-scaling N   Scaling factor for filtering pages [default: 3]
  -C, --vset-scale N      Scaling factor when building vset sets [default: 3]
  -t, --sleep-time SEC    Sleep SEC seconds between insertions [default: 0]
  -c, --num-cores N       Number of threads to use
  -v, --verbose LEVEL     Verbosity level [default: 0]
  -d, --debug LEVEL       Debug level (prints host colors)
  --vset                  After insertion, monitor color hotness live
    --scan-period MS      Print interval in milliseconds [default: 1000]
    --scan-wait US        Wait time during prime+probe [default: 7000]
    --alpha-rise A        EWMA rise alpha [default: 0.85]
    --alpha-fall A        EWMA fall alpha [default: 0.85]

Examples:
  ./vcolor -s 10
  ./vcolor -s 10 --vset -C 3 --scan-period 1000 --scan-wait 7000

  -h, --help              Display this help and exit
```

This userspace tool writes to the `vcolor_km` which utilizes functions 
implemented in the custom vColor kernel.


See [the vcolor_km README](../kern_mods/vcolor_km/README.md#vtest-program)
for more information and setup instructions.


1. Example input:
```
sudo ./vcolor -s 10
```

Output:
```
[INFO] Completed L2 evsets build | 16.762ms
├─ Thread   1: Filtered 70400 candidate lines to  4305 | 73.218ms
├─ Thread   3: Filtered 70400 candidate lines to  4281 | 73.409ms
├─ Thread   7: Filtered 70400 candidate lines to  4355 | 73.909ms
├─ Thread   0: Filtered 70400 candidate lines to  4902 | 74.322ms
├─ Thread   9: Filtered 70400 candidate lines to  4374 | 74.863ms
├─ Thread   2: Filtered 70400 candidate lines to  4362 | 74.944ms
├─ Thread   6: Filtered 70400 candidate lines to  4355 | 74.710ms
├─ Thread   5: Filtered 70400 candidate lines to  4509 | 75.084ms
├─ Thread   8: Filtered 70400 candidate lines to  4306 | 76.086ms
├─ Thread   4: Filtered 70400 candidate lines to  4480 | 77.279ms
├─ Thread   2: Filtered 70400 candidate lines to  4210 | 69.948ms
├─ Thread   1: Filtered 70400 candidate lines to  4298 | 73.151ms
├─ Thread   3: Filtered 70400 candidate lines to  4327 | 72.507ms
├─ Thread   0: Filtered 70400 candidate lines to  4473 | 73.054ms
├─ Thread   5: Filtered 70400 candidate lines to  4287 | 73.043ms
├─ Thread   4: Filtered 70400 candidate lines to  4486 | 73.150ms
[INFO] Program completed | 0.769s
```

This command will simply filter pages based on their L2 colors 
in parallel and store them in the kernel's per-color page buckets 
to be later allocated as page cache pages to requesting programs.


Checking the `/proc/vcolor_km` interface would show:
```bash
Status: ENABLED
--------------------------------------------
Color        Free           Allocated
--------------------------------------------
 0         4636               266
 1         4473                 0
 2         4305                 0
 3         4298                 0
 4         4362                 0
 5         4210                 0
 6         4281                 0
 7         4327                 0
 8         4480                 0
 9         4486                 0
10         4509                 0
11         4287                 0
12         4355                 0
13         4355                 0
14         4306                 0
15         4374                 0
Total     70044 ( 273 MiB)    266 (   1 MiB)
--------------------------------------------
last_alloc: jbd2/vda2-8
last_writer: vcolor
hottest: 0
order: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
```

For easier monitoring use `watch` to periodically update the stats.

One can enable hotness-aware allocation by passing `--vset`:
2. Example input:
```bash
sudo ./vcolor -s 10 --vset -C 3
```
* Use `sudo` for vcolor userspace to have proper write access to the `/proc/vcolor_km` interface.
* `-s 10` is candidate scaling factor for pages to be filtered to later be written to the module.
* `-C 3` is the scaling factor used for filtering candidate addresses for L2 evset construction.

Output:
```bash
... truncated ...
├─ Thread   3: Filtered 70400 candidate lines to  4329 | 83.320ms
... truncated ...
-------------------------
Starting vSet Monitoring
-------------------------
[INFO] Using 10 cores to construct EvCands complex in parallel
... truncated ...
├─ Thread   4: Filtered 21120 candidate lines to  1373 | 23.600ms
[INFO] Built EvCands Complex in Parallel | 74.823ms
[INFO] Starting 5 thread pairs for granular L3 evset construction
... truncated ...
[+] Total evsets built: 2048/2048 (100.00%)
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
LLC color hotness
Color  0:  21.42%
Color  1:  14.07%
Color  2:  57.26%
Color  3:  16.25%
Color  4:  15.03%
Color  5:  17.25%
Color  6:  19.19%
Color  7:  19.34%
Color  8:  19.73%
Color  9:  20.48%
Color 10:  15.53%
Color 11:  19.02%
Color 12:  13.43%
Color 13:  13.57%
Color 14:  13.74%
Color 15:  15.62%
Hottest color:  2
```
This list would be re-rendered/updated every `-t` milliseconds (`1000` by default).

In this example vColor would prioritize allocation from pages mapped to the 
LLC zone that is already polluted the most:
```bash
Status: ENABLED
--------------------------------------------
Color        Free           Allocated
--------------------------------------------
 0         4562                 0
 1         4223                 0
 2            0              4656
 3         4336                 0
 4         4153                 0
 5         4596                 0
 6         3425               904
 7         4344                 0
 8         4223                 0
 9         4235                 0
10         4305                 0
11         4393                 0
12         4291                 0
13         4437                 0
14         5034                 0
15         4282                 0
Total     64839 ( 253 MiB)   5560 (  21 MiB)
--------------------------------------------
last_alloc: jbd2/vda2-8
last_writer: vcolor
hottest: 2
order: 2 6 8 13 0 1 3 4 5 7 9 10 11 12 14 15
```

And fall back to other colors once available pages in the prioiritized color run out.

## polluter

The `vpo` binary is referred to as *poisoner* in the paper. It targets controlled poisoning in specific parts 
of the LLC. `polluter` however performs a general LLC pollution by repeatedly accessing a buffer with a configurable stride. 
The work can be split across multiple threads for faster access.

### Help menu

```bash
usage: ./polluter [options]
options:
  -s <stride>    stride in bytes (must be >0)
  -b <buf_size>  buffer size in bytes (must be >= stride)
  -t <threads>   number of threads (>0)
  -m <measures>  number of measurements before exit (0=infinite)
  -h             display this help message
```

### Example

```bash
./polluter -s 64 -b 32000000 -t 4 -m 5
```

Output:
```bash
measurement 1: 297.10 million accesses/sec
measurement 2: 298.23 million accesses/sec
measurement 3: 298.61 million accesses/sec
measurement 4: 298.39 million accesses/sec
measurement 5: 297.95 million accesses/sec
average: 298.06 million accesses/sec
```

* `-s` sets the stride, in bytes, used when walking the buffer.
* `-b` specifies the buffer size.
* `-t` chooses how many threads share the work of touching the buffer.
* `-m` controls the number of throughput measurements before exiting. When `-m` is omitted or set to `0`, `polluter` prints a running `throughput:` line every second. Setting `-m N` prints `N` measurements followed by the average and then exits.

