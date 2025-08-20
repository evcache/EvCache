#include "../include/common.h"
#include "../include/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

i64 init_base_cands(EvCands* cands, u64* region_size)
{
    u32 stride = PAGE_SIZE;
    *region_size = (cands->count + 1) * stride;

    u8* base = _calloc(1, *region_size);
    if (!base) {
        fprintf(stderr, ERR "regular/base page allocation failed.\n");
        return -1;
    }

    cands->addrs = _calloc(1, cands->count * sizeof(u8*));
    if (cands->addrs == NULL) {
        free(base);
        return -1;
    }

    for (i32 i = 0; i < cands->count; i++) {
        cands->addrs[i] = base + ((i + 1) * stride);
    }
    return (i64)base;
}

