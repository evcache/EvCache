// Memory related, e.g. adress handling, allocation, etc
#ifndef MEM_H
#define MEM_H

#include "asm.h"
#include "common.h"
#include "cache_info.h"
#include "evset.h"
#include "evset_para.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u8 *start_addr;
    size_t size;
    u8 init_value;
    u32 thread_id;
} memset_thread_arg_t;

static inline void *_calloc(u64 n_elem, u64 elem_size)
{
    void *p = malloc(n_elem * elem_size);

    if (p)
        memset(p, 0, n_elem * elem_size);

    return p;
}

static ALWAYS_INLINE u8 *mmap_shared(void *addr, u64 size)
{
    u8 *ptr = (u8*)mmap(addr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

static ALWAYS_INLINE u8 *mmap_shared_init(void *addr, size_t size, u8 init)
{
    u8 *ptr = mmap_shared(addr, size);

    if (ptr)
        memset(ptr, init, size);

    return ptr;
}

static void *para_memset_worker(void *arg)
{
    memset_thread_arg_t *ctx = (memset_thread_arg_t *)arg;
    memset(ctx->start_addr, ctx->init_value, ctx->size);
    return NULL;
}

static ALWAYS_INLINE u8 *mmap_shared_init_para(void *addr, size_t size, u8 init)
{
    u8 *ptr = mmap_shared(addr, size);
    if (!ptr)
        return NULL;

    u32 n_threads = g_config.num_threads ? g_config.num_threads : n_system_cores();

    if (n_threads < 2) {
        memset(ptr, init, size);
        return ptr;
    }
    
    size_t base_chunk_size = size / n_threads;
    size_t remainder = size % n_threads;
    
    pthread_t threads[n_threads];
    memset_thread_arg_t args[n_threads];
    
    u8 *current_addr = ptr;
    
    for (u32 i = 0; i < n_threads; i++) {
        // first remainder threads get an extra byte
        size_t this_chunk_size = base_chunk_size + (i < remainder ? 1 : 0);
        
        args[i].start_addr = current_addr;
        args[i].size = this_chunk_size;
        args[i].init_value = init;
        args[i].thread_id = i;
        
        if (pthread_create(&threads[i], NULL, para_memset_worker, &args[i])) {
            fprintf(stderr, ERR "Failed to create memset thread %u\n", i);
            size_t remaining = size - (current_addr - ptr); // fallback
            memset(current_addr, init, remaining);
            // already created threads
            for (u32 j = 0; j < i; j++)
                pthread_join(threads[j], NULL);

            return ptr;
        }
        
        current_addr += this_chunk_size;
    }
    
    for (u32 i = 0; i < n_threads; i++)
        pthread_join(threads[i], NULL);
    
    return ptr;
}

i64 init_base_cands(EvCands* cands, u64* region_size);

i32 ALWAYS_INLINE same_set_stride(CacheInfo* c)
{
    return 1 << (c->n_cl_bits + c->n_set_idx_bits);
}

i32 ALWAYS_INLINE next_set_stride(CacheInfo* c)
{
    return 1 << c->n_cl_bits;
}

#ifdef __cplusplus
}
#endif
#endif // MEM_H
