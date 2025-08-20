#ifndef GPA_HPA_H
#define GPA_HPA_H

#include "../include/common.h"

#define DEBUG_MOD_PATH "/proc/gpa_hpa"

i32 start_debug_mod(void);

void stop_debug_mod(void);

u64 va_to_hpa(void* va);

u32 l3_slice_skx_20(i64 addr);

#endif /* GPA_HPA_H */

