# EvCache overview

EvCache offers a set of utilities and case studies for recovering and using
cache-related information inside VMs and CVMs. The utilities build and use
eviction sets to expose information that is otherwise hidden from the guest,
such as virtual L2 colors, LLC contention, and, in local debug setups, host
physical address information.

The repository has three main parts. The EvCache utilities are the programs
under `build/` after compilation, such as `vev`, `vset`, `vpo`, `vcolor`, and
`vtest`. The two case studies show that the information exposed by EvCache is
actionable. LCAS uses `vset` with `scx_rusty` to guide scheduling away from
polluted LLC domains. VCPC uses `vcolor` and a custom vColor kernel to steer
page cache allocations by cache color. The remaining scripts, kernel modules,
and patches support these utilities and case studies.

> [!WARNING]
> If compiling on a CVM, switch to the `cvm-build` branch before following the
> instructions below. If compiling within traditional VMs hosted on e.g. SKX
> processors, use the `master` branch.

## Setup and building

In the root of the project directory, run:

```bash
sudo ./scripts/build.sh
```

After the necessary packages are installed, a `build/` directory is created by
the script. The generated binaries are placed there.

For future builds after changing the source code, run:

```bash
sudo make -j
```

inside the `build/` directory. `build.sh` is mainly a first-time setup script.

If one changes file names, adds source files, or adds new binaries, update
`CMakeLists.txt` and rerun CMake from the `build/` directory before running
`make` again:

```bash
sudo cmake ..
sudo make -j
```

## What setup is needed?

Basic eviction set construction with `vev` does not need custom kernel support.
After building, start from:

```bash
cd build
./vev -h
```

`vset` modes that move the process to a high-priority cgroup need the cgroup
setup script. The cgroups are cleared on reboot, so rerun the script after each
restart:

```bash
sudo ./scripts/setup_vset.sh
```

The `-d/--debug` options, `vpo --use-hpa`, and `vtest` need the GPA/HPA debug
path. This requires a host kernel hypercall patch and the guest `gpa_hpa`
module. See [GPA/HPA debug support](./4-gpa-hpa-debug.md).

Graphing modes in `vset` write data and call Python plotting scripts. See
[Graphing and data](./5-graphing-and-data.md) for the virtual environment and
`sudo --preserve-env=HOME,PATH` notes.

LCAS needs the sched_ext/scx setup described in
[LCAS](./6-case-studies/1-lcas.md). VCPC needs the vColor kernel patch and
module setup described in [VCPC](./6-case-studies/2-vcpc.md).

## Where to go next

For utility usage, see [EvCache utilities](./2-utilities.md). This includes
`vev`, `vset`, `vpo`, `vcolor`, and `vtest`.

For cache- and topology-dependent parameter choices, see
[Cache parameters](./3-cache-parameters.md). This page explains how to choose
values such as `-n`, `-u`, `-f`, `-o`, and `-c`.

For local debug support using host physical addresses, see
[GPA/HPA debug support](./4-gpa-hpa-debug.md).

For graphing modes and generated data files, see
[Graphing and data](./5-graphing-and-data.md).

For the two case studies, see [LCAS](./6-case-studies/1-lcas.md) and
[VCPC](./6-case-studies/2-vcpc.md).
