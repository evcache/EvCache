#include "../include/config.h"
#include <stdlib.h>

ArgConf g_config;
i32 verbose;
i32 debug;
bool vtop;
bool granular;
bool remap;
bool graph_mode; // for vset
GraphType graph_type;
bool vset = false;
char *data_append = NULL;

void init_def_args_conf(void)
{
    g_config.num_threads = 0; // as many as possible
    g_config.verbose_level = 0;
    g_config.debug_level = 0;
    g_config.num_sets = 0;          // will replace to def later based on operations
    g_config.num_offsets = 1;      // def: 1 offset
    g_config.num_l2_sets = 0;      // default: all uncertain sets if granular
    g_config.evsets_per_l2 = 1;    // default evsets per L2 set
    g_config.cand_scaling = 0; // set to 0 to later tell whether user has changed it or not
    g_config.vtop_freq = 2000000; // default periodic vTop check in microseconds
    graph_type = GRAPH_NONE;
    if (data_append) {
        free(data_append);
        data_append = NULL;
    }
}
