#include "engine.h"

#include "../cache/cache.h"
#include "../runtime/runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROOTFS_BASE "/tmp/docksmith-build-rootfs"
#define MAX_ENV 32
#define PATH_BUF 512
#define CMD_BUF 1024

void get_docksmith_dir(char *out)
{
    char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0]) {
        snprintf(out, PATH_BUF, "/home/%s/.docksmith", sudo_user);
    } else {
        char *home = getenv("HOME");
        if (!home) home = "/root";
        snprintf(out, PATH_BUF, "%s/.docksmith", home);
    }
}

static int compute_sha256_file(const char *filename, char *output)
{
    FILE *file = fopen(filename, "rb");
    if (!file) return -1;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        fclose(file);
        return -1;
    }

    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    unsigned char buf[4096];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), file)) > 0) {
        EVP_DigestUpdate(mdctx, buf, nread);
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hlen = 0;
    EVP_DigestFinal_ex(mdctx, hash, &hlen);

    EVP_MD_CTX_free(mdctx);
    fclose(file);

    for (unsigned int i = 0; i < hlen; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[hlen * 2] = '\0';
    return 0;
}

static void rmdir_recursive(const char *path)
{
    pid_t p = fork();
    if (p == 0) {
        execlp("rm", "rm", "-rf", path, (char *)NULL);
        _exit(1);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static double elapsed(struct timespec *t_start)
{
    struct timespec t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    return (t_now.tv_sec - t_start->tv_sec) +
           (t_now.tv_nsec - t_start->tv_nsec) / 1e9;
}

static int ensure_dir(const char *path)
{
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    return system(cmd);
}

static int extract_layer_to_rootfs(const char *layer_path, const char *rootfs)
{
    char cmd[CMD_BUF * 2];
    snprintf(cmd, sizeof(cmd),
             "tar -xf '%s' --no-same-owner -C '%s'",
             layer_path, rootfs);
    return system(cmd);
}

static int create_layer_tar(const char *anchor_dir,
                            const char *rel_path,
                            const char *docksmith_dir,
                            const char *label,
                            char *layer_path_out,
                            char *digest_out,
                            size_t *size_out)
{
    char temp_base[PATH_BUF];
    snprintf(temp_base, sizeof(temp_base), "/tmp/docksmith-layer-XXXXXX");
    int fd = mkstemp(temp_base);
    if (fd >= 0) {
        close(fd);
        unlink(temp_base);
    }

    char temp_tar[PATH_BUF];
    snprintf(temp_tar, sizeof(temp_tar), "%s.tar", temp_base);

    char tar_cmd[CMD_BUF * 2];
    snprintf(tar_cmd, sizeof(tar_cmd),
             "tar --sort=name --format=gnu --mtime='1970-01-01 00:00:00' "
             "--owner=0 --group=0 --numeric-owner "
             "-cf '%s' -C '%s' '%s'",
             temp_tar, anchor_dir, rel_path);

    if (system(tar_cmd) != 0) {
        fprintf(stderr, "[engine] tar failed for layer '%s'\n", label);
        return -1;
    }

    char digest[65];
    if (compute_sha256_file(temp_tar, digest) != 0) {
        unlink(temp_tar);
        return -1;
    }

    char final_path[PATH_BUF];
    snprintf(final_path, sizeof(final_path), "%s/layers/%s.tar", docksmith_dir, digest);

    if (access(final_path, F_OK) != 0) {
        if (rename(temp_tar, final_path) != 0) {
            char mv_cmd[CMD_BUF * 2];
            snprintf(mv_cmd, sizeof(mv_cmd),
                     "cp '%s' '%s' && rm -f '%s'",
                     temp_tar, final_path, temp_tar);
            if (system(mv_cmd) != 0) {
                unlink(temp_tar);
                return -1;
            }
        }
    } else {
        unlink(temp_tar);
    }

    struct stat st;
    *size_out = (stat(final_path, &st) == 0) ? (size_t)st.st_size : 0;

    strncpy(layer_path_out, final_path, PATH_BUF - 1);
    layer_path_out[PATH_BUF - 1] = '\0';
    strncpy(digest_out, digest, 64);
    digest_out[64] = '\0';

    return 0;
}

static int build_rt_env(char env_strings[][512], int env_count, rt_env_t *out)
{
    int n = 0;
    for (int i = 0; i < env_count && n < MAX_ENV; i++) {
        if (rt_parse_env_string(env_strings[i], &out[n]) == RT_OK) n++;
    }
    return n;
}

static int read_manifest_string_field(const char *json,
                                      const char *key,
                                      char *out,
                                      size_t out_sz)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\": \"", key);
    const char *p = strstr(json, search);
    if (!p) {
        snprintf(search, sizeof(search), "\"%s\":\"", key);
        p = strstr(json, search);
    }
    if (!p) return -1;

    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int handle_from(const char *image_ref,
                       const char *docksmith_dir,
                       const char *rootfs,
                       BuildResult *result)
{
    char safe_ref[128];
    strncpy(safe_ref, image_ref, sizeof(safe_ref) - 1);
    safe_ref[sizeof(safe_ref) - 1] = '\0';
    for (int i = 0; safe_ref[i]; i++) {
        if (safe_ref[i] == ':') safe_ref[i] = '_';
    }

    char manifest_path[PATH_BUF];
    snprintf(manifest_path, sizeof(manifest_path), "%s/images/%s.json", docksmith_dir, safe_ref);

    FILE *f = fopen(manifest_path, "r");
    if (!f) {
        fprintf(stderr, "[engine] ERROR: base image '%s' not found.\n", image_ref);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long msz = ftell(f);
    rewind(f);
    char *manifest_json = malloc((size_t)msz + 1);
    if (!manifest_json) {
        fclose(f);
        return -1;
    }
    fread(manifest_json, 1, (size_t)msz, f);
    manifest_json[msz] = '\0';
    fclose(f);

    read_manifest_string_field(manifest_json, "digest",
                               result->base_manifest_digest,
                               sizeof(result->base_manifest_digest));
    if (strncmp(result->base_manifest_digest, "sha256:", 7) == 0) {
        memmove(result->base_manifest_digest,
                result->base_manifest_digest + 7,
                strlen(result->base_manifest_digest + 7) + 1);
    }

    const char *layers_section = strstr(manifest_json, "\"layers\":");
    const char *arr_start = layers_section ? strchr(layers_section, '[') : NULL;
    if (!arr_start) {
        free(manifest_json);
        fprintf(stderr, "[engine] ERROR: manifest for '%s' has no layers.\n", image_ref);
        return -1;
    }

    int depth = 0;
    const char *arr_end = arr_start;
    while (*arr_end) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') {
            depth--;
            if (depth == 0) break;
        }
        arr_end++;
    }

    const char *p = arr_start;
    while (p < arr_end && result->layers.count < MAX_LAYERS) {
        const char *d = strstr(p, "\"digest\": \"sha256:");
        if (!d || d > arr_end) d = strstr(p, "\"digest\":\"sha256:");
        if (!d || d > arr_end) break;

        const char *start = strstr(d, "sha256:");
        if (!start || start > arr_end) break;
        start += 7;

        char digest[65] = {0};
        int j = 0;
        while (start[j] && start[j] != '"' && j < 64) {
            digest[j] = start[j];
            j++;
        }

        char layer_path[PATH_BUF];
        snprintf(layer_path, sizeof(layer_path), "%s/layers/%s.tar", docksmith_dir, digest);
        if (access(layer_path, R_OK) != 0 || extract_layer_to_rootfs(layer_path, rootfs) != 0) {
            free(manifest_json);
            fprintf(stderr, "[engine] ERROR: base layer missing: %s\n", layer_path);
            return -1;
        }

        int idx = result->layers.count;
        strncpy(result->layers.paths[idx], layer_path, PATH_BUF - 1);
        result->layers.paths[idx][PATH_BUF - 1] = '\0';
        strncpy(result->layers.digests[idx], digest, 64);
        result->layers.digests[idx][64] = '\0';
        snprintf(result->layers.created_by[idx], sizeof(result->layers.created_by[idx]),
                 "FROM %s layer %d", image_ref, idx + 1);

        struct stat st;
        result->layers.sizes[idx] = (stat(layer_path, &st) == 0) ? (size_t)st.st_size : 0;
        result->layers.count++;
        p = start + j;
    }

    free(manifest_json);
    return 0;
}

static int copy_sources_into(const char *src,
                             const char *context_dir,
                             const char *dest_a,
                             const char *dest_b)
{
    if (strcmp(src, ".") == 0 || strcmp(src, "./") == 0) {
        char cp_cmd[CMD_BUF * 2];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s'/. '%s/' 2>/dev/null", context_dir, dest_a);
        if (system(cp_cmd) != 0) return -1;
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s'/. '%s/' 2>/dev/null", context_dir, dest_b);
        return system(cp_cmd);
    }

    char pattern[PATH_BUF];
    if (src[0] == '/') {
        snprintf(pattern, sizeof(pattern), "%s", src);
    } else {
        snprintf(pattern, sizeof(pattern), "%s/%s", context_dir, src);
    }

    glob_t globbuf;
    int grc = glob(pattern, GLOB_TILDE | GLOB_MARK, NULL, &globbuf);
    if (grc == GLOB_NOMATCH) {
        fprintf(stderr, "[engine] COPY: no files matched '%s'\n", src);
        return -1;
    }

    if (grc != 0) {
        char cp_cmd[CMD_BUF * 2];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s'/. '%s/' 2>/dev/null", context_dir, dest_a);
        if (system(cp_cmd) != 0) return -1;
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s'/. '%s/' 2>/dev/null", context_dir, dest_b);
        return system(cp_cmd);
    }

    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        char cp_cmd[CMD_BUF * 2];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s' '%s/' 2>/dev/null", globbuf.gl_pathv[i], dest_a);
        if (system(cp_cmd) != 0) {
            globfree(&globbuf);
            return -1;
        }

        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r '%s' '%s/' 2>/dev/null", globbuf.gl_pathv[i], dest_b);
        if (system(cp_cmd) != 0) {
            globfree(&globbuf);
            return -1;
        }
    }

    globfree(&globbuf);
    return 0;
}

static int handle_copy(const char *src,
                       const char *dest,
                       const char *context_dir,
                       const char *rootfs,
                       const char *docksmith_dir,
                       LayerList *layers)
{
    char abs_dest[PATH_BUF];
    snprintf(abs_dest, sizeof(abs_dest), "%s%s", rootfs, dest);
    if (ensure_dir(abs_dest) != 0) return -1;

    char delta_dir[PATH_BUF];
    snprintf(delta_dir, sizeof(delta_dir), "/tmp/docksmith-copy-delta-XXXXXX");
    if (mkdtemp(delta_dir) == NULL) return -1;

    char delta_dest[PATH_BUF];
    snprintf(delta_dest, sizeof(delta_dest), "%s%s", delta_dir, dest);
    if (ensure_dir(delta_dest) != 0) {
        rmdir_recursive(delta_dir);
        return -1;
    }

    if (copy_sources_into(src, context_dir, abs_dest, delta_dest) != 0) {
        rmdir_recursive(delta_dir);
        return -1;
    }

    char layer_path[PATH_BUF];
    char digest[65];
    size_t size = 0;
    char label[256];
    snprintf(label, sizeof(label), "COPY %s %s", src, dest);

    if (create_layer_tar(delta_dir, ".", docksmith_dir, label, layer_path, digest, &size) != 0) {
        rmdir_recursive(delta_dir);
        return -1;
    }

    rmdir_recursive(delta_dir);

    if (layers->count < MAX_LAYERS) {
        int idx = layers->count;
        strncpy(layers->paths[idx], layer_path, PATH_BUF - 1);
        layers->paths[idx][PATH_BUF - 1] = '\0';
        strncpy(layers->digests[idx], digest, 64);
        layers->digests[idx][64] = '\0';
        strncpy(layers->created_by[idx], label, sizeof(layers->created_by[idx]) - 1);
        layers->created_by[idx][sizeof(layers->created_by[idx]) - 1] = '\0';
        layers->sizes[idx] = size;
        layers->count++;
    }

    return 0;
}

static int handle_run(const char *cmd_str,
                      const char *workdir,
                      char env_strings[][512],
                      int env_count,
                      const char **layer_paths,
                      size_t layer_count,
                      const char *docksmith_dir,
                      LayerList *layers)
{
    (void)layer_paths;
    (void)layer_count;

    char snapshot_base[PATH_BUF];
    snprintf(snapshot_base, sizeof(snapshot_base), "/tmp/docksmith-run-snapshot-XXXXXX");
    int fd = mkstemp(snapshot_base);
    if (fd < 0) return -1;
    close(fd);
    unlink(snapshot_base);

    char snapshot_tar[PATH_BUF];
    snprintf(snapshot_tar, sizeof(snapshot_tar), "%s.tar", snapshot_base);

    char snapshot_cmd[CMD_BUF * 2];
    snprintf(snapshot_cmd, sizeof(snapshot_cmd),
             "tar --sort=name --format=gnu --mtime='1970-01-01 00:00:00' "
             "--owner=0 --group=0 --numeric-owner -cf '%s' -C '%s' .",
             snapshot_tar, ROOTFS_BASE);
    if (system(snapshot_cmd) != 0) {
        unlink(snapshot_tar);
        return -1;
    }

    const char *snapshot_layers[] = { snapshot_tar };

    rt_env_t rt_env[MAX_ENV];
    int rt_env_count = build_rt_env(env_strings, env_count, rt_env);
    const char *cmd[] = { "/bin/sh", "-c", cmd_str, NULL };

    rt_config_t cfg = {
        .layers = snapshot_layers,
        .layer_count = 1,
        .working_dir = workdir,
        .image_env = rt_env,
        .image_env_count = (size_t)rt_env_count,
        .env_overrides = NULL,
        .env_override_count = 0,
        .cmd = cmd,
        .keep_rootfs = 1,
    };

    rt_result_t rt_result;
    rt_error_t rc = rt_run(&cfg, &rt_result);

    rt_free_env_array(rt_env, (size_t)rt_env_count);
    unlink(snapshot_tar);

    if (rc != RT_OK) {
        fprintf(stderr, "[engine] RUN failed: %s\n", rt_strerror(rc));
        return -1;
    }

    char delta_dir[PATH_BUF];
    snprintf(delta_dir, sizeof(delta_dir), "/tmp/docksmith-delta-XXXXXX");
    if (mkdtemp(delta_dir) == NULL) {
        rmdir_recursive(rt_result.rootfs_path);
        return -1;
    }

    char rsync_cmd[CMD_BUF * 2];
    snprintf(rsync_cmd, sizeof(rsync_cmd),
             "rsync -a --compare-dest='%s/' '%s/' '%s/' 2>/dev/null",
             ROOTFS_BASE, rt_result.rootfs_path, delta_dir);
    if (system(rsync_cmd) != 0) {
        rmdir_recursive(delta_dir);
        rmdir_recursive(rt_result.rootfs_path);
        return -1;
    }

    char sync_back_cmd[CMD_BUF * 2];
    snprintf(sync_back_cmd, sizeof(sync_back_cmd),
             "rsync -a '%s/' '%s/' 2>/dev/null",
             rt_result.rootfs_path, ROOTFS_BASE);
    if (system(sync_back_cmd) != 0) {
        rmdir_recursive(delta_dir);
        rmdir_recursive(rt_result.rootfs_path);
        return -1;
    }

    rmdir_recursive(rt_result.rootfs_path);

    char layer_path[PATH_BUF];
    char digest[65];
    size_t size = 0;
    char label[256];
    snprintf(label, sizeof(label), "RUN %s", cmd_str);

    int tar_rc = create_layer_tar(delta_dir, ".", docksmith_dir, label, layer_path, digest, &size);
    rmdir_recursive(delta_dir);
    if (tar_rc != 0) return -1;

    if (layers->count < MAX_LAYERS) {
        int idx = layers->count;
        strncpy(layers->paths[idx], layer_path, PATH_BUF - 1);
        layers->paths[idx][PATH_BUF - 1] = '\0';
        strncpy(layers->digests[idx], digest, 64);
        layers->digests[idx][64] = '\0';
        strncpy(layers->created_by[idx], label, sizeof(layers->created_by[idx]) - 1);
        layers->created_by[idx][sizeof(layers->created_by[idx]) - 1] = '\0';
        layers->sizes[idx] = size;
        layers->count++;
    }

    return 0;
}

int execute_build(InstructionList *list,
                  const char *tag,
                  const char *context_dir,
                  BuildResult *result,
                  int no_cache)
{
    (void)tag;

    char docksmith_dir[PATH_BUF];
    get_docksmith_dir(docksmith_dir);

    char layers_dir[PATH_MAX];
    char images_dir[PATH_MAX];
    char cache_dir[PATH_MAX];
    snprintf(layers_dir, sizeof(layers_dir), "%s/layers", docksmith_dir);
    snprintf(images_dir, sizeof(images_dir), "%s/images", docksmith_dir);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", docksmith_dir);

    ensure_dir(layers_dir);
    ensure_dir(images_dir);
    ensure_dir(cache_dir);

    memset(result, 0, sizeof(*result));
    strncpy(result->working_dir, "/", sizeof(result->working_dir) - 1);
    strncpy(result->cmd, "[]", sizeof(result->cmd) - 1);
    result->all_cache_hits = 1;

    char current_workdir[256] = "/";
    char env_strings[MAX_ENV][512] = {{0}};
    int env_count = 0;
    int total_steps = list->count;
    int cascade_miss = no_cache ? 1 : 0;

    for (int i = 0; i < list->count; i++) {
        Instruction inst = list->instructions[i];
        int step = i + 1;

        struct timespec t_step;
        clock_gettime(CLOCK_MONOTONIC, &t_step);

        if (strcmp(inst.type, "FROM") == 0) {
            printf("Step %d/%d : FROM %s\n", step, total_steps, inst.arg1);
            rmdir_recursive(ROOTFS_BASE);
            ensure_dir(ROOTFS_BASE);
            if (handle_from(inst.arg1, docksmith_dir, ROOTFS_BASE, result) != 0) return -1;
            continue;
        }

        if (strcmp(inst.type, "WORKDIR") == 0) {
            strncpy(current_workdir, inst.arg1, sizeof(current_workdir) - 1);
            current_workdir[sizeof(current_workdir) - 1] = '\0';

            char workdir_path[PATH_BUF];
            snprintf(workdir_path, sizeof(workdir_path), "%s%s", ROOTFS_BASE, current_workdir);
            ensure_dir(workdir_path);

            strncpy(result->working_dir, current_workdir, sizeof(result->working_dir) - 1);
            result->working_dir[sizeof(result->working_dir) - 1] = '\0';
            printf("Step %d/%d : WORKDIR %s\n", step, total_steps, inst.arg1);
            continue;
        }

        if (strcmp(inst.type, "ENV") == 0) {
            if (env_count < MAX_ENV) {
                if (inst.arg2[0] != '\0') {
                    snprintf(env_strings[env_count], sizeof(env_strings[env_count]),
                             "%s=%s", inst.arg1, inst.arg2);
                } else {
                    snprintf(env_strings[env_count], sizeof(env_strings[env_count]),
                             "%s", inst.arg1);
                }
                if (rt_parse_env_string(env_strings[env_count], &result->env[result->env_count]) == RT_OK) {
                    result->env_count++;
                    env_count++;
                }
            }
            printf("Step %d/%d : ENV %s\n", step, total_steps, env_strings[env_count - 1]);
            continue;
        }

        if (strcmp(inst.type, "CMD") == 0) {
            strncpy(result->cmd, inst.arg1, sizeof(result->cmd) - 1);
            result->cmd[sizeof(result->cmd) - 1] = '\0';
            printf("Step %d/%d : CMD %s\n", step, total_steps, inst.arg1);
            continue;
        }

        if (strcmp(inst.type, "COPY") == 0 || strcmp(inst.type, "RUN") == 0) {
            char full_cmd[512] = "";
            char instruction_text[512];
            const char *cache_src_context = NULL;
            const char *cache_src_pattern = NULL;

            if (strcmp(inst.type, "COPY") == 0) {
                snprintf(instruction_text, sizeof(instruction_text), "COPY %s %s", inst.arg1, inst.arg2);
                cache_src_context = context_dir;
                cache_src_pattern = inst.arg1;
            } else {
                if (inst.arg2[0] != '\0') {
                    snprintf(full_cmd, sizeof(full_cmd), "%s %s", inst.arg1, inst.arg2);
                } else {
                    snprintf(full_cmd, sizeof(full_cmd), "%s", inst.arg1);
                }
                snprintf(instruction_text, sizeof(instruction_text), "RUN %s", full_cmd);
            }

            char cache_key[CACHE_KEY_SIZE];
            char *hit_digest = NULL;
            if (!no_cache) {
                const char *prev_digest = result->layers.count > 0
                    ? result->layers.digests[result->layers.count - 1]
                    : result->base_manifest_digest;

                hit_digest = cache_lookup(docksmith_dir, prev_digest, instruction_text,
                                          current_workdir, env_strings, env_count,
                                          cache_src_context, cache_src_pattern,
                                          cache_key);
                
                if (cascade_miss && hit_digest) {
                    free(hit_digest);
                    hit_digest = NULL;
                }
            }

            if (hit_digest) {
                int idx = result->layers.count;
                snprintf(result->layers.paths[idx], PATH_BUF, "%s/layers/%s.tar", docksmith_dir, hit_digest);
                strncpy(result->layers.digests[idx], hit_digest, 64);
                result->layers.digests[idx][64] = '\0';
                strncpy(result->layers.created_by[idx], instruction_text,
                        sizeof(result->layers.created_by[idx]) - 1);
                result->layers.created_by[idx][sizeof(result->layers.created_by[idx]) - 1] = '\0';

                struct stat st;
                result->layers.sizes[idx] = (stat(result->layers.paths[idx], &st) == 0) ? (size_t)st.st_size : 0;
                result->layers.count++;

                if (extract_layer_to_rootfs(result->layers.paths[idx], ROOTFS_BASE) != 0) {
                    free(hit_digest);
                    return -1;
                }

                if (strcmp(inst.type, "COPY") == 0) {
                    printf("Step %d/%d : COPY %s %s [CACHE HIT] %.2fs\n", step, total_steps, inst.arg1, inst.arg2, elapsed(&t_step));
                } else {
                    printf("Step %d/%d : RUN %s [CACHE HIT] %.2fs\n",
                           step, total_steps, full_cmd, elapsed(&t_step));
                }

                free(hit_digest);
                continue;
            }

            cascade_miss = 1;
            result->all_cache_hits = 0;

            if (strcmp(inst.type, "COPY") == 0) {
                if (handle_copy(inst.arg1, inst.arg2, context_dir, ROOTFS_BASE, docksmith_dir, &result->layers) != 0) {
                    return -1;
                }
                printf("Step %d/%d : COPY %s %s [CACHE MISS] %.2fs\n",
                       step, total_steps, inst.arg1, inst.arg2, elapsed(&t_step));
            } else {
                const char *ordered_layer_paths[MAX_LAYERS];
                for (int j = 0; j < result->layers.count; j++) {
                    ordered_layer_paths[j] = result->layers.paths[j];
                }

                if (handle_run(full_cmd, current_workdir, env_strings, env_count,
                               ordered_layer_paths, (size_t)result->layers.count,
                               docksmith_dir, &result->layers) != 0) {
                    return -1;
                }
                printf("Step %d/%d : RUN %s [CACHE MISS] %.2fs\n",
                       step, total_steps, full_cmd, elapsed(&t_step));
            }

            if (!no_cache) {
                cache_store(docksmith_dir, cache_key, result->layers.digests[result->layers.count - 1]);
            }
            continue;
        }

        fprintf(stderr,
                "[engine] ERROR: unrecognised instruction '%s' at step %d\n"
                "         Supported: FROM, COPY, RUN, WORKDIR, ENV, CMD\n",
                inst.type, step);
        return -1;
    }

    return 0;
}
