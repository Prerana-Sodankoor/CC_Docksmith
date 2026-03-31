/*
 * cli.c — Docksmith Command Line Interface
 *
 * Commands:
 *   docksmith build -t <name:tag> <context> [--no-cache]
 *   docksmith images
 *   docksmith rmi   <name:tag>
 *   docksmith run   [-e KEY=VALUE ...] <name:tag> [cmd]
 *
 * Bugs fixed:
 *   BUG1  - handle_images() called load_manifest_json(NULL,NULL) → segfault.
 *   BUG2b - handle_run() layer extraction picked up the top-level manifest
 *           digest as a layer. Fixed by scoping the search to the "layers"
 *           JSON array only, same fix as engine.c handle_from().
 *   BUG3  - handle_rmi() while-loop OR condition caused infinite loop.
 *   BUG5  - --no-cache now passed to execute_build().
 *   BUG6  - created timestamp preserved on cache-hit rebuilds.
 *   MISS2 - Step timing added.
 *   MISS3 - Successfully built line includes digest prefix and total time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <openssl/evp.h>

#include "cli.h"
#include "parser.h"
#include "../engine/engine.h"
#include "../runtime/runtime.h"

#define PATH_BUF       4096
#define MAX_LAYERS_CLI 64

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void cli_get_docksmith_dir(char *out)
{
    char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0])
        snprintf(out, PATH_BUF, "/home/%s/.docksmith", sudo_user);
    else {
        char *home = getenv("HOME");
        if (!home) home = "/root";
        snprintf(out, PATH_BUF, "%s/.docksmith", home);
    }
}

static void sha256_of_string(const char *input, char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, input, strlen(input));
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hlen = 0;
    EVP_DigestFinal_ex(ctx, hash, &hlen);
    EVP_MD_CTX_free(ctx);
    for (unsigned int i = 0; i < hlen; i++)
        sprintf(out + (i * 2), "%02x", hash[i]);
    out[hlen * 2] = '\0';
}

static void iso8601_now(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", tm);
}

static void safe_name(const char *tag, char *out)
{
    strncpy(out, tag, PATH_BUF - 1);
    out[PATH_BUF - 1] = '\0';
    for (int i = 0; out[i]; i++)
        if (out[i] == ':') out[i] = '_';
}

/* ══════════════════════════════════════════════════════════════════════════
 * Manifest helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static int read_manifest_field(const char *json, const char *key,
                                char *out, size_t out_sz)
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

static int read_manifest_array_field(const char *json, const char *key,
                                     char *out, size_t out_sz)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;

    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return -1;

    int depth = 0;
    size_t i = 0;
    while (*p && i < out_sz - 1) {
        out[i++] = *p;
        if (*p == '[') depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0) break;
        }
        p++;
    }

    out[i] = '\0';
    return (depth == 0) ? 0 : -1;
}

static int parse_cmd_json_array(const char *cmd_json,
                                const char **cmd_argv,
                                int max_args)
{
    static char cmd_tokens[64][256];
    int cmd_argc = 0;
    const char *cp = cmd_json;

    while (*cp && *cp != '[') cp++;
    if (*cp != '[') return 0;
    cp++;

    while (*cp && *cp != ']' && cmd_argc < max_args - 1) {
        while (*cp == ' ' || *cp == '\t' || *cp == ',') cp++;
        if (*cp == '"') {
            cp++;
            int j = 0;
            while (*cp && *cp != '"' && j < 255) {
                if (*cp == '\\' && cp[1] != '\0') cp++;
                cmd_tokens[cmd_argc][j++] = *cp++;
            }
            cmd_tokens[cmd_argc][j] = '\0';
            if (*cp == '"') cp++;
            cmd_argv[cmd_argc] = cmd_tokens[cmd_argc];
            cmd_argc++;
        } else if (*cp) {
            cp++;
        }
    }

    cmd_argv[cmd_argc] = NULL;
    return cmd_argc;
}

static char *load_manifest_json(const char *tag, const char *docksmith_dir)
{
    char safe[PATH_BUF];
    safe_name(tag, safe);
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/images/%s.json", docksmith_dir, safe);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════════
 * write_manifest
 *
 * existing_created: if non-NULL/non-empty, use it (cache-hit rebuild must
 * preserve the original timestamp for manifest digest stability, spec §8).
 * ══════════════════════════════════════════════════════════════════════════ */

static int write_manifest(const char *image_ref,
                           const char *name, const char *tag,
                           const BuildResult *result,
                           const char *docksmith_dir,
                           const char *existing_created)
{
    char timestamp[64];
    if (existing_created && existing_created[0]) {
        strncpy(timestamp, existing_created, sizeof(timestamp) - 1);
        timestamp[sizeof(timestamp) - 1] = '\0';
    } else {
        iso8601_now(timestamp, sizeof(timestamp));
    }

    /* Build Env JSON array */
    char env_json[2048] = "[";
    for (int i = 0; i < result->env_count; i++) {
        char entry[512];
        snprintf(entry, sizeof(entry), "\"%s=%s\"%s",
                 result->env[i].key, result->env[i].value,
                 (i < result->env_count - 1) ? "," : "");
        strncat(env_json, entry, sizeof(env_json) - strlen(env_json) - 1);
    }
    strncat(env_json, "]", sizeof(env_json) - strlen(env_json) - 1);

    /* Build layers JSON array */
    char layers_json[8192] = "[\n";
    for (int i = 0; i < result->layers.count; i++) {
        char entry[1024];
        snprintf(entry, sizeof(entry),
                 "    { \"digest\": \"sha256:%s\", \"size\": %zu,"
                 " \"createdBy\": \"%s\" }%s\n",
                 result->layers.digests[i],
                 result->layers.sizes[i],
                 result->layers.created_by[i],
                 (i < result->layers.count - 1) ? "," : "");
        strncat(layers_json, entry,
                sizeof(layers_json) - strlen(layers_json) - 1);
    }
    strncat(layers_json, "  ]", sizeof(layers_json) - strlen(layers_json) - 1);

    /* Canonical form with digest="" for computing the manifest digest */
    char canonical[16384];
    snprintf(canonical, sizeof(canonical),
             "{\n"
             "  \"name\": \"%s\",\n"
             "  \"tag\": \"%s\",\n"
             "  \"digest\": \"\",\n"
             "  \"created\": \"%s\",\n"
             "  \"config\": {\n"
             "    \"Env\": %s,\n"
             "    \"Cmd\": %s,\n"
             "    \"WorkingDir\": \"%s\"\n"
             "  },\n"
             "  \"layers\": %s\n"
             "}",
             name, tag, timestamp,
             env_json, result->cmd, result->working_dir, layers_json);

    char manifest_digest[65];
    sha256_of_string(canonical, manifest_digest);

    /* Final manifest */
    char final_json[16384];
    snprintf(final_json, sizeof(final_json),
             "{\n"
             "  \"name\": \"%s\",\n"
             "  \"tag\": \"%s\",\n"
             "  \"digest\": \"sha256:%s\",\n"
             "  \"created\": \"%s\",\n"
             "  \"config\": {\n"
             "    \"Env\": %s,\n"
             "    \"Cmd\": %s,\n"
             "    \"WorkingDir\": \"%s\"\n"
             "  },\n"
             "  \"layers\": %s\n"
             "}",
             name, tag, manifest_digest, timestamp,
             env_json, result->cmd, result->working_dir, layers_json);

    char safe[PATH_BUF], manifest_path[PATH_BUF];
    safe_name(image_ref, safe);
    snprintf(manifest_path, sizeof(manifest_path),
             "%s/images/%s.json", docksmith_dir, safe);

    FILE *f = fopen(manifest_path, "w");
    if (!f) {
        fprintf(stderr, "[cli] cannot write manifest %s: %s\n",
                manifest_path, strerror(errno));
        return -1;
    }
    fputs(final_json, f);
    fclose(f);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * docksmith build
 * ══════════════════════════════════════════════════════════════════════════ */

void handle_build(int argc, char *argv[])
{
    char *tag = NULL;
    char *context = NULL;
    int no_cache = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            no_cache = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 < argc) {
                tag = argv[i + 1];
                i++;
            }
        } else if (!context) {
            context = argv[i];
        } else {
            fprintf(stderr, "Unknown build argument: %s\n", argv[i]);
            exit(1);
        }
    }

    if (!tag || !context) {
        fprintf(stderr,
                "Usage: docksmith build -t <name:tag> [--no-cache] <context>\n");
        exit(1);
    }

    char docksmithfile_path[PATH_BUF];
    snprintf(docksmithfile_path, sizeof(docksmithfile_path),
             "%s/Docksmithfile", context);

    if (access(docksmithfile_path, R_OK) != 0) {
        fprintf(stderr, "Error: cannot read %s: %s\n",
                docksmithfile_path, strerror(errno));
        exit(1);
    }

    InstructionList list = {0};
    parse_docksmithfile(docksmithfile_path, &list);

    if (list.count == 0) {
        fprintf(stderr, "Error: Docksmithfile is empty or unparseable\n");
        exit(1);
    }

    char docksmith_dir[PATH_BUF];
    cli_get_docksmith_dir(docksmith_dir);

    /* Load existing created timestamp for cache-hit rebuild preservation */
    char existing_created[64] = "";
    char *existing_json = load_manifest_json(tag, docksmith_dir);
    if (existing_json) {
        read_manifest_field(existing_json, "created",
                            existing_created, sizeof(existing_created));
        free(existing_json);
    }

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    BuildResult result;
    memset(&result, 0, sizeof(result));

    /* BUG5 FIX: pass no_cache */
    int rc = execute_build(&list, tag, context, &result, no_cache);
    if (rc != 0) {
        fprintf(stderr, "Build failed.\n");
        exit(1);
    }

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double total_sec = (t_end.tv_sec  - t_start.tv_sec) +
                       (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    char name_part[128] = {0}, tag_part[128] = {0};
    char *colon = strchr(tag, ':');
    if (colon) {
        strncpy(name_part, tag, (size_t)(colon - tag));
        strncpy(tag_part, colon + 1, sizeof(tag_part) - 1);
    } else {
        strncpy(name_part, tag, sizeof(name_part) - 1);
        strncpy(tag_part, "latest", sizeof(tag_part) - 1);
    }

    /*
     * BUG6 FIX: pass existing_created so a fully-cached rebuild preserves
     * the original timestamp → same manifest digest → reproducible builds.
     */
    if (write_manifest(tag, name_part, tag_part, &result,
                       docksmith_dir, existing_created) != 0) {
        fprintf(stderr, "Error: failed to write image manifest.\n");
        exit(1);
    }

    /* Read back the digest for the success line */
    char manifest_digest[65] = "unknown";
    char *mj = load_manifest_json(tag, docksmith_dir);
    if (mj) {
        char full_digest[128] = "";
        read_manifest_field(mj, "digest", full_digest, sizeof(full_digest));
        free(mj);
        /* full_digest = "sha256:XXXX" — extract just the hex */
        char *hex = strstr(full_digest, "sha256:");
        if (hex) strncpy(manifest_digest, hex + 7, 64);
        else     strncpy(manifest_digest, full_digest, 64);
        manifest_digest[64] = '\0';
    }

    rt_free_env_array(result.env, (size_t)result.env_count);

    /* Spec §5.2: "Successfully built sha256:<12> name:tag (Xs)" */
    printf("Successfully built sha256:%.12s %s (%.2fs)\n",
           manifest_digest, tag, total_sec);
}

/* ══════════════════════════════════════════════════════════════════════════
 * docksmith images
 * ══════════════════════════════════════════════════════════════════════════ */

void handle_images(void)
{
    char docksmith_dir[PATH_BUF];
    cli_get_docksmith_dir(docksmith_dir);

    char images_dir[PATH_BUF];
    snprintf(images_dir, sizeof(images_dir), "%s/images", docksmith_dir);

    DIR *d = opendir(images_dir);
    if (!d) { printf("No images found.\n"); return; }

    printf("%-20s %-15s %-15s %-25s\n", "NAME", "TAG", "ID", "CREATED");
    printf("%-20s %-15s %-15s %-25s\n",
           "────────────────────", "───────────────",
           "───────────────",     "─────────────────────────");

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, ".json") == NULL) continue;

        /* BUG1 FIX: read file directly — no more load_manifest_json(NULL,NULL) */
        char json_path[PATH_BUF];
        snprintf(json_path, sizeof(json_path),
                 "%s/%s", images_dir, entry->d_name);

        FILE *f = fopen(json_path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        char *json = malloc((size_t)sz + 1);
        if (!json) { fclose(f); continue; }
        fread(json, 1, (size_t)sz, f);
        json[sz] = '\0';
        fclose(f);

        char name[128]   = "?";
        char tag[64]     = "?";
        char digest[128] = "?";
        char created[64] = "?";

        read_manifest_field(json, "name",    name,    sizeof(name));
        read_manifest_field(json, "tag",     tag,     sizeof(tag));
        read_manifest_field(json, "digest",  digest,  sizeof(digest));
        read_manifest_field(json, "created", created, sizeof(created));
        free(json);

        char id[16] = "?";
        char *hex = strstr(digest, "sha256:");
        if (hex) strncpy(id, hex + 7, 12);
        else     strncpy(id, digest,  12);
        id[12] = '\0';

        printf("%-20s %-15s %-15s %-25s\n", name, tag, id, created);
        found++;
    }

    closedir(d);
    if (!found) printf("(no images)\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * docksmith rmi
 * ══════════════════════════════════════════════════════════════════════════ */

void handle_rmi(const char *tag)
{
    if (!tag || tag[0] == '\0') {
        fprintf(stderr, "Error: rmi requires an image name, e.g. myapp:latest\n");
        exit(1);
    }

    char docksmith_dir[PATH_BUF];
    cli_get_docksmith_dir(docksmith_dir);

    char *json = load_manifest_json(tag, docksmith_dir);
    if (!json) {
        fprintf(stderr, "Error: image '%s' not found in local store.\n", tag);
        exit(1);
    }

    /*
     * BUG3 FIX: original while-loop with OR caused infinite loop.
     * Clean sequential scan using a single search pattern at a time.
     *
     * Also: scope to layers array only to avoid deleting by manifest digest.
     * The manifest digest is NOT a layer file so skipping it is harmless,
     * but being explicit prevents j==64 from matching it (it always will).
     * We scope by finding the layers array boundary first.
     */
    int deleted_layers = 0;

    /* Find layers array */
    const char *layers_start = strstr(json, "\"layers\":");
    if (!layers_start) layers_start = json;  /* fallback: scan whole json */
    const char *arr = strchr(layers_start, '[');
    const char *arr_end = json + strlen(json);
    if (arr) {
        int depth = 0;
        const char *e = arr;
        while (*e) {
            if (*e == '[') depth++;
            else if (*e == ']') { depth--; if (depth == 0) { arr_end = e; break; } }
            e++;
        }
    }

    const char *p = arr ? arr : json;
    while (p < arr_end) {
        const char *found = strstr(p, "\"digest\": \"sha256:");
        if (!found || found >= arr_end)
            found = strstr(p, "\"digest\":\"sha256:");
        if (!found || found >= arr_end) break;

        const char *val = strstr(found, "sha256:");
        if (!val || val >= arr_end) break;
        val += 7;

        char digest[65] = {0};
        int j = 0;
        while (val[j] && val[j] != '"' && j < 64) {
            digest[j] = val[j]; j++;
        }
        digest[j] = '\0';

        if (j == 64) {
            char layer_path[PATH_BUF];
            snprintf(layer_path, sizeof(layer_path),
                     "%s/layers/%s.tar", docksmith_dir, digest);
            
            int is_shared = 0;
            char images_dir[PATH_BUF];
            snprintf(images_dir, sizeof(images_dir), "%s/images", docksmith_dir);
            DIR *d = opendir(images_dir);
            if (d) {
                struct dirent *entry;
                char safe_current[PATH_BUF];
                safe_name(tag, safe_current);
                strcat(safe_current, ".json");
                
                while ((entry = readdir(d)) != NULL) {
                    if (strstr(entry->d_name, ".json") == NULL) continue;
                    if (strcmp(entry->d_name, safe_current) == 0) continue;
                    
                    char jpath[PATH_BUF];
                    snprintf(jpath, sizeof(jpath), "%s/%s", images_dir, entry->d_name);
                    FILE *jf = fopen(jpath, "r");
                    if (jf) {
                        char search_str[128];
                        snprintf(search_str, sizeof(search_str), "\"sha256:%s\"", digest);
                        char line[1024];
                        while (fgets(line, sizeof(line), jf)) {
                            if (strstr(line, search_str)) {
                                is_shared = 1;
                                break;
                            }
                        }
                        fclose(jf);
                    }
                    if (is_shared) break;
                }
                closedir(d);
            }

            if (!is_shared) {
                if (unlink(layer_path) == 0) {
                    printf("Deleted layer: %s\n", layer_path);
                    deleted_layers++;
                }
            } else {
                printf("Skipped shared layer: %s\n", layer_path);
            }
        }
        p = val + j + 1;
    }

    free(json);

    char safe[PATH_BUF], manifest_path[PATH_BUF];
    safe_name(tag, safe);
    snprintf(manifest_path, sizeof(manifest_path),
             "%s/images/%s.json", docksmith_dir, safe);

    if (unlink(manifest_path) == 0)
        printf("Deleted manifest: %s\n", manifest_path);
    else
        fprintf(stderr, "Warning: could not delete manifest: %s\n",
                strerror(errno));

    printf("Removed image '%s' (%d layer(s) deleted)\n", tag, deleted_layers);
}

/* ══════════════════════════════════════════════════════════════════════════
 * docksmith run
 *
 * BUG2b FIX: Layer path extraction now scopes to the "layers" JSON array
 * only, so the top-level manifest digest is never mistaken for a layer.
 * ══════════════════════════════════════════════════════════════════════════ */

void handle_run(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: docksmith run [-e KEY=VALUE ...] <name:tag> [cmd]\n");
        exit(1);
    }

    char docksmith_dir[PATH_BUF];
    cli_get_docksmith_dir(docksmith_dir);

    rt_env_t overrides[32];
    int      override_count = 0;
    char    *tag            = NULL;
    int      cmd_start      = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -e requires KEY=VALUE\n");
                exit(1);
            }
            i++;
            if (override_count < 32) {
                rt_error_t err = rt_parse_env_string(argv[i],
                                                     &overrides[override_count]);
                if (err != RT_OK) {
                    fprintf(stderr, "Error: invalid -e argument '%s'\n", argv[i]);
                    exit(1);
                }
                override_count++;
            }
        } else if (!tag) {
            tag = argv[i];
            cmd_start = i + 1;
        }
    }

    if (!tag) {
        fprintf(stderr, "Error: no image name specified\n");
        exit(1);
    }

    char *json = load_manifest_json(tag, docksmith_dir);
    if (!json) {
        fprintf(stderr,
                "Error: image '%s' not found. "
                "Run 'docksmith images' to list available images.\n", tag);
        rt_free_env_array(overrides, (size_t)override_count);
        exit(1);
    }

    char working_dir[256] = "/";
    char cmd_json[512]    = "";
    read_manifest_field(json, "WorkingDir", working_dir, sizeof(working_dir));
    read_manifest_array_field(json, "Cmd", cmd_json, sizeof(cmd_json));

    /* Parse image ENV */
    rt_env_t image_env[32];
    int      image_env_count = 0;

    const char *env_section = strstr(json, "\"Env\": [");
    if (!env_section) env_section = strstr(json, "\"Env\":[");
    if (env_section) {
        const char *ep = strchr(env_section, '[');
        if (ep) ep++;
        while (ep && *ep && *ep != ']' && image_env_count < 32) {
            const char *q = strchr(ep, '"');
            if (!q || q >= strchr(env_section, ']')) break;
            q++;
            char kv[256] = {0};
            int j = 0;
            while (*q && *q != '"' && j < 255) kv[j++] = *q++;
            kv[j] = '\0';
            if (j > 0 && strchr(kv, '=')) {
                rt_parse_env_string(kv, &image_env[image_env_count]);
                image_env_count++;
            }
            ep = (*q == '"') ? q + 1 : q;
        }
    }

    /*
     * BUG2b FIX: Extract layer digests ONLY from within the "layers": [...]
     * array. This prevents the top-level manifest digest from being mistaken
     * as a layer digest.
     */
    const char *layer_paths_storage[MAX_LAYERS_CLI];
    char        layer_path_bufs[MAX_LAYERS_CLI][PATH_BUF];
    int         layer_count = 0;

    /* Find the layers array */
    const char *layers_section = strstr(json, "\"layers\":");
    if (!layers_section) layers_section = strstr(json, "\"layers\" :");
    const char *larr_start = layers_section ? strchr(layers_section, '[') : NULL;
    const char *larr_end   = json + strlen(json);

    if (larr_start) {
        int depth = 0;
        const char *e = larr_start;
        while (*e) {
            if (*e == '[') depth++;
            else if (*e == ']') { depth--; if (depth == 0) { larr_end = e; break; } }
            e++;
        }
    }

    const char *lp = larr_start ? larr_start : json;
    while (layer_count < MAX_LAYERS_CLI && lp < larr_end) {
        const char *d = strstr(lp, "\"digest\": \"sha256:");
        if (!d || d >= larr_end) d = strstr(lp, "\"digest\":\"sha256:");
        if (!d || d >= larr_end) break;

        const char *val = strstr(d, "sha256:");
        if (!val || val >= larr_end) break;
        val += 7;

        char digest[65] = {0};
        int j = 0;
        while (val[j] && val[j] != '"' && j < 64) {
            digest[j] = val[j]; j++;
        }
        digest[j] = '\0';

        if (j == 64) {
            snprintf(layer_path_bufs[layer_count], PATH_BUF,
                     "%s/layers/%s.tar", docksmith_dir, digest);
            layer_paths_storage[layer_count] = layer_path_bufs[layer_count];
            layer_count++;
        }
        lp = val + j + 1;
    }

    free(json);

    if (layer_count == 0) {
        fprintf(stderr, "Error: image '%s' has no layers.\n", tag);
        exit(1);
    }

    /* Determine command */
    const char *cmd_argv[64];
    int         cmd_argc = 0;

    if (cmd_start >= 0 && cmd_start < argc) {
        for (int i = cmd_start; i < argc && cmd_argc < 63; i++)
            cmd_argv[cmd_argc++] = argv[i];
        cmd_argv[cmd_argc] = NULL;
    } else {
        cmd_argc = parse_cmd_json_array(cmd_json, cmd_argv, 64);
    }

    if (cmd_argc == 0 || !cmd_argv[0] || cmd_argv[0][0] == '\0') {
        fprintf(stderr,
                "Error: no CMD defined in image '%s' and no command given.\n"
                "       Usage: docksmith run %s <cmd>\n", tag, tag);
        rt_free_env_array(overrides,  (size_t)override_count);
        rt_free_env_array(image_env,  (size_t)image_env_count);
        exit(1);
    }

    /* Call rt_run() — same isolation primitive as build-time RUN */
    rt_config_t cfg = {
        .layers             = layer_paths_storage,
        .layer_count        = (size_t)layer_count,
        .working_dir        = working_dir,
        .image_env          = image_env,
        .image_env_count    = (size_t)image_env_count,
        .env_overrides      = overrides,
        .env_override_count = (size_t)override_count,
        .cmd                = cmd_argv,
        .keep_rootfs        = 0,
    };

    rt_result_t result;
    rt_error_t  rc = rt_run(&cfg, &result);

    rt_free_env_array(overrides, (size_t)override_count);
    rt_free_env_array(image_env, (size_t)image_env_count);

    if (rc != RT_OK) {
        fprintf(stderr, "Error: container failed: %s\n", rt_strerror(rc));
        exit(1);
    }

    exit(result.exit_code);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main dispatcher
 * ══════════════════════════════════════════════════════════════════════════ */

void handle_cli(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: docksmith <command>\n");
        printf("Commands: build, images, rmi, run\n");
        return;
    }

    if (strcmp(argv[1], "build") == 0) {
        handle_build(argc, argv);
    } else if (strcmp(argv[1], "images") == 0) {
        handle_images();
    } else if (strcmp(argv[1], "rmi") == 0) {
        if (argc < 3) { fprintf(stderr, "Error: rmi requires an image name\n"); exit(1); }
        handle_rmi(argv[2]);
    } else if (strcmp(argv[1], "run") == 0) {
        handle_run(argc, argv);
    } else {
        fprintf(stderr, "Error: unknown command '%s'\nCommands: build, images, rmi, run\n",
                argv[1]);
        exit(1);
    }
}
