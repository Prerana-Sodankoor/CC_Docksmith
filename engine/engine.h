#ifndef ENGINE_H
#define ENGINE_H

#include "../cli/parser.h"
#include "../runtime/runtime.h"

void get_docksmith_dir(char *out);  

#define MAX_LAYERS 64

typedef struct {
    char   paths[MAX_LAYERS][512];   
    char   digests[MAX_LAYERS][65];    
    char   created_by[MAX_LAYERS][256];
    size_t sizes[MAX_LAYERS];           
    int    count;
} LayerList;

typedef struct {
    LayerList layers;

    char cmd[256];         
    char working_dir[256];  
    rt_env_t env[32];      
    int  env_count;

    char base_manifest_digest[65];

    int all_cache_hits;
} BuildResult;

int execute_build(InstructionList *list,
                  const char      *tag,
                  const char      *context_dir,
                  BuildResult     *result,
                  int              no_cache);

#endif
