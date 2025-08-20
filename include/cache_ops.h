#ifndef CACHE_OPS_H
#define CACHE_OPS_H

#include "asm.h"
#include "common.h"
#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
  insert a cache line into the L2 cache
  This procedure is crafted based on Intel Xeon Scalable's
  cache replacement policies when a miss in all cache levels occurs.
  See:
  https://www.intel.com/content/www/us/en/developer/articles/technical/xeon-processor-scalable-family-technical-overview.html
  Gist: Upon a miss in all cache levels for data `x`, the CPU brings the line
  into the MLC (Mid-Level Cache), which in our case is the L2
*/
//void l2_insert_seq(u8** cands, u8* target_addr);

static inline void flush_array(u8 **addrs, u64 size)
{
    u64 i;
    for (i = 0; i < size; i++)
        _clflushopt(addrs[i]);
}

static ALWAYS_INLINE void access_stride(u8 *start, u32 stride, u32 count) 
{
    for (u32 i = 0; i < count; i++) {
        maccess(start + i * stride);
        _lfence();
        _mfence();
    }
}

/* 
  if we forward traverse the array, we may access elements after (size - 1) due
  to speculation. Backward traversal mitigates this problem, since the index will
  underflow and make the reference invalid, blocking speculative accesses.
*/
static inline void access_array_bwd(u8 **addrs, u64 size)
{
    u64 i;
    for (i = size; i > 0; i--) {
        maccess(addrs[i - 1]);
    }
}

static inline void maccess_arr(u8 **addrs, u64 size)
{
    u64 i;
    for (i = 0; i < size; i++)
        maccess(addrs[i]);
}


// By Daniel Gruss
// Rowhammer.js: https://github.com/isec-tugraz/rowhammerjs
static ALWAYS_INLINE void prime_cands_daniel(u8 **cands, u64 cnt, u64 repeat, 
                                             u64 stride, u64 block) 
{
    block = _min(block, cnt);
    for (u64 s = 0; s < cnt; s += stride) {
        for (u64 c = 0; c < repeat; c++) {
            if (cnt >= block + s) {
                access_array_bwd(&cands[s], block);
            } else {
                u32 rem = cnt - s;
                access_array_bwd(&cands[s], rem);
                access_array_bwd(cands, block - rem);
            }
        }
    }
}

static ALWAYS_INLINE void access_array(u8 **addrs, u64 size)
{
   u64 i;
   for (i = 0; i < size; i++) {
      maccess(addrs[i]);
   }
}

void traverse_cands_mt(u8 **cands, u64 cnt, EvBuildConf* tconf);

#ifdef __cplusplus
}
#endif
#endif // CACHE_OPS_H
