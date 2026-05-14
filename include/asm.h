#ifndef ASM_H
#define ASM_H

#include "common.h"

#define ALWAYS_INLINE inline __attribute__((always_inline))

#define maccess(P)                            \
    do {                                      \
        typeof(*(P)) _NO_USE;                 \
        __asm__ __volatile__("mov (%1), %0\n" \
                             : "=r"(_NO_USE)  \
                             : "r"(P)         \
                             : "memory");     \
    } while (0)

#define _mwrite(P, V)                                                          \
    do {                                                                       \
        __asm__ __volatile__("mov %1, %0\n"                                    \
                             : "=m"(*(P))                                      \
                             : "r"((V))                                        \
                             : "memory");                                      \
    } while (0)

#define _force_addr_calc(P)                   \
    do {                                      \
        __asm__ __volatile__("mov %0, %0\n\t" \
                             : "+r"(P)        \
                             :                \
                             : "memory");     \
    } while (0)

static ALWAYS_INLINE void _nop(void)
{
    __asm__ __volatile__("nop" : : : "memory");
}

static ALWAYS_INLINE void _clflush(void *addr)
{
    __asm__ __volatile__("clflush (%0)"
                         : 
                         : "r"(addr) 
                         : "memory");
}

static ALWAYS_INLINE void _clflushopt(void *addr)
{
    __asm__ __volatile__("clflushopt (%0)"
                         : 
                         : "r"(addr) 
                         : "memory");
}

static ALWAYS_INLINE void _mfence()
{
    __asm__ __volatile__("mfence"
                         :
                         :
                         : "memory");
}

static ALWAYS_INLINE void _lfence(void)
{
    __asm__ __volatile__("lfence"
                         :
                         :
                         : "memory");
}

static ALWAYS_INLINE void compiler_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

static ALWAYS_INLINE u64 _rdtsc(void)
{
    u64 rax;
    __asm__ __volatile__(
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %0\n\t"
        :"=a"(rax)
        :: "rdx", "memory", "cc"
    );
    return rax;
}

/*
  check core ID returned by rdtscp 
  Core IDs (auxiliary/aux value) in ecx
  are checked before (written to aux1) and after (aux2)
  the operation and if they are not the same,
  the timing is discarded.
*/
static ALWAYS_INLINE u64 _rdtscp_aux(u32 *aux)
{
    u64 rax;
    __asm__ __volatile__("rdtscp\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "mov %%ecx, %1\n\t"
                         "lfence"
                         : "=a"(rax), "=r"(*aux)
                         :
                         : "rcx", "rdx", "memory", "cc");
    return rax;
}

// https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
static ALWAYS_INLINE u64 timer_start(void)
{
    u64 t;
    __asm__ __volatile__("mfence\n\t"
                         "lfence\n\t"
                         "rdtsc\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         : "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 timer_stop(void)
{
    u64 t;
    __asm__ __volatile__("rdtscp\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         : "rcx", "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 _timer_warmup(void)
{
    /*
    u64 lat = timer_start();
    lat = timer_stop() - lat;
    return lat;
    */
    return _rdtsc(); // actually, this seems to suffice
}

static ALWAYS_INLINE u64 count_ones(u64 val) {
    u64 ret;
    __asm__ __volatile__("popcnt %1, %0" : "=r"(ret) : "r"(val) : "cc");
    return ret;
}

#endif // ASM_H
