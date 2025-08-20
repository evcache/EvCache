# `gpa_hpa` kernel module

This kernel module, with the use of a custom KVM Hyper Call (HC) implemented on the host machine, 
enables user space processes in VMs to retrieve the physical address used on the host that backs
their emulated address.

## Adding the HC support to the host kernel

> [!NOTE]
> Tested on:
> Intel-equipped host machines with linux kernel `v6.15.2` (Ubuntu `24.04` LTS distro)
> VMs with linux kernel `v6.15.2` (Ubuntu `24.04` LTS distro)

In order to support this new hypercall, [the hc.diff patch](./hc.diff) needs to be applied
to the host's kernel source tree. Follow the steps below:
```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.15.2.tar.xz
tar -xvf ./linux-6.15.2.tar.xz
cd linux-6.15.2
```

Now at the root of the 6.15.2 source tree, download/paste the `hc.diff` file and its content into a 
file called `hc.diff`.

Next, apply the diff to the source tree, and then delete the diff file:
```bash
git apply hc.diff
rm hc.diff
```

Install prerequisites necessary to build the kernel as listed [here](https://www.kernel.org/doc/html/v4.13/process/changes.html).

For example, on Ubuntu, the necessary packages can be installed using:
```bash
sudo apt update
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev fakeroot dwarves bc kernel-package libudev-dev libpci-dev libiberty-dev autoconf xz-utils
```

Now build the kernel and boot into the new custom kernel:
```bash
sudo make defconfig
sudo make -j
sudo make modules
sudo make modules_install
sudo make install
sudo update-grub
```

Finally, at boot time, make sure you select the newly compiled kernel entry.

## Module setup in the guest machine (VM)

A custom kernel module inside the VM initiates this HC on behalf of a userspace program.
You can build and insert the module by running the following script:
```bash
sudo ./setup_mod.sh
```

This module is given the Guest Physical Address (GPA) of a user space program in the VM, and passes it to the HC.
After the HC finds the Host Physical Address used to back the given address, it passes results back.
Results include the translated address and flags indicating the properties of the frame i.e., compound/huge page, etc.
These flags are **not** used in the EvCache project.

### Function to make this hypercall inside a VM

The `va_to_hpa` function in the `vm_tools/gpa_hpa.c` file demonstrates the implementation of this 
process for a program executed inside a VM.
