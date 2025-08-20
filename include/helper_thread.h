#pragma once

#include "common.h"
#include "asm.h"
#include <pthread.h>

struct _ev_build_conf;
struct _evset;
typedef struct _evset EvSet;

typedef enum {
    HELPER_STOP,
    READ_SINGLE,
    TIME_SINGLE,
    READ_ARRAY,
    TRAVERSE_CANDS
} helper_thread_action;

typedef struct {
    bool running;
    volatile bool waiting;
    volatile helper_thread_action action;
    void * volatile payload;
    u64 lat;
    pthread_t pid;
    i32 core_id;
} helper_thread_ctrl;

void *helper_thread_worker(void * _args);

static ALWAYS_INLINE void wait_helper_thread(helper_thread_ctrl *ctrl)
{
    while (!ctrl->waiting);
}

static ALWAYS_INLINE bool start_helper_thread(helper_thread_ctrl *ctrl)
{
    ctrl->waiting = false;
    compiler_barrier();
    if (pthread_create(&ctrl->pid, NULL, helper_thread_worker, ctrl)) {
        perror("Failed to start the helper thread!\n");
        return true;
    }
    wait_helper_thread(ctrl);
    ctrl->running = true;
    return false;
}

static ALWAYS_INLINE void stop_helper_thread(helper_thread_ctrl *ctrl)
{
    if (ctrl->running) {
        ctrl->action = HELPER_STOP;
        compiler_barrier();
        ctrl->waiting = false;
        pthread_join(ctrl->pid, NULL);
        ctrl->running = false;
    }
}

static ALWAYS_INLINE bool start_helper_thread_pinned(helper_thread_ctrl *ctrl, i32 core_id) {
    ctrl->waiting = false;
    ctrl->core_id = core_id;
    compiler_barrier();
    
    if (pthread_create(&ctrl->pid, NULL, helper_thread_worker, ctrl)) {
        perror("Failed to start the helper thread!\n");
        return true;
    }
    
    wait_helper_thread(ctrl);
    ctrl->running = true;
    return false;
}

static ALWAYS_INLINE void
helper_thread_read_single(u8 *target, helper_thread_ctrl *ctrl) {
    assert(ctrl->running);
    ctrl->action = READ_SINGLE;
    ctrl->payload = target;
    compiler_barrier();
    ctrl->waiting = false;
    wait_helper_thread(ctrl);
}

static ALWAYS_INLINE u64
helper_thread_time_single(u8 *target, helper_thread_ctrl *ctrl) {
    assert(ctrl->running);
    ctrl->action = TIME_SINGLE;
    ctrl->payload = target;
    compiler_barrier();
    ctrl->waiting = false;
    wait_helper_thread(ctrl);
    _mfence();
    _lfence();
    return ctrl->lat;
}

struct helper_thread_read_array {
    u8 ** volatile addrs;
    volatile u64 cnt, repeat, stride, block;
    volatile bool bwd;
};

struct _evtest_config;

struct helper_thread_traverse_cands {
    void (*volatile traverse)(u8 **cands, u64 cnt, struct _ev_build_conf *c);
    u8 ** volatile cands;
    u64 volatile cnt;
    struct _ev_build_conf * volatile tconfig;
};

void attach_helper_to_evsets(EvSet ***color_sets, u32 *color_counts,
                             u32 start_color, u32 num_colors,
                             helper_thread_ctrl *hctrl);

