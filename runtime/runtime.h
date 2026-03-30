#ifndef DOCKSMITH_RUNTIME_H
#define DOCKSMITH_RUNTIME_H

/*
 * runtime.h — Public API for the Docksmith Container Runtime
 *
 * This header defines every type and function that the Build Engine (engine/)
 * and the CLI (cli/) need to call into the runtime.  Nothing else in this file
 * is part of the public contract.
 *
 * Isolation model
 * ───────────────
 * We use chroot(2) rather than pivot_root(2) because the project runs as root
 * inside a Linux VM and chroot is sufficient to satisfy the hard requirement
 * ("a file written inside a container must not appear on the host").
 * The child process is created with clone(2) / fork(2)+unshare(2) so it runs
 * in new PID and Mount namespaces, giving it an isolated process tree and
 * mount table before we chroot into the assembled rootfs.
 *
 * The same isolation primitive (run_in_container_root) is called by BOTH:
 *   • The build engine for every RUN instruction.
 *   • The CLI for `docksmith run`.
 * This satisfies the "one primitive, used in two places" hard requirement.
 */

#include <sys/types.h>   /* pid_t */
#include <stddef.h>      /* size_t */

/* ── Error codes returned by all public functions ─────────────────────────── */
typedef enum {
    RT_OK              =  0,
    RT_ERR_ARGS        = -1,   /* bad arguments passed by caller              */
    RT_ERR_MANIFEST    = -2,   /* cannot read / parse image manifest          */
    RT_ERR_LAYER       = -3,   /* a layer tar file is missing or unreadable   */
    RT_ERR_EXTRACT     = -4,   /* tar extraction failed                       */
    RT_ERR_TMPDIR      = -5,   /* cannot create temporary rootfs directory    */
    RT_ERR_FORK        = -6,   /* fork / clone failed                         */
    RT_ERR_CHROOT      = -7,   /* chroot(2) failed                            */
    RT_ERR_EXEC        = -8,   /* execvp(3) failed inside the container       */
    RT_ERR_WAIT        = -9,   /* waitpid(2) failed                           */
    RT_ERR_WORKDIR     = -10,  /* cannot chdir to WorkingDir inside rootfs    */
    RT_ERR_NAMESPACE   = -11,  /* unshare(2) failed                           */
    RT_ERR_NOCMD       = -12,  /* no CMD defined and no override given        */
    RT_ERR_NOMEM       = -13,  /* malloc / strdup failed                      */
} rt_error_t;

/* Convert an rt_error_t to a human-readable string. Never returns NULL.      */
const char *rt_strerror(rt_error_t err);

/* ── Key-value pair (used for environment variables) ─────────────────────── */
typedef struct {
    char *key;    /* e.g. "PATH"             */
    char *value;  /* e.g. "/usr/local/bin"   */
} rt_env_t;

/* ── Runtime configuration passed by the caller ──────────────────────────── */
typedef struct {
    /*
     * Ordered list of layer tar paths to extract into the rootfs.
     * layers[0] is the oldest (base image layer 1), layers[n-1] is newest.
     * Must have at least one entry.
     */
    const char **layers;      /* array of absolute paths to .tar files       */
    size_t       layer_count;

    /*
     * Working directory inside the container filesystem.
     * Set to "/" when the image manifest has no WorkingDir.
     */
    const char *working_dir;

    /*
     * Image-level environment variables (from the manifest "Env" array).
     * These are applied first; env_overrides take precedence.
     */
    rt_env_t   *image_env;
    size_t      image_env_count;

    /*
     * Per-run overrides supplied via `docksmith run -e KEY=VALUE`.
     * NULL / 0 when called from the build engine (RUN instruction).
     */
    rt_env_t   *env_overrides;
    size_t      env_override_count;

    /*
     * The command to execute inside the container, as a NULL-terminated
     * argv array (just like execvp).  e.g. {"python", "main.py", NULL}.
     * Must have at least one non-NULL entry.
     */
    const char **cmd;

    /*
     * When non-zero, keep the temporary rootfs directory after the container
     * exits (useful for debugging).  Normally 0.
     */
    int keep_rootfs;
} rt_config_t;

/* ── Result returned after a container exits ─────────────────────────────── */
typedef struct {
    int exit_code;            /* process exit status as returned by WEXITSTATUS */
    char rootfs_path[512];    /* path of the temp dir (informational / cleanup) */
} rt_result_t;

/* ── Primary public entry point ───────────────────────────────────────────── */

/*
 * rt_run  —  Assemble the rootfs, isolate a process, exec the command.
 *
 * Caller (Engine or CLI) fills in a rt_config_t, then calls rt_run().
 * On success RT_OK is returned and *result is populated.
 * On failure a negative rt_error_t is returned; *result is undefined.
 *
 * Thread-safety: NOT thread-safe (uses global temp dir per call).  The CLI
 * is single-threaded so this is fine for the project scope.
 */
rt_error_t rt_run(const rt_config_t *cfg, rt_result_t *result);

/* ── Convenience helpers used by the build engine only ───────────────────── */

/*
 * rt_parse_env_string  —  Split "KEY=VALUE" into an rt_env_t.
 *
 * The caller owns the returned key/value strings and must free them.
 * Returns RT_OK or RT_ERR_ARGS if the string has no '='.
 */
rt_error_t rt_parse_env_string(const char *kv, rt_env_t *out);

/*
 * rt_free_env_array  —  Free key/value strings inside an rt_env_t array.
 *
 * Does NOT free the array pointer itself — only the strings inside.
 */
void rt_free_env_array(rt_env_t *env, size_t count);

#endif /* DOCKSMITH_RUNTIME_H */