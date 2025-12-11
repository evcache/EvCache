# EvCache: Tapping the Power of CPU Caches in Modern Public Cloud VMs with Eviction Sets

> [!WARNING]  
> If compiling on a CVM, please first switch to the `cvm-build` branch before following the instructions below.
> If compiling within traditional VMs hosted on e.g. SKX processors, use the `master` branch.

EvCache offers a set of utilities along with
case studies that demonstrate how these tools can be used in practice.

## Setup and Building

In the root of the project directory, do:
```bash
sudo ./scripts/build.sh
```
After necessary packages are installed, a `build/` directory is created
by the script, which will further host the binaries generated.

For **future** builds after changing the source code, simply run:
```c
sudo make -j
```
inside the `build/` directory. `build.sh` is mainly a first-time setup script.

If one changes file names, add file(s), or new binaries to generate, etc., they need
to take care of changes necessary to `CMakeLists.txt`, which necessitates rerunning
`sudo cmake ..` under the `build/` directory also, before running `sudo make -j`.

## EvCache Utilities

After building, utilities such as `vev`, `vset`, `vcolor`, `vpo`, and `polluter` are
available under `build/`. See the [utilities README](./src/README.md) for
detailed usage instructions.

## Case Studies

EvCache showcases two techniques that build on these utilities.

### LCAS - LLC Contention-Aware Task Scheduling

LCAS extends the `scx_rusty` sched_ext scheduler and leverages `vset` with the
`--lcas` option to probe socket-level contention. Instructions for building and
running the scheduler can be found in [scx/README.md](./scx/README.md).

### VCPC - Virtual Color-Aware Page Cache Management

VCPC steers page cache accesses toward heavily contended LLC sets to mitigate
cache pollution in more LLC "zones" (refer to the paper for more explanation on 
this terminology). It is implemented via the `vcolor` utility; see the
[`vcolor` section](./src/README.md#vcolor) of the utilities README for details
on setup and usage.

## Optional (debug support using `-d, --debug`)

The EvCache utilities are specifically designed for running inside of Virtual Machines (VM) and one can
use the custom module and hypercall for translating a VM's VA to Host's PA when
running with the `-d, --debug` mode. `-d <1|2|3>` chooses between the verbosity level of debug info being shown.

Refer to the [gpa_hpa_km README](./kern_mods/gpa_hpa_km/README.md) for instructions on setting up this functionality.

> [!NOTE]
> * Considering the setup of the gpa_hpa module requires kernel changes to the host, usage of the `-d, --debug` option is
limited to local environment setups and not feasible in VMs provided by cloud vendors.
> * Ensure to run the program with `sudo` when using the `-d, --debug` option.
