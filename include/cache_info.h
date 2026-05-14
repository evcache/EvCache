#ifndef CACHE_INFO_H
#define CACHE_INFO_H

#include "common.h"
#include "config.h"
#include "asm.h"
#include "bitwise.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { // all sizes in Bytes
    CacheLevel level;
    u32 cl_size; // cache line size
    u32 n_cl_bits; // num cache line bits / log2ceil(cl_size)
    u32 n_sets;    // for L3, this would be sum in all slices
    u32 n_sets_one_slice; // unset for L1/2
    u32 n_set_idx_bits;
    u32 n_ways;
    u32 n_slices; // set to 1 for L1/2
    u32 size;  // for L1/2, this is per core, for L3 is per socket
    i32 unknown_sib; // *S*et *I*ndex *B*its under control from userspace
} CacheInfo;

extern CacheInfo l1_info, l2_info, l3_info;

extern u32 g_n_uncertain_l2_sets;
extern u64 g_l3_cnt;

typedef struct {
    u64 l1d,
        l2,
        l3,
        dram,

        l1d_thresh,
        l2_thresh,
        l3_thresh,
        interrupt_thresh;
} CacheLats; // latencies

i32 calc_unknown_sib(CacheInfo* c);

void init_l1_info(CacheInfo* info);

void init_l2_info(CacheInfo* info);

void init_l3_info(CacheInfo* info);

void init_cache_info(void);

static u64 ALWAYS_INLINE cache_uncertainty(CacheInfo* c)
{
    u64 bits_under_ctrl = PAGE_SHIFT;
    u64 set_bits_under_ctrl = bits_under_ctrl - c->n_cl_bits;
    if (set_bits_under_ctrl >= c->n_set_idx_bits) {
        return c->n_slices;
    } else {
        return (1ull << (c->n_set_idx_bits - set_bits_under_ctrl)) * c->n_slices;
    }
}

static ALWAYS_INLINE u32 cache_get_sib(u64 addr, CacheInfo* c)
{
    return (u32)_read_bit_range(addr,
                                c->n_cl_bits + c->n_set_idx_bits,
                                c->n_cl_bits);
}

static ALWAYS_INLINE u32 cache_get_color(u64 addr, CacheInfo* c)
{
    if (c->unknown_sib <= 0)
        return 0;

    return (u32)_read_bit_range(addr,
                                c->n_cl_bits + c->n_set_idx_bits,
                                c->n_cl_bits + c->n_set_idx_bits - c->unknown_sib);
}

#ifdef __cplusplus
}
#endif

#endif // CACHE_INFO_H
