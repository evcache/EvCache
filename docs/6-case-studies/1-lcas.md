# LCAS

LCAS is the LLC Contention-Aware Scheduling case study. It uses EvCache's
per-socket LLC contention measurements to guide scheduling decisions.

`vset --vtop --lcas` probes socket-level LLC contention. The extended
`scx_rusty` scheduler reads the preferred socket order from the BPF map
`lcas_dom_order` and can prioritize the least polluted socket.

## Kernel requirements

Tested on Linux v6.15.2. Enable the following options when building the kernel:

```text
CONFIG_BPF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
```

If some options do not appear in the configuration TUI, make sure debug info is
enabled first:

```bash
scripts/config --disable DEBUG_INFO_NONE
scripts/config --enable DEBUG_INFO_DWARF5
scripts/config --enable DEBUG_INFO_BTF
scripts/config --enable SCHED_CLASS_EXT
```

## Building `scx_rusty`

This repository keeps only the pieces needed to run `scx_rusty`, which is
further extended for LCAS. For full upstream sched_ext documentation, see the
upstream `scx` repository.

Install prerequisites to build the Rust project. On Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential libssl-dev llvm lld libelf-dev meson cargo rustc clang cmake pkg-config protobuf-compiler
```

From under the `scx` directory, run:

```bash
meson compile -C build scx_rusty
```

The binary will be placed at:

```text
scx/build/release/scx_rusty
```

## Monitor-only mode

`vset` can monitor LLC hotness per socket using `--vtop --lcas` even without a
custom scheduler:

```bash
sudo ./scripts/setup_vset.sh
cd build
sudo ./vset --vtop --lcas -u 16 -f 8 -o 64
```

Example output:

```text
Per-socket LLC hotness monitoring (Ctrl+C to stop)
Wait: 7 ms
Socket 0:  20.24%
Socket 1:  18.20%
LCAS: preferred socket: []
```

If the contention level in both detected sockets is similar, there is no
reported preference.

## Scheduler-guided mode

To have `scx_rusty` prioritize the least polluted socket:

```bash
sudo ./scripts/setup_vset.sh
```

Start the scheduler:

```bash
cd scx/build/release
sudo ./scx_rusty --no-load-balance
```

In another terminal, run `vset`:

```bash
cd build
sudo ./vset --vtop --lcas -u 16 -f 8 -o 64
```

Half of the `-f` sets for each color are probed on one socket and the rest on
the other. When thresholds indicate a preferred socket, the order is written to
the BPF map `lcas_dom_order`, which `scx_rusty` reads.

If inner-VM activity causes interference during `vset`'s wait period, run the
scheduler with a longer FIFO slice:

```bash
sudo ./scx_rusty --no-load-balance -f -o 15000
```

This grants about 15 ms per process, enough for `vset`'s cycle to complete
without inner-VM noise.

For the meaning of `-u`, `-f`, and `-o`, see
[Cache parameters](../3-cache-parameters.md).
