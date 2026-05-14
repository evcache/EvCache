#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE // cpu affinity
#include "colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <memory.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>

// typedef uint8_t u8;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define PAGE_SIZE          (1<<12) // 4KiB
#define PAGE_SHIFT         12      // for 4KiB regular pages
#define CORE_PIN_ID        3
#define MAX_ITERATIONS     1000
#define DEF_LAT_REPS       350     // default n_iteration for caches' latency measurement
#define PAGE_OFFSET_BITS   12      // for 4KiB regular pages

#define CL_SIZE            (1UL<<6)  // 64 bytes
#define NUM_OFFSETS        (PAGE_SIZE / CL_SIZE)
/*
  If auto detected values are different than your VM's underlying pCPU,
  you don't need to worry about ONE_SLICE_SETS's value.
  Only manually setting the value of N_L3_SLICE to the correct amount
  would adjust the rest of the variables accordingly.
*/
#define ONE_SLICE_SETS     2048
#define N_L3_SLICE         0

#endif // COMMON_H
