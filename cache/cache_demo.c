#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "cache.h"

int main() {
    char docksmith_dir[1024] = "/tmp/docksmith_interactive";
    
    // Setup mock directories for testing
    mkdir("/tmp", 0777);
    mkdir("/tmp/docksmith_interactive", 0777);
    mkdir("/tmp/docksmith_interactive/cache", 0777);
    mkdir("/tmp/docksmith_interactive/layers", 0777);

    char context_dir[1024];
    getcwd(context_dir, sizeof(context_dir));

    char current_digest[256] = "base_layer_digest_123456789";
    char current_workdir[256] = "/app";
    char env_strings[1][512] = {"MODE=interactive"};
    int env_count = 1;

    printf("\n============================================\n");
    printf("   INTERACTIVE DOCKSMITH CACHE REPL \n");
    printf("============================================\n");
    printf("Type a Dockerfile instruction to test the cache engine.\n");
    printf("Supported: RUN <command>, COPY <src> <dest>\n");
    printf("Example: RUN echo hello or COPY . /app\n");
    printf("Type 'exit' to quit and see the awesome telemetry!\n\n");

    char input[1024];

    while (1) {
        printf("\033[1;32mdocksmith-cache>\033[0m ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) continue;
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) break;

        char inst_type[32] = {0};
        char arg1[256] = {0};
        char arg2[256] = {0};

        sscanf(input, "%s %s %s", inst_type, arg1, arg2);

        if (strcmp(inst_type, "RUN") != 0 && strcmp(inst_type, "COPY") != 0) {
            printf("Error: Only RUN and COPY instructions are supported for this interactive cache demo.\n");
            continue;
        }

        char cache_key[CACHE_KEY_SIZE];
        char *hit_digest = NULL;

        if (strcmp(inst_type, "RUN") == 0) {
            hit_digest = cache_lookup(docksmith_dir, current_digest, input, current_workdir, env_strings, env_count, NULL, NULL, cache_key);
        } else if (strcmp(inst_type, "COPY") == 0) {
            hit_digest = cache_lookup(docksmith_dir, current_digest, input, current_workdir, env_strings, env_count, context_dir, arg1, cache_key);
        }

        if (hit_digest) {
            printf("\033[1;36m>> [CACHE HIT]\033[0m Layer retrieved successfully! (Digest: %.15s...)\n\n", hit_digest);
            strcpy(current_digest, hit_digest);
            free(hit_digest);
        } else {
            printf("\033[1;31m>> [CACHE MISS]\033[0m Executing new layer...\n");
            // Simulate generating a new abstract layer
            char new_layer_digest[65];
            snprintf(new_layer_digest, sizeof(new_layer_digest), "layer_%s_%ld", inst_type, random());
            
            cache_store(docksmith_dir, cache_key, new_layer_digest);
            strcpy(current_digest, new_layer_digest);
            printf("\033[1;33m>> [CACHE STORE]\033[0m Saved new layer permanently to /tmp/docksmith_interactive/cache/\n\n");
        }
    }

    // cache_print_stats() will be automatically called at exit if defined with __attribute__((destructor))
    return 0;
}
