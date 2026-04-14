#ifndef ENGINE_H
#define ENGINE_H

/*
 * engine.h — Docksmith Build Engine Public API
 *
 * Included by: cli/cli.c, cache/cache.c
 */

#include "../cli/parser.h"
#include "../runtime/runtime.h"

/* Resolves ~/.docksmith correctly under both sudo and normal user. */
void get_docksmith_dir(char *out);   /* out must be PATH_MAX bytes */

#define MAX_LAYERS 64

/*
 * LayerList — ordered list of all layers built during execute_build().
 * Layer 0 = first base image layer. Layer count-1 = most recent.
 */
typedef struct {
    char   paths[MAX_LAYERS][512];      /* absolute path to .tar file       */
    char   digests[MAX_LAYERS][65];     /* SHA-256 hex (64 chars + NUL)     */
    char   created_by[MAX_LAYERS][256]; /* human label: "COPY . /app"       */
    size_t sizes[MAX_LAYERS];           /* byte size of the tar on disk      */
    int    count;
} LayerList;

/*
 * BuildResult — populated by execute_build(), consumed by cli.c to write
 * the image manifest and by cache/cache.c for key computation.
 */
typedef struct {
    LayerList layers;

    char cmd[256];          /* JSON array string, e.g. ["/app/run"]         */
    char working_dir[256];  /* final WORKDIR value; "/" if not set           */
    rt_env_t env[32];       /* accumulated ENV key=value pairs               */
    int  env_count;

    /*
     * BUG6 FIX: base image manifest digest (NOT a layer digest).
     * The cache module needs this as the "previous layer digest" for the
     * first layer-producing instruction (spec §5.1).
     * Set by handle_from(); empty string if FROM was not executed.
     */
    char base_manifest_digest[65];

    /*
     * Set to 1 when every COPY/RUN step was a cache hit.
     * CLI uses this to preserve the original `created` timestamp so the
     * manifest digest is identical across rebuilds (spec §8).
     */
    int all_cache_hits;
} BuildResult;

/*
 * execute_build — main entry point called by cli.c after parsing.
 *
 * list        : parsed instructions from the Docksmithfile
 * tag         : "name:tag" string (informational; CLI writes the manifest)
 * context_dir : directory containing the Docksmithfile and build files
 * result      : populated on success
 * no_cache    : 1 = skip all cache lookups/writes (--no-cache flag)
 *
 * Returns 0 on success, -1 on any fatal error.
 */
int execute_build(InstructionList *list,
                  const char      *tag,
                  const char      *context_dir,
                  BuildResult     *result,
                  int              no_cache);

#endif /* ENGINE_H */
