/*
 * runtime.c — Docksmith Container Runtime
 *
 * Responsibilities (matching the project spec verbatim):
 *   1. Read the image manifest to get layers, ENV, CMD, WorkingDir.          ← caller's job; we receive rt_config_t
 *   2. Extract layer tar files in order into a temporary directory.
 *   3. Create the root filesystem from the extracted layers.
 *   4. Isolate the process so it runs inside this root (chroot).
 *   5. Set environment variables from the image and any -e overrides.
 *   6. Set the working directory inside the container filesystem.
 *   7. Execute the command (CMD or runtime override).
 *   8. Wait for the process to finish and print the exit code.
 *   9. Delete the temporary container filesystem after execution.
 *
 * Isolation mechanism (used for BOTH `docksmith run` AND `RUN` during build):
 *   • fork(2)   — create a child process
 *   • unshare(2) with CLONE_NEWNS | CLONE_NEWPID — give the child its own
 *     mount namespace and PID namespace so it cannot see host mounts or
 *     host PIDs.  (CLONE_NEWNET is intentionally omitted — networking is
 *     out of scope per section 1 of the spec.)
 *   • chroot(2) — restrict the child's filesystem view to the assembled rootfs
 *   • chdir(2)  — honour WorkingDir inside the new root
 *   • execvp(3) — replace the child with the requested command
 *
 * Build notes:
 *   gcc -std=c11 -Wall -Wextra -D_GNU_SOURCE runtime.c -o runtime.o -c
 *   Link with -lssl -lcrypto only if you call the SHA helpers from cache.c;
 *   this file itself does not need libssl.
 */

/* _GNU_SOURCE is defined via -D_GNU_SOURCE in the compiler flags (Makefile).
 * It enables: unshare(2), execvpe(3), MS_REC, MS_PRIVATE, mkdtemp(3). */

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>          /* fork, chroot, chdir, execvp, unlink */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>        /* waitpid, WEXITSTATUS */
#include <sys/mount.h>       /* mount(2), MS_REC, MS_PRIVATE */
#include <sched.h>           /* unshare, CLONE_NEWNS, CLONE_NEWPID */
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>          /* PATH_MAX */
#include <stdarg.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Internal logging helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * rt_log — print a prefixed message to stderr.
 * We write to stderr so that container stdout/stdin stays clean for the user.
 */
static void rt_log(const char *level, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[runtime][%s] ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

#define LOG_INFO(...)  rt_log("INFO ",  __VA_ARGS__)
#define LOG_ERROR(...) rt_log("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...) rt_log("DEBUG", __VA_ARGS__)

/* ══════════════════════════════════════════════════════════════════════════
 * rt_strerror
 * ══════════════════════════════════════════════════════════════════════════ */

const char *rt_strerror(rt_error_t err)
{
    switch (err) {
        case RT_OK:            return "success";
        case RT_ERR_ARGS:      return "invalid arguments";
        case RT_ERR_MANIFEST:  return "cannot read image manifest";
        case RT_ERR_LAYER:     return "layer tar file missing or unreadable";
        case RT_ERR_EXTRACT:   return "tar extraction failed";
        case RT_ERR_TMPDIR:    return "cannot create temporary rootfs directory";
        case RT_ERR_FORK:      return "fork failed";
        case RT_ERR_CHROOT:    return "chroot failed";
        case RT_ERR_EXEC:      return "exec failed inside container";
        case RT_ERR_WAIT:      return "waitpid failed";
        case RT_ERR_WORKDIR:   return "cannot chdir to WorkingDir inside rootfs";
        case RT_ERR_NAMESPACE: return "unshare (namespace setup) failed";
        case RT_ERR_NOCMD:     return "no CMD defined and no command override given";
        case RT_ERR_NOMEM:     return "memory allocation failed";
        default:               return "unknown error";
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 1 — Create a unique temporary directory for the rootfs
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * make_temp_rootfs
 *
 * Creates /tmp/docksmith-XXXXXX and writes the path into `out_path`
 * (which must be at least PATH_MAX bytes).
 *
 * Why /tmp?  It is writable by root and survives the build/run session.
 * We clean it up ourselves in step 9.
 */
static rt_error_t make_temp_rootfs(char *out_path)
{
    /* mkdtemp requires the template to end in exactly six 'X' characters. */
    snprintf(out_path, PATH_MAX, "/tmp/docksmith-XXXXXX");

    if (mkdtemp(out_path) == NULL) {
        LOG_ERROR("mkdtemp failed: %s", strerror(errno));
        return RT_ERR_TMPDIR;
    }

    LOG_INFO("temporary rootfs: %s", out_path);
    return RT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 2 — Extract layer tar files in order
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * extract_layer
 *
 * Extracts a single tar archive into `dest_dir`.
 *
 * We delegate to the system `tar` binary rather than reimplementing a tar
 * parser in C.  This keeps the code simple and correct for all tar features
 * (symlinks, hard links, special files) that a real base image might use.
 *
 * Flags:
 *   -x  extract
 *   -f  read from file (the layer path)
 *   -C  change to dest_dir before extracting
 *   --no-same-owner  do not chown to the tar-recorded uid/gid
 *                    (avoids permission errors when running as non-root in a VM)
 *
 * Security: layer_path and dest_dir come from our own manifest; they are not
 * user-supplied shell strings, but we still avoid passing them to sh -c.
 * execvp builds the argument list directly, which prevents shell injection.
 */
static rt_error_t extract_layer(const char *layer_path, const char *dest_dir)
{
    /* Verify the layer file exists and is readable before we try to extract. */
    if (access(layer_path, R_OK) != 0) {
        LOG_ERROR("layer not found or unreadable: %s (%s)",
                  layer_path, strerror(errno));
        return RT_ERR_LAYER;
    }

    LOG_INFO("extracting layer: %s → %s", layer_path, dest_dir);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork for tar: %s", strerror(errno));
        return RT_ERR_FORK;
    }

    if (pid == 0) {
        /* ── child: exec tar ── */
        /* execvp replaces the child process; if it returns, exec failed.    */
        execlp("tar", "tar",
               "-x",
               "--no-same-owner",
               "-f", layer_path,
               "-C", dest_dir,
               (char *)NULL);
        /* Only reached on exec failure. */
        fprintf(stderr, "[runtime][ERROR] execlp(tar) failed: %s\n",
                strerror(errno));
        _exit(127);          /* 127 = "command not found" convention         */
    }

    /* ── parent: wait for tar to finish ── */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        LOG_ERROR("waitpid(tar): %s", strerror(errno));
        return RT_ERR_WAIT;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG_ERROR("tar exited with code %d for layer %s",
                  WEXITSTATUS(status), layer_path);
        return RT_ERR_EXTRACT;
    }

    return RT_OK;
}

/*
 * extract_all_layers
 *
 * Calls extract_layer for each layer in order.
 * Later layers overwrite earlier ones at the same path — this is how
 * the union-like layered filesystem is assembled without a union mount.
 */
static rt_error_t extract_all_layers(const rt_config_t *cfg,
                                     const char *rootfs)
{
    for (size_t i = 0; i < cfg->layer_count; i++) {
        LOG_INFO("layer %zu/%zu: %s", i + 1, cfg->layer_count, cfg->layers[i]);

        rt_error_t rc = extract_layer(cfg->layers[i], rootfs);
        if (rc != RT_OK)
            return rc;
    }
    return RT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 3 — Build the merged environment for the container process
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * build_env
 *
 * Merges image_env and env_overrides into a NULL-terminated array suitable
 * for execve(2) / execvp(3).
 *
 * Merge rule (matching spec section 6):
 *   - Start with all image ENV values.
 *   - For each -e KEY=VALUE override: if KEY already exists, replace it;
 *     otherwise append it.
 *
 * The caller must free the returned array with free_env_strings().
 *
 * Returns NULL on allocation failure.
 */
static char **build_env(const rt_config_t *cfg)
{
    /*
     * Upper bound: image_env_count + env_override_count + 1 (NULL terminator).
     * We may use fewer slots if overrides replace existing keys.
     */
    size_t max = cfg->image_env_count + cfg->env_override_count + 1;
    char **env = calloc(max, sizeof(char *));
    if (!env) {
        LOG_ERROR("calloc env: %s", strerror(errno));
        return NULL;
    }

    size_t n = 0;   /* number of entries written so far */

    /* 1. Copy all image env entries. */
    for (size_t i = 0; i < cfg->image_env_count; i++) {
        /* Format: "KEY=VALUE\0" */
        size_t len = strlen(cfg->image_env[i].key) + 1 +
                     strlen(cfg->image_env[i].value) + 1;
        env[n] = malloc(len);
        if (!env[n]) goto oom;
        snprintf(env[n], len, "%s=%s",
                 cfg->image_env[i].key,
                 cfg->image_env[i].value);
        n++;
    }

    /* 2. Apply overrides: replace existing KEY or append. */
    for (size_t i = 0; i < cfg->env_override_count; i++) {
        const char *ok  = cfg->env_overrides[i].key;
        const char *ov  = cfg->env_overrides[i].value;
        size_t key_len  = strlen(ok);
        int found = 0;

        /* Search existing entries for a matching KEY. */
        for (size_t j = 0; j < n; j++) {
            if (strncmp(env[j], ok, key_len) == 0 && env[j][key_len] == '=') {
                /* Replace this entry. */
                free(env[j]);
                size_t len = key_len + 1 + strlen(ov) + 1;
                env[j] = malloc(len);
                if (!env[j]) goto oom;
                snprintf(env[j], len, "%s=%s", ok, ov);
                found = 1;
                break;
            }
        }

        if (!found) {
            /* Append new entry. */
            size_t len = key_len + 1 + strlen(ov) + 1;
            env[n] = malloc(len);
            if (!env[n]) goto oom;
            snprintf(env[n], len, "%s=%s", ok, ov);
            n++;
        }
    }

    env[n] = NULL;   /* NULL-terminate the array */
    return env;

oom:
    LOG_ERROR("malloc failed while building environment");
    /* Free whatever we managed to allocate. */
    for (size_t i = 0; i < n; i++) free(env[i]);
    free(env);
    return NULL;
}

/*
 * free_env_strings — free the strings allocated by build_env().
 */
static void free_env_strings(char **env)
{
    if (!env) return;
    for (size_t i = 0; env[i] != NULL; i++)
        free(env[i]);
    free(env);
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 4 — Namespace setup (called INSIDE the child, BEFORE chroot)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * setup_namespaces
 *
 * Called by the child process after fork(), before chroot().
 *
 * CLONE_NEWNS  — new mount namespace: the child gets a private copy of the
 *                host's mount table.  Any mounts the child makes (or unmounts
 *                the host makes) are not visible across the boundary.
 *                This is the key primitive that makes "a file written inside
 *                a container must not appear on the host filesystem" true for
 *                the mount table view.  Combined with chroot, it fully
 *                isolates the container's view of the filesystem.
 *
 * CLONE_NEWPID — new PID namespace: the container process sees itself as
 *                PID 1.  Host processes are invisible.  Prevents the container
 *                from sending signals (kill -9, etc.) to host PIDs.
 *
 * Why not CLONE_NEWNET?  Networking is explicitly out of scope (spec §1).
 * Adding it without also setting up a veth pair / loopback would break
 * DNS/socket calls inside the container, so we leave it out.
 *
 * After unshare(CLONE_NEWNS) we re-mount the root as MS_PRIVATE | MS_REC so
 * that the MS_SHARED propagation of the host's rootfs does not leak bind-mount
 * events into the container's namespace.
 */
static rt_error_t setup_namespaces(void)
{
    /*
     * unshare(2) creates new namespaces for the calling process.
     * We do this in the child (after fork) so the parent (the CLI / engine)
     * is unaffected.
     */
    if (unshare(CLONE_NEWNS | CLONE_NEWPID) != 0) {
        fprintf(stderr, "[runtime][ERROR] unshare failed: %s\n",
                strerror(errno));
        return RT_ERR_NAMESPACE;
    }

    /*
     * Make the root mount private so events on the host's mount namespace
     * (e.g. the host mounting a USB stick) do not propagate into ours.
     * MS_REC applies this recursively to all submounts under "/".
     */
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        /*
         * This can fail with EINVAL in some minimal VMs where the root is
         * already private.  Treat that as non-fatal.
         */
        if (errno != EINVAL) {
            fprintf(stderr,
                    "[runtime][ERROR] mount --make-rprivate failed: %s\n",
                    strerror(errno));
            return RT_ERR_NAMESPACE;
        }
    }

    return RT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 5 — chroot + chdir  (filesystem isolation)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * isolate_filesystem
 *
 * Called inside the child after setup_namespaces().
 *
 * chroot(2) changes the root directory of the calling process to `rootfs`.
 * After chroot, any path starting with "/" resolves inside `rootfs`.
 * The process cannot cd / open paths that escape the new root
 * (assuming it has no open file descriptors pointing outside — which we
 * ensure by not caching any).
 *
 * chdir("/") resets the CWD to the new root so that relative paths work
 * correctly inside the container.  Without this, the process still has the
 * old CWD, which points outside the chroot, and "../../../etc/passwd" tricks
 * would work.
 *
 * The working_dir chdir is done AFTER chroot, so `working_dir` is resolved
 * relative to the container root.
 */
static rt_error_t isolate_filesystem(const char *rootfs,
                                     const char *working_dir)
{
    /* Change to rootfs on the host filesystem BEFORE chroot so the OS can
     * find it.  Using chroot(".") after chdir avoids a TOCTOU race where
     * the directory is renamed between stat and chroot. */
    if (chdir(rootfs) != 0) {
        fprintf(stderr, "[runtime][ERROR] chdir to rootfs %s: %s\n",
                rootfs, strerror(errno));
        return RT_ERR_CHROOT;
    }

    /*
     * chroot(".") — restrict filesystem root to cwd (== rootfs).
     * The "." form is idiomatic and avoids the race described above.
     */
    if (chroot(".") != 0) {
        fprintf(stderr, "[runtime][ERROR] chroot: %s\n", strerror(errno));
        return RT_ERR_CHROOT;
    }

    /* Reset CWD to the new root so all relative paths are safe. */
    if (chdir("/") != 0) {
        fprintf(stderr, "[runtime][ERROR] chdir / after chroot: %s\n",
                strerror(errno));
        return RT_ERR_CHROOT;
    }

    /*
     * Apply the image's WorkingDir.
     * If it doesn't exist (it should have been created by the build engine
     * for WORKDIR instructions), we return a clear error.
     */
    if (working_dir && working_dir[0] != '\0' &&
        strcmp(working_dir, "/") != 0) {

        if (chdir(working_dir) != 0) {
            fprintf(stderr,
                    "[runtime][ERROR] chdir to WorkingDir %s inside container: %s\n",
                    working_dir, strerror(errno));
            return RT_ERR_WORKDIR;
        }
    }

    return RT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 6 — Execute the command inside the container
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * exec_container_cmd
 *
 * Called inside the child after chroot.  Replaces the child process image
 * with the requested command using execvpe(3).
 *
 * execvpe is like execvp but also accepts a custom environment array.
 * It searches PATH (inside the container's PATH, since we've already chrooted)
 * for the executable.
 *
 * This function does NOT return on success (the process image is replaced).
 * On failure it prints to stderr and returns RT_ERR_EXEC — the caller
 * (_exit()s with a distinctive code so the parent can distinguish an exec
 * failure from the container process returning exit code 1).
 */
static rt_error_t exec_container_cmd(const char **cmd, char **env)
{
    /*
     * execvpe(3) — execute `cmd[0]` with args `cmd` and environment `env`.
     * The cast to (char *const *) is required by the POSIX prototype.
     */
    execvpe(cmd[0], (char *const *)cmd, env);

    /* Only reached if exec failed. */
    fprintf(stderr, "[runtime][ERROR] execvpe(%s) failed: %s\n",
            cmd[0], strerror(errno));
    return RT_ERR_EXEC;
}

/* ══════════════════════════════════════════════════════════════════════════
 * STEP 7 — Remove the temporary rootfs (recursive directory deletion)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * rmdir_recursive
 *
 * Walks `path` depth-first and removes every file and directory.
 * Equivalent to `rm -rf path`.
 *
 * We implement this ourselves rather than shelling out to rm so that:
 *   (a) we have full control over error reporting, and
 *   (b) we avoid a second fork/exec just for cleanup.
 *
 * Note: this is called on the HOST after the child has exited and we have
 * already left the chroot, so paths resolve on the host filesystem.
 */
static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        /* If it's a file, just unlink it. */
        unlink(path);
        return;
    }

    struct dirent *entry;
    char child[PATH_MAX];

    while ((entry = readdir(d)) != NULL) {
        /* Skip the self and parent directory entries. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        if (entry->d_type == DT_DIR) {
            rmdir_recursive(child);          /* recurse */
        } else {
            if (unlink(child) != 0) {
                LOG_ERROR("unlink %s: %s", child, strerror(errno));
            }
        }
    }

    closedir(d);

    if (rmdir(path) != 0) {
        LOG_ERROR("rmdir %s: %s", path, strerror(errno));
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC API — rt_run
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * rt_run
 *
 * Orchestrates all steps: create rootfs → extract layers → fork child →
 * (child) setup namespaces + chroot + exec → (parent) wait + cleanup.
 *
 * This is the single entry point called by BOTH the build engine (for RUN
 * instructions) and the CLI (for `docksmith run`).
 */
rt_error_t rt_run(const rt_config_t *cfg, rt_result_t *result)
{
    /* ── Validate inputs ── */
    if (!cfg || !result) {
        LOG_ERROR("rt_run called with NULL cfg or result");
        return RT_ERR_ARGS;
    }
    if (cfg->layer_count == 0 || !cfg->layers) {
        LOG_ERROR("no layers provided");
        return RT_ERR_ARGS;
    }
    if (!cfg->cmd || !cfg->cmd[0]) {
        LOG_ERROR("no command to execute");
        return RT_ERR_NOCMD;
    }

    rt_error_t rc = RT_OK;

    /* ── STEP 1: Create temporary rootfs directory ── */
    char rootfs[PATH_MAX];
    rc = make_temp_rootfs(rootfs);
    if (rc != RT_OK) return rc;

    strncpy(result->rootfs_path, rootfs, sizeof(result->rootfs_path) - 1);

    /* ── STEP 2: Extract all layer tars in order ── */
    rc = extract_all_layers(cfg, rootfs);
    if (rc != RT_OK) {
        if (!cfg->keep_rootfs) rmdir_recursive(rootfs);
        return rc;
    }

    /* ── STEP 3: Build the merged environment array ── */
    char **env = build_env(cfg);
    if (!env) {
        if (!cfg->keep_rootfs) rmdir_recursive(rootfs);
        return RT_ERR_NOMEM;
    }

    /* ── STEP 4: Fork the container child process ── */
    LOG_INFO("forking container process");
    pid_t child = fork();

    if (child < 0) {
        LOG_ERROR("fork: %s", strerror(errno));
        free_env_strings(env);
        if (!cfg->keep_rootfs) rmdir_recursive(rootfs);
        return RT_ERR_FORK;
    }

    /* ════════════════════════════════════════════════════════
     * CHILD process: namespace isolation → chroot → exec
     * ════════════════════════════════════════════════════════ */
    if (child == 0) {

        /* STEP 4a — Set up mount + PID namespaces. */
        rc = setup_namespaces();
        if (rc != RT_OK) _exit(126);

        /* STEP 5 — chroot into the assembled rootfs + set WorkingDir. */
        const char *wd = (cfg->working_dir && cfg->working_dir[0])
                         ? cfg->working_dir
                         : "/";
        rc = isolate_filesystem(rootfs, wd);
        if (rc != RT_OK) _exit(126);

        /* STEP 6 — exec the command. */
        /* exec_container_cmd only returns on failure. */
        exec_container_cmd(cfg->cmd, env);
        _exit(125);   /* exec failed; parent detects exit code 125 */
    }

    /* ════════════════════════════════════════════════════════
     * PARENT process: wait for child, then clean up.
     * ════════════════════════════════════════════════════════ */

    /* Free the env array — the child has its own copy after fork(). */
    free_env_strings(env);

    /* STEP 8 — Wait for the container to exit. */
    int status;
    LOG_INFO("waiting for container (pid %d)", (int)child);

    if (waitpid(child, &status, 0) < 0) {
        LOG_ERROR("waitpid: %s", strerror(errno));
        if (!cfg->keep_rootfs) rmdir_recursive(rootfs);
        return RT_ERR_WAIT;
    }

    /* Decode the wait status. */
    if (WIFEXITED(status)) {
        result->exit_code = WEXITSTATUS(status);
        /* Distinguish our own sentinel exit codes from the container's. */
        if (result->exit_code == 126) {
            LOG_ERROR("container setup failed (namespace or chroot error)");
            rc = RT_ERR_NAMESPACE;
        } else if (result->exit_code == 125) {
            LOG_ERROR("exec failed inside container");
            rc = RT_ERR_EXEC;
        } else {
            /* Normal container exit. */
            fprintf(stdout, "[runtime] Container exited with code %d\n",
                    result->exit_code);
        }
    } else if (WIFSIGNALED(status)) {
        result->exit_code = 128 + WTERMSIG(status);
        LOG_INFO("container killed by signal %d", WTERMSIG(status));
    }

    /* STEP 9 — Clean up the temporary rootfs. */
    if (!cfg->keep_rootfs) {
        LOG_INFO("cleaning up rootfs: %s", rootfs);
        rmdir_recursive(rootfs);
    }

    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC HELPER — rt_parse_env_string
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * rt_parse_env_string
 *
 * Split a "KEY=VALUE" string (as passed via `-e KEY=VALUE` on the CLI, or
 * stored in the manifest's Env array) into an rt_env_t.
 *
 * The caller is responsible for freeing out->key and out->value.
 */
rt_error_t rt_parse_env_string(const char *kv, rt_env_t *out)
{
    if (!kv || !out) return RT_ERR_ARGS;

    const char *eq = strchr(kv, '=');
    if (!eq) {
        LOG_ERROR("env string has no '=': %s", kv);
        return RT_ERR_ARGS;
    }

    size_t key_len = (size_t)(eq - kv);

    out->key = strndup(kv, key_len);
    if (!out->key) return RT_ERR_NOMEM;

    out->value = strdup(eq + 1);
    if (!out->value) {
        free(out->key);
        out->key = NULL;
        return RT_ERR_NOMEM;
    }

    return RT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC HELPER — rt_free_env_array
 * ══════════════════════════════════════════════════════════════════════════ */

void rt_free_env_array(rt_env_t *env, size_t count)
{
    if (!env) return;
    for (size_t i = 0; i < count; i++) {
        free(env[i].key);
        free(env[i].value);
    }
}