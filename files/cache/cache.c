#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>

/* Advanced Telemetry features for the Demo! */
static int total_lookups = 0;
static int cache_hits = 0;

/* Compute SHA256 of all files in a source path sorted by path */
static int hash_context_files(EVP_MD_CTX *mdctx, const char *context_dir, const char *instruction_arg1) {
    char cmd[2048];
    char pattern[2048];
    
    if (instruction_arg1[0] == '/') {
        snprintf(pattern, sizeof(pattern), "%s", instruction_arg1);
    } else if (strcmp(instruction_arg1, ".") == 0 || strcmp(instruction_arg1, "./") == 0) {
        snprintf(pattern, sizeof(pattern), ".");
    } else {
        snprintf(pattern, sizeof(pattern), "%s", instruction_arg1);
    }

    /* 
     * Find all files, sort them lexicographically by path, then hash them.
     * By sorting the paths first and passing to sha256sum, we get deterministic output.
     */
    snprintf(cmd, sizeof(cmd), "cd '%s' && find '%s' -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 rsync -a --copy-links 2>/dev/null; find '%s' -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 sha256sum 2>/dev/null", context_dir, pattern, pattern);
    
    // Simplier reliable find command
    snprintf(cmd, sizeof(cmd), "cd '%s' && find '%s' -type f 2>/dev/null | LC_ALL=C sort | xargs sha256sum 2>/dev/null", context_dir, pattern);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        EVP_DigestUpdate(mdctx, buf, n);
    }
    
    pclose(fp);
    return 0;
}

char* cache_lookup(const char *docksmith_dir,
                   const char *prev_digest,
                   const char *instruction_text,
                   const char *workdir,
                   char env_strings[][512],
                   int env_count,
                   const char *context_dir,
                   const char *instruction_arg1,
                   char *generated_key_out) 
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) return NULL;
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

    int debug = (getenv("DOCKSMITH_DEBUG_CACHE") != NULL);

    if (debug) {
        printf("\n\033[1;33m[CACHE INSPECTOR]\033[0m Computing Hash for Step...\n");
        printf("  ├─ \033[1;36mPrev Digest:\033[0m %s\n", (prev_digest && prev_digest[0]) ? prev_digest : "(none)");
        printf("  ├─ \033[1;36mInstruction:\033[0m %s\n", instruction_text ? instruction_text : "(none)");
        printf("  ├─ \033[1;36mWORKDIR:\033[0m     %s\n", (workdir && workdir[0]) ? workdir : "(none)");
    }

    /* 1. Add Previous Digest */
    if (prev_digest && strlen(prev_digest) > 0) {
        EVP_DigestUpdate(mdctx, prev_digest, strlen(prev_digest));
        EVP_DigestUpdate(mdctx, "\n", 1);
    }

    /* 2. Add Instruction Text */
    if (instruction_text) {
        EVP_DigestUpdate(mdctx, instruction_text, strlen(instruction_text));
        EVP_DigestUpdate(mdctx, "\n", 1);
    }

    /* 3. Add WORKDIR */
    if (workdir) {
        EVP_DigestUpdate(mdctx, workdir, strlen(workdir));
    }
    EVP_DigestUpdate(mdctx, "\n", 1);

    /* 4. Add ENV array */
    /* Sort environment variables lexicographically! */
    if (env_count > 0) {
        char *sorted_envs[32];
        for (int i = 0; i < env_count; i++) sorted_envs[i] = env_strings[i];
        
        for (int i = 0; i < env_count - 1; i++) {
            for (int j = i + 1; j < env_count; j++) {
                if (strcmp(sorted_envs[i], sorted_envs[j]) > 0) {
                    char *tmp = sorted_envs[i];
                    sorted_envs[i] = sorted_envs[j];
                    sorted_envs[j] = tmp;
                }
            }
        }
        for (int i = 0; i < env_count; i++) {
            if (debug) printf("  ├─ \033[1;36mENV[%d]:\033[0m      %s\n", i, sorted_envs[i]);
            EVP_DigestUpdate(mdctx, sorted_envs[i], strlen(sorted_envs[i]));
            EVP_DigestUpdate(mdctx, "\n", 1);
        }
    } else if (debug) {
        printf("  ├─ \033[1;36mENV STATE:\033[0m   (none)\n");
    }

    /* 5. Add File Hashes (if COPY) */
    if (instruction_arg1 && context_dir) {
        if (debug) printf("  ├─ \033[1;36mFILE RAW HASH PIPELINE RUNNING...\033[0m\n");
        hash_context_files(mdctx, context_dir, instruction_arg1);
    }

    /* Finalize hash to create the Cache Key */
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hlen = 0;
    EVP_DigestFinal_ex(mdctx, hash, &hlen);
    EVP_MD_CTX_free(mdctx);

    for (unsigned int i = 0; i < hlen; i++) {
        sprintf(generated_key_out + (i * 2), "%02x", hash[i]);
    }
    generated_key_out[hlen * 2] = '\0';

    if (debug) {
        printf("  └─ \033[1;32mGenerated Deterministic Key:\033[0m %s\n", generated_key_out);
    }

    /* Now look up the key in cache backend */
    char cache_file_path[2048];
    snprintf(cache_file_path, sizeof(cache_file_path), "%s/cache/%s", docksmith_dir, generated_key_out);

    FILE *f = fopen(cache_file_path, "r");
    if (!f) {
        return NULL; // MISS
    }

    char layer_digest[256] = {0};
    if (fgets(layer_digest, sizeof(layer_digest), f) == NULL) {
        fclose(f);
        return NULL; // MISS / Error
    }
    fclose(f);

    layer_digest[strcspn(layer_digest, "\r\n")] = '\0';

    if (strlen(layer_digest) > 0) {
        char layer_path[2048];
        snprintf(layer_path, sizeof(layer_path), "%s/layers/%s.tar", docksmith_dir, layer_digest);
        if (access(layer_path, F_OK) == 0) {
            cache_hits++;
            return strdup(layer_digest); // HIT
        } else {
            unlink(cache_file_path); // Clear dangling cache
        }
    }

    return NULL;
}

void cache_store(const char *docksmith_dir, const char *cache_key, const char *layer_digest) {
    char cache_file_path[2048];
    snprintf(cache_file_path, sizeof(cache_file_path), "%s/cache/%s", docksmith_dir, cache_key);

    FILE *f = fopen(cache_file_path, "w");
    if (f) {
        fprintf(f, "%s\n", layer_digest);
        fclose(f);
    }
}

/* __attribute__((destructor)) runs at the very end of the process.
 * Perfect for a fancy Live Demo summary without breaking the required step formatting! */
__attribute__((destructor))
static void cache_print_stats() {
    if (total_lookups > 0) {
        float hit_ratio = ((float)cache_hits / (float)total_lookups) * 100.0f;
        printf("\n\033[1;36m========================================\033[0m\n");
        printf("\033[1;35m  DOCKSMITH CACHE TELEMETRY DASHBOARD   \033[0m\n");
        printf("\033[1;36m========================================\033[0m\n");
        printf("  \033[1;33mTotal Cache Lookups:\033[0m %d\n", total_lookups);
        printf("  \033[1;32mTotal Cache Hits:\033[0m    %d\n", cache_hits);
        printf("  \033[1;31mTotal Cache Misses:\033[0m  %d\n", total_lookups - cache_hits);
        printf("  \033[1;36mCache Hit Ratio:\033[0m     %.1f%%\n", hit_ratio);
        printf("\033[1;36m========================================\033[0m\n\n");
    }
}
