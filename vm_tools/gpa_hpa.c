#include "gpa_hpa.h"
#include "../include/utils.h"
#include "../include/common.h"
#include <errno.h>

extern i32 verbose;
i32 debug_mod_fd;

i32 start_debug_mod(void)
{
    debug_mod_fd = open(DEBUG_MOD_PATH, O_RDWR);
    if (debug_mod_fd < 0) {
        fprintf(stderr, ERR "Cannot open " DEBUG_MOD_PATH ". Is gpa_hpa loaded?\n");
        return -1;
    }
    if (verbose > 1)
        printf(V2 "set up debug module successfully\n");
    return 1;
}

void stop_debug_mod(void)
{
    close(debug_mod_fd);
    if (verbose > 1)
        printf(V2 "closed debug module's file descriptor\n");
}

// VM user space address to phys addr used to back it on the host
u64 va_to_hpa(void *va)
{
    u64 offset = (unsigned long) va & 0xFFF;
    char buf[256];
    u64 pfn = va_to_pa(va);

    // proc write
    snprintf(buf, sizeof(buf), "%lx", pfn);
    if (write(debug_mod_fd, buf, strlen(buf)) < 0) {
        if (errno == EBADF)
            fprintf(stderr, ERR "Cannot open /proc/gpa_hpa. Is gpa_hpa loaded?\n");
        else
            perror("failed write to proc");
        exit(EXIT_FAILURE);
    }

    // read res right after
    memset(buf, 0, sizeof(buf));
    i64 ret = read(debug_mod_fd, buf, sizeof(buf) - 1);
    if (ret < 0) {
        perror("failed read from proc");
        exit(EXIT_FAILURE);
    }

    // extract HPA from response (we don't use provided flags by the mod)
    u64 hpa;
    if (sscanf(buf, "HPA=0x%lx", &hpa) != 1) {
        fprintf(stderr, ERR "Failed to parse HPA from response\n");
        exit(EXIT_FAILURE);
    }

    //hpa = hpa << PAGE_SHIFT | offset;
    hpa = hpa | offset;
    return hpa;
}

// Dr. Bandwidth
// https://repositories.lib.utexas.edu/items/78ed399f-0e5e-41fe-96e1-c12a5acf74d7
// base seqs: https://repositories.lib.utexas.edu/server/api/core/bitstreams/96abbd10-744e-4c0e-9536-2f1a7378c27f/content
u32 l3_slice_skx_20(i64 addr) {
    static const u32 base_seq[256] = {
        0, 11, 2, 9, 7, 12, 5, 14, 1, 10, 3, 8, 6, 13, 4, 15,
        1, 10, 3, 8, 6, 13, 4, 15, 0, 11, 18, 17, 7, 12, 17, 18,
        8, 3, 10, 1, 15, 4, 13, 6, 9, 2, 19, 16, 14, 5, 16, 19,
        9, 2, 11, 0, 14, 5, 12, 7, 8, 3, 18, 17, 15, 4, 17, 18,
        10, 1, 8, 3, 13, 6, 15, 4, 11, 0, 9, 2, 12, 7, 14, 5,
        11, 0, 9, 2, 12, 7, 14, 5, 18, 17, 8, 3, 17, 18, 15, 4,
        2, 9, 0, 11, 5, 14, 7, 12, 19, 16, 1, 10, 16, 19, 6, 13,
        3, 8, 1, 10, 4, 15, 6, 13, 18, 17, 0, 11, 17, 18, 7, 12,
        4, 15, 6, 13, 3, 8, 1, 10, 5, 14, 7, 12, 2, 9, 0, 11,
        5, 14, 7, 12, 2, 9, 0, 11, 16, 19, 6, 13, 19, 16, 1, 10,
        12, 7, 14, 5, 11, 0, 9, 2, 17, 18, 15, 4, 18, 17, 8, 3,
        13, 6, 15, 4, 10, 1, 8, 3, 16, 19, 14, 5, 19, 16, 9, 2,
        14, 5, 12, 7, 9, 2, 11, 0, 15, 4, 13, 6, 8, 3, 10, 1,
        15, 4, 13, 6, 8, 3, 10, 1, 14, 5, 16, 19, 9, 2, 19, 16,
        6, 13, 4, 15, 1, 10, 3, 8, 7, 12, 17, 18, 0, 11, 18, 17,
        7, 12, 5, 14, 0, 11, 2, 9, 6, 13, 16, 19, 1, 10, 19, 16
    };

    // table 5: perm selector masks from the paper
    static const i64 perm_masks[8] = {
        0x3ecbad4000ULL,  // p0
        0x35cf7c000ULL,   // p1
        0x387242c000ULL,  // p2
        0xe2f28c000ULL,   // p3
        0x1c5e518000ULL,  // p4
        0x38bca30000ULL,  // p5
        0xfb2eb4000ULL,   // p6
        0x1f65d68000ULL   // p7
    };
    
    // b[13:6]
    u32 index = (addr >> 6) & 0xFF;
    
    u32 perm = 0;
    for (i16 i = 0; i < 8; i++) {
        // cnt set bits (pop count) after masking
        i64 masked = addr & perm_masks[i];
        
        u32 count = 0;
        i64 temp = masked;
        while (temp) {
            count += temp & 1;
            temp >>= 1;
        }
        
        // parity check (odd number of bits = 1) 
        if (count & 1) {
            perm |= (1 << i);
        }
    }
    
    u32 permuted_idx = index ^ perm;
    
    u32 slice = base_seq[permuted_idx];
    
    return slice;
}

