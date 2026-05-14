# VCPC

VCPC is the Virtual Color-Aware Page Cache Management case study. It uses
EvCache's cache-color information to steer page cache allocation toward selected
LLC zones.

The userspace `vcolor` program allocates anonymous memory, filters pages by
cache color, and writes PFNs and colors to `/proc/vcolor_km`. The vColor kernel
module keeps per-color page lists and uses them for later page cache
allocations.

## Kernel setup

The vColor kernel module requires a custom kernel build. Patch Linux `v6.15.2`
with `kern_mods/vcolor_km/vcolor.diff` and boot into the patched kernel before
compiling and installing the module.

Download and extract the kernel:

```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.15.2.tar.xz
tar -xvf ./linux-6.15.2.tar.xz
cd linux-6.15.2
```

Place the contents of `kern_mods/vcolor_km/vcolor.diff` into a file named
`vcolor.diff` at the root of the kernel source tree, then apply it:

```bash
git apply vcolor.diff
rm vcolor.diff
```

Install build prerequisites. On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev fakeroot dwarves bc kernel-package libudev-dev libpci-dev libiberty-dev autoconf xz-utils
```

Enable vColor:

```bash
sudo make menuconfig
```

In the TUI menu, check `CONFIG_VCOLOR`, then save and exit. Build and install
the kernel:

```bash
sudo make -j
sudo make modules
sudo make modules_install
sudo make install
sudo update-grub
sudo reboot
```

At boot time, select the newly compiled kernel.

## Module setup

After booting into the vColor kernel, build and insert the module:

```bash
cd kern_mods/vcolor_km
sudo ./setup_mod.sh
```

The top-level module helper can also build and install all modules under
`kern_mods/`:

```bash
cd kern_mods
sudo ./setup_all_mods.sh
```

## Kernel side of vColor

The `vcolor_km` module:

* Tags pages and stores free ones on `colored_page_list[color]`.
* Falls back to the buddy allocator if no colored page is available.
* Hooks into `__free_frozen_pages` to divert freed pages back into the colored lists.

The `/proc/vcolor_km` interface accepts:

```text
enable / disable
flush
free <n>
<pfn> <n>
```

Reading `/proc/vcolor_km` shows status, counts per color, and last allocation
or write activity.

## Fill colored page pools

Build EvCache, then run:

```bash
cd build
sudo ./vcolor -s 10
```

This filters pages based on their L2 colors in parallel and stores them in the
kernel's per-color page buckets.

Checking the proc interface shows the current state:

```bash
sudo cat /proc/vcolor_km
```

Example output:

```text
Status: ENABLED
--------------------------------------------
Color        Free           Allocated
--------------------------------------------
 0         4636               266
 1         4473                 0
... truncated ...
Total     70044 ( 273 MiB)    266 (   1 MiB)
--------------------------------------------
last_alloc: jbd2/vda2-8
last_writer: vcolor
hottest: 0
order: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
```

`order` and `hottest` influence allocation priority when userspace `vcolor`
runs with `--vset`. Without `--vset`, there is no priority in allocation and
page cache pages are allocated sequentially from color 0 to n.

## Hotness-aware allocation

One can enable hotness-aware allocation by passing `--vset`:

```bash
sudo ./vcolor -s 10 --vset -C 3
```

`-s 10` is the candidate scaling factor for pages filtered and written to the
module. `-C 3` is the scaling factor used for filtering candidate addresses for
L2 evset construction.

Example output:

```text
-------------------------
Starting vSet Monitoring
-------------------------
[INFO] Using 10 cores to construct EvCands complex in parallel
... truncated ...
[+] Total evsets built: 2048/2048 (100.00%)
[INFO] Minimal eviction set size: 11 (Size: 27.50 MiB)
LLC color hotness
Color  0:  21.42%
Color  1:  14.07%
Color  2:  57.26%
... truncated ...
Hottest color:  2
```

## `vtest`

`vtest` validates filtered page allocation. Allocations for `vtest` are
intercepted in `vma_alloc_folio_noprof`. It uses the GPA/HPA debug path to
check the host physical addresses of allocated pages for matching L2 color
bits.

Requirements:

* The GPA/HPA hypercall must be set up on the host.
* The `gpa_hpa` kernel module must be loaded in the guest.
* The `vcolor` kernel module must be loaded in the guest.
* Run `vtest` with `sudo` for valid reads and writes to `/proc/gpa_hpa`.

Example:

```bash
sudo ./vtest -n 10
```

Example output:

```text
Page      0 HPA: 0x8685c2000
Page      1 HPA: 0x3d1142000
... truncated ...
[INFO] 10/10 matched color
Color of first page: 2
[INFO] Sleeping. Ctrl+C to cancel.
```

As seen above, all 10 pages share the same low bits used as L2 cache color
bits.
