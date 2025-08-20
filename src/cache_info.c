#include "../include/cache_info.h"
#include "../include/utils.h"
#include <cpuid.h>
#include <stdlib.h>

CacheInfo l1_info = {0}, // L1Data
          l2_info = {0},
          l3_info = {0};

u32 g_n_uncertain_l2_sets = 0;
u64 g_l3_cnt = 0;

void init_cache_info()
{
    init_l1_info(&l1_info);
    init_l2_info(&l2_info);
    init_l3_info(&l3_info);

    g_n_uncertain_l2_sets = cache_uncertainty(&l2_info);
    g_l3_cnt = (1ULL << (l3_info.n_set_idx_bits - l2_info.n_set_idx_bits)) *
               l3_info.n_slices;
}

i32 calc_unknown_sib(CacheInfo* c)
{
    return (c->n_cl_bits + c->n_set_idx_bits - PAGE_OFFSET_BITS);
}

void init_l1_info(CacheInfo* l1)
{
    u32 eax, ebx, ecx, edx;
    // L1d cache is indexed by __cpuid_count(4, 0, ...)
    __cpuid_count(4, 0, eax, ebx, ecx, edx);

    u32 line_size_minus_1 = (ebx & 0xFFF);
    u32 ways_minus_1 = (ebx >> 22) & 0x3FF;
    u32 sets_minus_1 = ecx;

    l1->level = L1;
    l1->cl_size = line_size_minus_1 + 1;
    l1->n_cl_bits = log2_ceil(l1->cl_size);
    l1->n_ways = ways_minus_1 + 1;
    l1->n_sets = sets_minus_1 + 1;
    l1->n_set_idx_bits = log2_ceil(l1->n_sets);
    l1->unknown_sib = calc_unknown_sib(l1);
    l1->n_slices = 1;
    if (l1->unknown_sib == -1) {
        fprintf(stderr, ERR "Could not determine L1d's set index bits.");
        exit(EXIT_FAILURE);
    }
    l1->size = l1->cl_size *
               l1->n_ways  *
               l1->n_sets;
}

void init_l2_info(CacheInfo* l2)
{
    u32 eax, ebx, ecx, edx;
    // L2 is indexed by __cpuid_count(4, 2, ...)
    __cpuid_count(4, 2, eax, ebx, ecx, edx); 

    u32 line_size_minus_1 = (ebx & 0xFFF);
    u32 ways_minus_1      = (ebx >> 22) & 0x3FF;
    u32 sets_minus_1      = ecx;

    l2->level = L2;
    l2->cl_size = line_size_minus_1 + 1;
    l2->n_cl_bits = log2_ceil(l2->cl_size);
    l2->n_ways = ways_minus_1 + 1;
    l2->n_sets = sets_minus_1 + 1;
    u32 l2_n_sets = l2->n_sets;
    l2->size = l2->cl_size *
               l2->n_ways  *
               l2_n_sets;

    l2->n_set_idx_bits = log2_ceil(l2_n_sets);
    l2->unknown_sib = calc_unknown_sib(l2);
    l2->n_slices = 1;
}

/*
  underlying _p_hysical CPU core count is commonly exposed inaccurately in VMs.
  e.g. if the VM is running on a 28-core Intel Skylake processor and the VM
  is given 12 vCPUs, the VM OS sees the p_core count as 12, eventhough the underlying
  pCPU contains 28 cores. Although, it's common that the pCPU cache info is
  made "see/pass-through" by cloud providers for performance benefits, hence information 
  such as the total cache set number of the L3/LLC would be accurate. On the other hand,
  in Intel Xeon Scalable (XS) processors, a common n_set per L3/LLC slice is 2048.
  So, `n_total_llc_set / n_set_per_slice = n_cores` is a generally accurate
  heuristic for the n_cores of the underlying pCPU to detect from the VM,
  considering the standard 1 L3/LLC slice per core on Intel XS processors.
  If you know this default value is not the case with your VM's underlying pCPU(s),
  changing the value set for `N_L3_SLICE` in common.h overrides the default value, 
  and adjusts n_set_per_slice accordingly.
  The program reports the auto-detected value at the start.
  If you feel that it's off, you could check the host's CPU type using `lscpu`,
  and a quick search would show the n_physical_cores the socket has.
  One could then match `N_L3_SLICE`'s value to it.
*/
void init_l3_info(CacheInfo* l3)
{
    u32 eax, ebx, ecx, edx;
    // L3 is indexed by __cpuid_count(4, 3, ...)
    __cpuid_count(4, 3, eax, ebx, ecx, edx); 

    u32 line_size_minus_1 = (ebx & 0xFFF);
    u32 ways_minus_1 = (ebx >> 22) & 0x3FF;
    u32 sets_minus_1 = ecx;

    l3->level = L3;
    l3->cl_size = line_size_minus_1 + 1;
    l3->n_cl_bits = log2_ceil(l3->cl_size);
    l3->n_ways = ways_minus_1 + 1;
    /*
      see comments at both init_l3_info and
      the definition of ONE_SLICE_SETS
    */
    l3->n_sets_one_slice = ONE_SLICE_SETS;
    l3->n_sets = sets_minus_1 + 1;
    u32 l3_n_sets = sets_minus_1 + 1; // used frequently, stack alloc
    l3->n_slices = l3->n_sets / l3->n_sets_one_slice;
    l3->n_set_idx_bits = log2_ceil(l3->n_sets_one_slice);
    l3->unknown_sib = calc_unknown_sib(l3);

    // if changed/set by user, adjust other values
    if (N_L3_SLICE) {
        l3->n_slices = N_L3_SLICE;
        l3->n_sets_one_slice = l3_n_sets / l3->n_slices;
        l3->n_set_idx_bits = log2_ceil(l3->n_sets_one_slice);
        l3->unknown_sib = calc_unknown_sib(l3);
        if (l3->unknown_sib == -1)
            exit(EXIT_FAILURE);
    }

    l3->size = l3->cl_size *
               l3->n_ways  *
               l3_n_sets; // total sets

    printf(NOTE "%s %u LLC slices for physical socket. %s\n",
           N_L3_SLICE ? "Statically set" : "Auto detected",
           l3->n_slices,
           N_L3_SLICE ? "" : "Double check this value.");
}


