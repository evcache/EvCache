/*
 * Copyright:
 * https://github.com/zzrcxb/LLCFeasible/blob/master/include/bitwise.h
*/
#pragma once

#include "asm.h"

#define _SET_BIT(data, bit) ((data) | (1ull << (bit)))
#define _CLEAR_BIT(data, bit) ((data) & ~(1ull << (bit)))
#define _TOGGLE_BIT(data, bit) ((data) ^ (1ull << (bit)))
#define _WRITE_BIT(data, bit, val)                                             \
    (((data) & (~(1ull << (bit)))) | ((!!(val)) << (bit)))
#define _TEST_BIT(data, bit) (!!((data) & (1ull << (bit))))
#define _SEL_NOSPEC(MASK, T, F)                                                \
    (((MASK) & (typeof((MASK)))(T)) | (~(MASK) & (typeof((MASK)))(F)))

#define _SHIFT_MASK(shift) ((1ull << shift) - 1)
#define _ALIGNED(data, shift) (!((u64)(data) & _SHIFT_MASK(shift)))
#define __ALIGN_UP(data, shift) ((((u64)(data) >> (shift)) + 1) << (shift))
#define _ALIGN_UP(data, shift)                                                 \
    ((typeof(data))(_ALIGNED(data, shift) ? (u64)(data)                        \
                                          : __ALIGN_UP(data, shift)))
#define _ALIGN_DOWN(data, shift)                                               \
    ((typeof(data))((u64)(data) & (~_SHIFT_MASK(shift))))

/**                        |-end      |- start
* Set data 0000000000000000111111111110000
  Count from right to left, starting from 0, range EXCLUDES end
  i.e., [start, end). Then, it assigns data[start:end] = new_val[0:end-start]
*/
static ALWAYS_INLINE u64 _write_bit_range(u64 data, u16 end, 
                                          u16 start, u64 new_val)
{
    u16 width = end - start;
    u64 mask = (1ull << width) - 1;
    if (end <= start)
        return data; // invalid range

    new_val = (new_val & mask) << start;
    mask = ~(mask << start);
    data = (data & mask) | new_val;

    return data;
}

// end is excluded, similar to _write_bit_range
static ALWAYS_INLINE u64 _read_bit_range(u64 data, u16 end, u16 start)
{
    u16 width = end - start;
    u64 mask = (1ull << width) - 1;

    if (end <= start)
        return 0;
    else
        return (data >> start) & mask;
}
