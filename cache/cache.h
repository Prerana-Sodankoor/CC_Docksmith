#ifndef CACHE_H
#define CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define CACHE_KEY_SIZE 65

/**
 * cache_lookup
 * 
 * Computes a deterministic cache key based on inputs, checks if a cache
 * entry exists for this key, and returns the cached layer digest if hit.
 * 
 * Returns dynamically allocated digest string if hit, or NULL if miss.
 * The generated cache_key is populated for use in cache_store on a miss.
 */
char* cache_lookup(const char *docksmith_dir,
                   const char *prev_digest,
                   const char *instruction_text,
                   const char *workdir,
                   char env_strings[][512],
                   int env_count,
                   const char *context_dir,
                   const char *instruction_arg1,
                   char *generated_key_out);

/**
 * cache_store
 * 
 * Saves the given layer_digest against the provided cache_key.
 */
void cache_store(const char *docksmith_dir, 
                 const char *cache_key, 
                 const char *layer_digest);

#endif // CACHE_H
