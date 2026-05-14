# GPA/HPA debug support

The EvCache utilities are designed for running inside VMs. In local setups, the
`-d/--debug` mode can use a custom module and hypercall to translate a VM
address to the host physical address backing it.

This path is used by debug sanity checks, `vpo --use-hpa`, and `vtest`. It is
not needed for basic eviction set construction.

> [!NOTE]
> Setting up the GPA/HPA module requires host kernel changes. This is meant for
> local environments and is not feasible in VMs provided by cloud vendors.
> Run the programs with `sudo` when using this path.

## Host hypercall support

The `gpa_hpa` kernel module relies on a custom KVM hypercall implemented on the
host. The patch in `kern_mods/gpa_hpa_km/hc.diff` must be applied to the host
kernel source tree.

Tested setup:

```text
Intel-equipped host machines with Linux kernel v6.15.2
Ubuntu 24.04 LTS
VMs with Linux kernel v6.15.2
```

Download and extract the kernel:

```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.15.2.tar.xz
tar -xvf ./linux-6.15.2.tar.xz
cd linux-6.15.2
```

Place the contents of `kern_mods/gpa_hpa_km/hc.diff` into a file named
`hc.diff` at the root of the kernel source tree, then apply it:

```bash
git apply hc.diff
rm hc.diff
```

Install the prerequisites necessary to build the kernel. On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev fakeroot dwarves bc kernel-package libudev-dev libpci-dev libiberty-dev autoconf xz-utils
```

Build the kernel and boot into it:

```bash
sudo make defconfig
sudo make -j
sudo make modules
sudo make modules_install
sudo make install
sudo update-grub
```

At boot time, select the newly compiled kernel entry.

## Guest module setup

Inside the VM, the guest kernel module initiates this hypercall on behalf of a
userspace program. Build and insert the module by running:

```bash
cd kern_mods/gpa_hpa_km
sudo ./setup_mod.sh
```

The module is given the guest physical address of a userspace address. It sends
that GPA to the host hypercall. The hypercall returns the host physical address
and flags indicating properties of the frame, such as whether it is part of a
compound or huge page. These flags are not used by EvCache.

The `va_to_hpa` function in `vm_tools/gpa_hpa.c` shows the userspace side of
this path.
