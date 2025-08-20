# Custom vColor Kernel

## Overview

The vColor kernel module requires a custom kernel build. You must patch Linux `v6.15.2` 
with [vcolor.diff](./vcolor.diff) and boot into the patched kernel before compiling and installing the module.

---

## Setup Instructions

### 1. Download and extract the kernel

```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.15.2.tar.xz
tar -xvf ./linux-6.15.2.tar.xz
cd linux-6.15.2
```

### 2. Apply the patch

Place the contents of `vcolor.diff` into a file named `vcolor.diff` at the root of the kernel source tree, then apply it:

```bash
git apply vcolor.diff
rm vcolor.diff
```

### 3. Install prerequisites

Refer to [the official requirements](https://www.kernel.org/doc/html/v4.13/process/changes.html).

For example on Ubuntu one can install the packages using:
```bash
sudo apt update
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev fakeroot dwarves bc kernel-package libudev-dev libpci-dev libiberty-dev autoconf xz-utils
```

### 4. Enable vColor, build, and install the kernel

```bash
sudo make menuconfig
```
In the TUI menu, check the `CONFIG_VCOLOR` option and then save & exit the configuration.

Next:
```bash
sudo make -j
sudo make modules
sudo make modules_install
sudo make install
sudo update-grub
```

Lastly reboot, but make sure at boot time you select the newly compiled kernel:
```bash
sudo reboot
```

---

## Kernel side of vColor's operations

The `vcolor` userspace program (read about its usage in [this README](../../src/README.md#vcolor)) 
allocates anonymous memory, filters pages by host cache color, 
and writes the PFN and color to `/proc/vcolor_km`.

The `vcolor_km` kernel module:
* Tags pages and stores free ones on `colored_page_list[color]`.
* Falls back to the buddy allocator if no colored page is available.
* Hooks into `__free_frozen_pages` to divert freed pages back into the colored lists.

### `/proc/vcolor_km` Control

* `enable` / `disable`: Turn coloring on/off.
* `flush`: Return all colored pages to the buddy allocator and clear tracking.
* `free <n>`: Release only color `n` pages while keeping tags.
* `<pfn> <n>`: Tag individual PFNs.

The module tracks:

* `tagged_frames`: All pages ever tagged, preserving metadata even after free.
* `colored_page_list[n]`: Free pages available per color.

Reading `/proc/vcolor_km` shows status, counts per color, and last allocation/write activity. Example:

```bash
$ sudo cat /proc/vcolor_km
Status: ENABLED
--------------------------------------------
Color        Free           Allocated
--------------------------------------------
 0            0              1348
 1          932               308
 2         1326                 0
 3         1353                 0
 4         1364                 0
 5         1394                 0
 6         1312                 0
 7         1276                 0
 8         1404                 0
 9         1299                 0
10         1349                 0
11         1287                 0
12         1250                 0
13         1288                 0
14         1303                 0
15         1324                 0
Total     19461 (  76 MiB)   1656 (   6 MiB)
--------------------------------------------
last_alloc: jbd2/vda2-8
last_writer: vcolor
hottest: 0
order: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
```

`order` and `hottest` influence allocation priority when userspace `vcolor` runs with `--vset`.
Without `--vset`, there would be no priority in allocation and pages cache pages 
are allocated sequentially (color 0 -> n).

---

## `vtest` Program

The `vtest` program validates filtered page allocation:

* Allocations for `vtest` are intercepted in `vma_alloc_folio_noprof`.
* It uses the `gpa_hpa` tool to check the HPA of allocated pages for matching L2 color bits.

**Requirements:**

* `gpa_hpa` hypercall must be set up on the host (see [the gpa_hpa_km README](../gpa_hpa_km/README.md)).
* The `gpa_hpa` kernel module must be loaded in the guest.
* The `vcolor` kernel module must be loaded in the guest.
* Run `vtest` with `sudo` for valid read/write from/to `/proc/gpa_hpa`.

### Example

```bash
$ sudo ./vtest -n 10
Page      0 HPA: 0x8685c2000
Page      1 HPA: 0x3d1142000
Page      2 HPA: 0x9e1de2000
Page      3 HPA: 0xc6d792000
Page      4 HPA: 0x326572000
Page      5 HPA: 0x16290a2000
Page      6 HPA: 0xc8a672000
Page      7 HPA: 0xbe4f22000
Page      8 HPA: 0x19e8b02000
Page      9 HPA: 0x1677df2000
[INFO] 10/10 matched color
Color of first page: 2
[INFO] Sleeping. Ctrl+C to cancel.
```

As seen above, all 10 pages share the same low 16 bits (`0x2000`), which are the same L2 cache color bits.
