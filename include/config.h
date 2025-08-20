#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// cross-checks evsets for duplication, and finds ones with
// mismatched L3 set index bits or slice numbers
#ifndef SANITY_CHECK_ALL_EVS
#define SANITY_CHECK_ALL_EVS 0
#endif

// enable custom eviction percentage experiment (-G 3)
#ifndef XP_EV_PCT
#define XP_EV_PCT 0
#endif

extern i32 verbose;
extern i32 debug;
extern i32 cand_count;
extern bool vtop;
extern bool granular;
extern bool remap;
extern bool graph_mode;
extern char *data_append;

typedef enum GraphType {
    GRAPH_NONE = -1,
    GRAPH_EVICTION_FREQ = 0,
    GRAPH_EVRATE_WAIT = 1,
    GRAPH_OCC_HEATMAP_L2COLOR = 2,
    GRAPH_EVRATE_TIME = 3,
    GRAPH_L2COLOR_DIST = 4
} GraphType;

extern GraphType graph_type;
extern bool vset;

typedef enum CacheLevel {
    L1 = 1,
    L2,
    L3,
    LLC = 3  // <L3|LLC>
} CacheLevel;

typedef enum EvRes {
    OK = 0,
    FAIL
} EvRes;

typedef struct {
    CacheLevel cache_level;
    u32 num_threads;
    u32 verbose_level;         // [1:3]
    u32 debug_level;           // [1:3]
    i32 num_sets;              // number of sets to construct evset for
    u32 num_offsets;           // -o N
    u32 num_l2_sets;           // -u N (granular): number of L2 uncertain sets
    u32 evsets_per_l2;         // -f N: eviction sets per L2 set
    bool granular;             // per-set evset construction and not per-offset
    u32 cand_scaling;          // cand scaling factor
    i32 vtop_freq;         // period vTop checking interval in microsecs
} ArgConf;

extern ArgConf g_config;

void init_def_args_conf(void);

void print_arg_conf(void);

#endif // CONFIG_H
