# Cache parameters

This page collects the parameter details shared by `vev`, `vset`, and `vpo`.
The tools describe these parameters in their own usage sections, but this page
keeps the cache-geometry reasoning in one place.

## `-n`

`-n` is used by regular construction modes to request eviction sets across page
offsets. For L2, one built eviction set can be shifted to other page offsets to
cover more sets. This is why output may report both uncertain sets and total
sets, for example:

```text
[+] Built 128/128 total sets (2/2 uncertain sets; rest are shifted)
```

For L3 regular construction, `-n` requests how many page offsets are used.

## `-u`

`-u` is the number of uncertain L2 sets to use in granular mode. These are also
called L2 colors in the code and output. The maximum useful value depends on
the number of L2 set index bits outside the page offset.

A system with 1 MiB L2 cache per core has 1024 sets. `log2(1024) = 10`, so
there are 10 L2 set index bits. Six bits come from the cache-line offset inside
the page offset, leaving 4 L2 color bits. This gives `2^4 = 16` possible
uncertain L2 sets.

A CPU with a 2 MiB L2 has 2048 sets, yielding 5 color bits and up to 32
possible colors, so `-u 1-32` can be appropriate in that case.

If `-u` is not specified in granular construction, the tools use all uncertain
L2 sets detected for the machine.

## `-f`

`-f` controls how many LLC eviction sets are built for each page-offset x
L2-color combination. The maximum depends on LLC uncertainty and the number of
LLC slices.

On our system, the L2 has 1024 sets, and the LLC has 2048 sets per slice. The
L2 therefore has 10 set index bits, and the LLC has 11 set index bits per
slice. One extra bit is used only in LLC indexing. With 20 slices, this gives:

```text
2^1 * 20 = 40
```

So `-f 40` is the maximum on that system. On a different machine, the maximum
can be different. If a larger value is requested, the tool exits with:

```text
[-] Requested -f xy but only max f is ab
```

where both values are calculated at runtime.

## `-o`

`-o` controls how many page offsets are used in granular construction. It is
commonly used with `-u` and `-f`.

For example:

```bash
./vev L3 -u 16 -f 4 -o 64
```

asks for 4 eviction sets per uncertain L2 set across 64 page offsets.

## `-c`

`-c` limits how many physical cores are used by the tool. It is capped by the
number of physical cores visible to the VM, not by SMT threads. This avoids
accepting a core count that the tool cannot sensibly use for paired main/helper
construction or parallel probing.

If the requested value is too large, the tool exits with:

```text
[-] Requested -c xy but only max c is ab
```

For tools that use paired construction, even values are expected because each
pair uses one main thread and one helper thread.

## Bit layout example

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
color x page-offset. Our system has 20 slices -> 40 LLC sets per combo.
```
