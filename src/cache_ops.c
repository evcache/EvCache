#include "../include/common.h"
#include "../include/asm.h"
#include "../include/cache_ops.h"

void traverse_cands_mt(u8 **cands, u64 cnt, EvBuildConf* tconf) 
{
    struct helper_thread_read_array *arr = malloc(sizeof(*arr));
    assert(arr);
    u64 repeat = tconf->ev_repeat, block = tconf->block,
        stride = tconf->stride;

    *arr = (struct helper_thread_read_array)
    {
        .addrs = cands,
        .cnt = cnt,
        .repeat = repeat,
        .block = block,
        .stride = stride,
        .bwd = true
    };

    tconf->hctrl->action = READ_ARRAY;
    tconf->hctrl->payload = arr;
    compiler_barrier();
    tconf->hctrl->waiting = false;

    //access_array(cands, cnt);
    prime_cands_daniel(cands, cnt, repeat, stride, block);
    if (cnt < l2_info.n_ways && tconf->lower_ev) {
        addrs_traverse(tconf->lower_ev->addrs,
                       tconf->lower_ev->size,
                       tconf->lower_ev->build_conf);
        _lfence();
        access_array_bwd(cands, cnt);
    }

    wait_helper_thread(tconf->hctrl);
    free(arr);
}
