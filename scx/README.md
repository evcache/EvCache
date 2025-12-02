# scx_rusty setup

This repository keeps only the pieces needed to run the `scx_rusty`
scheduler (which is further extended to LCAS) for EvCache. For full 
documentation on sched_ext and its schedulers, see 
the [upstream README](https://github.com/sched-ext/scx/blob/main/README.md).

## Kernel requirements

Tested on Linux v6.15.2. Enable the following options when building the kernel:

```
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

```
scripts/config --disable DEBUG_INFO_NONE
scripts/config --enable DEBUG_INFO_DWARF5
scripts/config --enable DEBUG_INFO_BTF
scripts/config --enable SCHED_CLASS_EXT
```

We removed other schedulers from the upstream tree and kept only what is needed
for `scx_rusty`.

## Building

First, install prerequisites to build the Rust project. E.g. on Ubuntu:
```bash
sudo apt update
sudo apt install -y build-essential libssl-dev llvm lld libelf-dev meson cargo rustc clang cmake pkg-config protobuf-compiler
```

From under the `scx` directory, run:
```bash
meson compile -C build scx_rusty
```

The binary will be placed at `scx/build/release/scx_rusty` upon a successful build.

## Using with `vset`

`vset` can monitor LLC hotness per socket using `--vtop --lcas` even without a
custom scheduler. To have `scx_rusty` prioritize the least polluted socket:

1. Run `../scripts/setup_vset.sh`.
2. Start the scheduler:
   ```bash
   sudo ./scx_rusty --no-load-balance
   ```
3. In another terminal, run `vset` to probe the sockets' occupancy:
   ```bash
   sudo ./vset --vtop --lcas -u 16 -f 8 -o 64
   ```
   Half of the `-f` sets for each color are probed on one socket and the rest
   on the other. When thresholds indicate a preferred socket, the order is
   written to the BPF map `lcas_dom_order` which `scx_rusty` reads.

If inner‑VM activity causes interference during `vset`'s wait period, run the
scheduler with a longer FIFO slice:

```bash
sudo ./scx_rusty --no-load-balance -f -o 15000
```
This grants ~15 ms per process, enough for `vset`'s ~10 ms cycle to complete
without inner-VM noise.
