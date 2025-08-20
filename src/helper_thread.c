#include "../include/helper_thread.h"
#include "../include/utils.h"
#include "../include/cache_ops.h"
#include "../include/asm.h"
#include "../include/evset.h"

void *helper_thread_worker(void * _args) {
    helper_thread_ctrl *ctrl = _args;

    if (ctrl->core_id >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(ctrl->core_id, &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            perror("Failed to set helper thread affinity");
        }
    }

    while (true) {
        ctrl->waiting = true;
        while (ctrl->waiting);
        switch (ctrl->action) {
            case HELPER_STOP: {
                return NULL;
            }
            case READ_SINGLE: {
                maccess((u8 *)ctrl->payload);
                break;
            }
            case TIME_SINGLE: {
                u8 *ptr = (u8 *)ctrl->payload;
                ctrl->lat = _time_maccess(ptr);
                break;
            }
            case READ_ARRAY: {
                struct helper_thread_read_array *arr = ctrl->payload;

                prime_cands_daniel(arr->addrs, arr->cnt, arr->repeat,
                                   arr->stride, arr->block);
                break;
            }
            case TRAVERSE_CANDS: {
                struct helper_thread_traverse_cands *cmd = ctrl->payload;
                cmd->traverse(cmd->cands, cmd->cnt, cmd->tconfig);
                break;
            }
        }
    }
}

void attach_helper_to_evsets(EvSet ***color_sets, u32 *color_counts,
                             u32 start_color, u32 num_colors,
                             helper_thread_ctrl *hctrl) {
    for (u32 idx = 0; idx < num_colors; idx++) {
        u32 color = start_color + idx;
        u32 set_cnt = color_counts[color];
        for (u32 s = 0; s < set_cnt; s++) {
            EvSet *ev = color_sets[color][s];
            if (ev && ev->size > 0) {
                ev->build_conf->hctrl = hctrl;
                if (ev->build_conf->lower_ev && ev->build_conf->lower_ev->build_conf) {
                    ev->build_conf->lower_ev->build_conf->hctrl = hctrl;
                }
            }
        }
    }
}
