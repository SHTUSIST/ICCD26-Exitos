/* preload: the real userspace interception layer.
 *
 * Same mechanism family bpftime uses for userspace interposition -- resolve the
 * original libc entry with dlsym(RTLD_NEXT, ...) and decide, per call, whether to
 * take the call over or let it proceed. bpftime's syscall_context.hpp does exactly
 * this (require_symbol -> dlsym(RTLD_NEXT, name)); bpftime additionally lets the
 * policy be an eBPF program loaded at runtime, whereas here the policy is this file.
 *
 * DISCIPLINE (borrowed from namei_ext): one decision point, DEFAULT PASS, act only
 * on explicitly registered files, verify the target LBA lies inside that file's own
 * extents, and fall through to the original call on ANY doubt. Nothing is
 * accelerated unless EXITOS_FILES names it, so loading this library is inert by
 * default -- that is deliberate: a bug here corrupts data silently.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include "exitos_frontend_admission.h"
#include "exitos_frontend_config.h"
#include "exitos_frontend_fallocate.h"
#include "exitos_intercept.h"
#include "exitos_stats.h"

typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*pwrite_fn)(int, const void *, size_t, off_t);
typedef int     (*fdatasync_fn)(int);
typedef int     (*fsync_fn)(int);
typedef int     (*close_fn)(int);
typedef int     (*ftruncate_fn)(int, off_t);
typedef int     (*ftruncate64_fn)(int, off64_t);
typedef int     (*fallocate_fn)(int, int, off_t, off_t);
typedef int     (*dup_fn)(int);
typedef int     (*fcntl_fn)(int, int, ...);
typedef int     (*open_fn)(const char *, int, ...);
typedef int     (*openat_fn)(int, const char *, int, ...);
typedef int     (*open2_fn)(const char *, int);
typedef int     (*openat2_fn)(int, const char *, int);
typedef int     (*close_range_fn)(unsigned, unsigned, int);
typedef FILE   *(*freopen_fn)(const char *, const char *, FILE *);
typedef int     (*execve_fn)(const char *, char *const[], char *const[]);
typedef int     (*execveat_fn)(int, const char *, char *const[],
                               char *const[], int);
typedef int     (*fexecve_fn)(int, char *const[], char *const[]);

static write_fn      o_write;
static pwrite_fn     o_pwrite;
static fdatasync_fn  o_fdatasync;
static fsync_fn      o_fsync;
static close_fn      o_close;
static ftruncate_fn  o_ftruncate;
static ftruncate64_fn o_ftruncate64;
static fallocate_fn  o_fallocate;
static fallocate_fn  o_fallocate64;
static dup_fn        o_dup;
static fcntl_fn      o_fcntl;
static fcntl_fn      o_fcntl64;
static open_fn       o_open;
static openat_fn     o_openat;
static open2_fn      o___open_2;
static open2_fn      o___open64_2;
static openat2_fn    o___openat_2;
static openat2_fn    o___openat64_2;
static close_range_fn o_close_range;
static freopen_fn    o_freopen;
static execve_fn     o_execve;
static execveat_fn   o_execveat;
static fexecve_fn    o_fexecve;

static struct exitos_frontend_config *g_config;
static struct exitos_ctx *g_ctx;
static struct exitos_frontend_fallocate_runtime *g_fallocate_runtime;
static int   g_enabled;
static int   g_fastpath = 1;
static _Atomic int g_stats_dumped;
static _Atomic int g_worker_exit_stats_enabled;
static char g_worker_exit_stats_template[PATH_MAX];
/* Reentrancy guard: our own takeover path issues syscalls (ioctl/pwrite). Without
 * this a write from inside the fast path would re-enter the hook and recurse. */
/* initial-exec for the same reason as the bpftime hook: a general-dynamic
 * thread-local can allocate on a thread's first access, and allocating issues
 * syscalls that re-enter this layer. */
static __thread int __attribute__((tls_model("initial-exec"))) g_in_hook;
/* Depth of the library's OWN control path on this thread.
 *
 * g_in_hook alone cannot tell two very different things apart: a signal that
 * interrupted a raw transaction, and a call this library made itself while
 * building a registration. The wrappers below assumed the first and poisoned
 * the context -- or, for the sync and extent-mutation family, failed the call
 * with EIO -- on every nested call they saw.
 *
 * Registration cannot avoid making such calls. The direct-call API above
 * covers what this library opens by name, but not the open and close libc
 * issues from inside opendir() and fopen() while devguard and geom read sysfs,
 * and not src/iopath.c's staging probe, which opens /proc/self/pagemap
 * outright. Under LD_PRELOAD those land in the wrappers below, and the first
 * registration poisoned the context on its way to succeeding: every later
 * exitos_register_fd() then returned -ECANCELED from the poison gate and
 * printed "declined (normal kernel path)", while every write went to the
 * kernel and the shortcut took over nothing at all.
 *
 * A nonzero depth means this thread is synchronously inside library control
 * code, so a nested call is ours by construction, and it is safe to let it
 * through: the counter is raised only around control paths, never around a raw
 * write, so nothing the poison exists to protect is in flight. The bpftime
 * frontend carries the same counter for the same reason.
 *
 * The exec boundary deliberately does not consult it. Library control code
 * never replaces the process image, so an exec arriving mid-registration is
 * application reentry no matter what depth says, and it really does discard
 * the debt ledger. */
static __thread int __attribute__((tls_model("initial-exec"))) g_ctl_depth;
static __thread int __attribute__((tls_model("initial-exec"))) g_internal_fds[8];
static __thread unsigned __attribute__((tls_model("initial-exec"))) g_internal_depth;
static __thread int __attribute__((tls_model("initial-exec"))) g_internal_wrapper_active;
static void *sym(const char *n);

struct preload_admission_scope {
    int entered;
};

static struct preload_admission_scope preload_admission_enter(void)
{
    struct preload_admission_scope scope = {
        .entered = exitos_frontend_admission_enter()
    };
    return scope;
}

static void preload_admission_leave(struct preload_admission_scope *scope)
{
    if (scope->entered)
        exitos_frontend_admission_leave();
}

#define PRELOAD_ADMISSION_SCOPE(name)                                      \
    struct preload_admission_scope name                                    \
        __attribute__((cleanup(preload_admission_leave))) =                 \
            preload_admission_enter()

void exitos_internal_fd_enter(int fd)
{
    if (g_internal_depth < sizeof(g_internal_fds) / sizeof(g_internal_fds[0]))
        g_internal_fds[g_internal_depth++] = fd;
    else
        exitos_ctx_poison(g_ctx);
}

void exitos_internal_fd_leave(int fd)
{
    if (g_internal_depth && g_internal_fds[g_internal_depth - 1] == fd)
        g_internal_depth--;
    else {
        g_internal_depth = 0;
        exitos_ctx_poison(g_ctx);
    }
}

int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    if (!o_open)
        o_open = (open_fn)sym("open");
    if (!o_open) {
        errno = ENOSYS;
        return -1;
    }
    return o_open(path, flags, mode);
}

int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    if (!o_openat)
        o_openat = (openat_fn)sym("openat");
    if (!o_openat) {
        errno = ENOSYS;
        return -1;
    }
    return o_openat(dirfd, path, flags, mode);
}

int exitos_internal_close_call(int fd)
{
    if (!o_close)
        o_close = (close_fn)sym("close");
    if (!o_close) {
        errno = ENOSYS;
        return -1;
    }
    return o_close(fd);
}

int exitos_internal_ftruncate_call(int fd, off_t len)
{
    if (!o_ftruncate)
        o_ftruncate = (ftruncate_fn)sym("ftruncate");
    if (!o_ftruncate) {
        errno = ENOSYS;
        return -1;
    }
    return o_ftruncate(fd, len);
}

int exitos_internal_fallocate_call(int fd, int mode, off_t off, off_t len)
{
    if (!o_fallocate)
        o_fallocate = (fallocate_fn)sym("fallocate");
    if (!o_fallocate) {
        errno = ENOSYS;
        return -1;
    }
    return o_fallocate(fd, mode, off, len);
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{
    if (!o_pwrite)
        o_pwrite = (pwrite_fn)sym("pwrite");
    if (!o_pwrite) {
        errno = ENOSYS;
        return -1;
    }
    return o_pwrite(fd, buf, len, off);
}

int exitos_internal_fdatasync_call(int fd)
{
    if (!o_fdatasync)
        o_fdatasync = (fdatasync_fn)sym("fdatasync");
    if (!o_fdatasync) {
        errno = ENOSYS;
        return -1;
    }
    return o_fdatasync(fd);
}

/* Returns one only for the first frontend wrapper entered for the exact fd
 * currently marked by iopath.  If a signal interrupts that wrapper, the
 * active bit is already set, so even a mutation of the same numeric fd poisons
 * the context rather than masquerading as internal recursion. */
static int reentrant_internal_try_begin(int fd)
{
    /* Library-owned open/close/pwrite/fdatasync calls now bypass interposed
     * wrappers through explicit direct-call APIs.  Consequently no recursive
     * frontend mutation is internal, even if a legacy marker names its fd. */
    (void)fd;
    return 0;
}

static int reentrant_internal_begin(int fd)
{
    if (reentrant_internal_try_begin(fd))
        return 1;
    if (!g_ctl_depth)
        exitos_ctx_poison(g_ctx);
    return 0;
}

static void reentrant_internal_end(int internal)
{
    if (internal)
        g_internal_wrapper_active = 0;
}

static void *sym(const char *n){ void *p = dlsym(RTLD_NEXT, n); return p; }

static int worker_stats_template_valid(const char *path)
{
    const char *marker;

    if (!path || !*path || strlen(path) >= sizeof g_worker_exit_stats_template)
        return 0;
    marker = strstr(path, "%p");
    return marker && !strstr(marker + 2, "%p");
}

static void worker_stats_after_fork_child(void)
{
    atomic_store_explicit(&g_stats_dumped, 0, memory_order_relaxed);
}

__attribute__((constructor)) static void exitos_preload_init(void)
{
    int config_rc;

    o_write     = (write_fn)     sym("write");
    o_pwrite    = (pwrite_fn)    sym("pwrite");
    o_fdatasync = (fdatasync_fn) sym("fdatasync");
    o_fsync     = (fsync_fn)     sym("fsync");
    o_close     = (close_fn)     sym("close");
    o_ftruncate = (ftruncate_fn) sym("ftruncate");
    o_ftruncate64 = (ftruncate64_fn) sym("ftruncate64");
    o_fallocate = (fallocate_fn) sym("fallocate");
    o_fallocate64 = (fallocate_fn) sym("fallocate64");
    o_open      = (open_fn)      sym("open");
    o_openat    = (openat_fn)    sym("openat");
    o___open_2 = (open2_fn) sym("__open_2");
    o___open64_2 = (open2_fn) sym("__open64_2");
    o___openat_2 = (openat2_fn) sym("__openat_2");
    o___openat64_2 = (openat2_fn) sym("__openat64_2");
    o_close_range = (close_range_fn) sym("close_range");
    o_freopen    = (freopen_fn)    sym("freopen");
    o_execve     = (execve_fn)     sym("execve");
    o_execveat   = (execveat_fn)   sym("execveat");
    o_fexecve    = (fexecve_fn)    sym("fexecve");

    /* Constructors run once in production.  White-box tests may replace a
     * snapshot: close admission and drain the old lifetime before clearing its
     * borrowed alias or destroying its owner. */
    exitos_frontend_admission_quiesce();
    g_enabled = 0;
    g_fastpath = 1;
    g_ctx = NULL;
    if (g_fallocate_runtime) {
        g_in_hook = 1;
        g_ctl_depth++;
        exitos_frontend_fallocate_runtime_destroy(g_fallocate_runtime);
        g_ctl_depth--;
        g_in_hook = 0;
        g_fallocate_runtime = NULL;
    }
    if (g_config) {
        g_in_hook = 1;
        exitos_frontend_config_destroy(g_config);
        g_in_hook = 0;
        g_config = NULL;
    }
    config_rc = exitos_frontend_config_from_env(&g_config);
    if (config_rc == 0 && g_config) {
        g_enabled = exitos_frontend_config_enabled(g_config);
        g_ctx = exitos_frontend_config_ctx(g_config); /* borrowed */
        if (g_enabled) {
            g_fastpath = exitos_frontend_config_fastpath(g_config);
            config_rc = exitos_frontend_fallocate_runtime_create(
                &g_fallocate_runtime, g_config, g_ctx);
            if (config_rc == 0)
                exitos_frontend_admission_enable();
            else {
                g_enabled = 0;
                g_ctx = NULL;
                exitos_frontend_config_destroy(g_config);
                g_config = NULL;
            }
        }
    }
    if (getenv("EXITOS_VERBOSE"))
        fprintf(stderr, "[exitos] preload %s (files=%s%s%s)\n",
                g_enabled ? "ARMED" : "inert",
                g_enabled ? exitos_frontend_config_files(g_config) : "none",
                config_rc ? ", config-error=" : "",
                config_rc ? strerror(-config_rc) : "");
    const char *worker_optin = getenv("EXITOS_STATS_ON_WORKER_EXIT");
    const char *worker_stats = getenv("EXITOS_STATS");
    if (worker_optin && worker_optin[0] == '1' && worker_optin[1] == '\0' &&
            worker_stats_template_valid(worker_stats)) {
        memcpy(g_worker_exit_stats_template, worker_stats,
               strlen(worker_stats) + 1);
        if (pthread_atfork(NULL, NULL, worker_stats_after_fork_child) == 0)
            atomic_store_explicit(&g_worker_exit_stats_enabled, 1,
                                  memory_order_release);
    }
}

static int worker_exit_stats_enabled(void)
{
    return atomic_load_explicit(&g_worker_exit_stats_enabled,
                                memory_order_acquire);
}

static int stats_are_nonzero(void)
{
    for (int i = 0; i < EXITOS_STAT_MAX; ++i)
        if (exitos_stat_get((exitos_stat_id)i) != 0)
            return 1;
    return 0;
}

static void stats_dump_once(const char *path)
{
    if (path && atomic_exchange_explicit(&g_stats_dumped, 1,
                                         memory_order_relaxed) == 0)
        (void)exitos_stats_dump(path);
}

__attribute__((destructor)) static void exitos_preload_fini(void)
{
    const char *sp = worker_exit_stats_enabled()
                         ? g_worker_exit_stats_template
                         : getenv("EXITOS_STATS");
    /* Close the check/use window before changing aliases. Quiescence waits for
     * every caller that observed this lifetime before destroy releases it. */
    exitos_frontend_admission_quiesce();
    g_enabled = 0;
    if (sp && (!worker_exit_stats_enabled() || stats_are_nonzero()))
        stats_dump_once(sp);
    if (g_ctx) {
        g_in_hook = 1;
        g_ctl_depth++;
        exitos_frontend_fallocate_runtime_destroy(g_fallocate_runtime);
        g_fallocate_runtime = NULL;
        g_ctx = NULL;
        exitos_frontend_config_destroy(g_config);
        g_config = NULL;
        g_ctl_depth--;
        g_in_hook = 0;
    } else if (g_config) {
        exitos_frontend_fallocate_runtime_destroy(g_fallocate_runtime);
        g_fallocate_runtime = NULL;
        exitos_frontend_config_destroy(g_config);
        g_config = NULL;
    }
}

static char *append_u64(char *out, uint64_t value)
{
    char reversed[32];
    size_t used = 0;

    do {
        reversed[used++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (used)
        *out++ = reversed[--used];
    return out;
}

static int worker_stats_expand_path(char *out, size_t out_size)
{
    const char *in = g_worker_exit_stats_template;
    uint64_t pid = (uint64_t)syscall(SYS_getpid);
    char pid_text[32];
    char *pid_end = append_u64(pid_text, pid);
    size_t pid_size = (size_t)(pid_end - pid_text);
    size_t used = 0;

    while (*in) {
        const char *source = in;
        size_t size = 1;
        if (in[0] == '%' && in[1] == 'p') {
            source = pid_text;
            size = pid_size;
            in += 2;
        } else {
            ++in;
        }
        if (size >= out_size - used)
            return -1;
        memcpy(out + used, source, size);
        used += size;
    }
    out[used] = '\0';
    return 0;
}

static int worker_stats_dump_raw_once(void)
{
    char path[PATH_MAX];
    char payload[EXITOS_STAT_MAX * 32];
    char *cursor = payload;
    size_t total;
    int fd;

    int expected = 0;
    uint64_t blocked = UINT64_MAX;

    (void)syscall(SYS_rt_sigprocmask, SIG_BLOCK, &blocked, NULL,
                  sizeof blocked);
    if (!atomic_compare_exchange_strong_explicit(
            &g_stats_dumped, &expected, 1,
            memory_order_relaxed, memory_order_relaxed))
        return 0;
    if (!stats_are_nonzero() || worker_stats_expand_path(path, sizeof path) != 0)
        return 1;
    for (int i = 0; i < EXITOS_STAT_MAX; ++i) {
        cursor = append_u64(cursor, exitos_stat_get((exitos_stat_id)i));
        *cursor++ = '\n';
    }
    total = (size_t)(cursor - payload);
    fd = (int)syscall(SYS_openat, AT_FDCWD, path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      0600);
    if (fd < 0)
        return 1;
    size_t written = 0;
    while (written < total) {
        ssize_t amount = syscall(SYS_write, fd, payload + written,
                                 total - written);
        if (amount <= 0)
            break;
        written += (size_t)amount;
    }
    if (written == total)
        (void)syscall(SYS_fsync, fd);
    (void)syscall(SYS_close, fd);
    return 1;
}

/* fio process workers intentionally use _exit/_Exit, which bypasses DSO
 * destructors.  The runner opts into this cold path only for process-mode
 * measurement cells.  Zero-counter parents are deliberately omitted so the
 * evidence set contains exactly the worker files that performed takeovers. */
__attribute__((noreturn)) void _exit(int status)
{
    if (worker_exit_stats_enabled() && !worker_stats_dump_raw_once()) {
        (void)syscall(SYS_exit, status);
        __builtin_unreachable();
    }
    (void)syscall(SYS_exit_group, status);
    (void)syscall(SYS_exit, status);
    __builtin_unreachable();
}

__attribute__((noreturn)) void _Exit(int status)
{
    _exit(status);
}

static void maybe_register(int admitted, int fd, const char *path)
{
    if (!admitted || fd < 0 ||
        !exitos_frontend_config_path_selected(g_config, path)) return;
    g_in_hook = 1;
    g_ctl_depth++;
    int rc = exitos_register_fd(g_ctx, fd);          /* refuses on unsafe devices */
    g_ctl_depth--;
    g_in_hook = 0;
    if (rc != 0) exitos_stat_inc(EXITOS_STAT_DECLINED);
    if (getenv("EXITOS_VERBOSE")) {
        if (rc == 0)
            fprintf(stderr, "[exitos] register fd=%d %s -> FAST PATH\n",
                    fd, path);
        else
            fprintf(stderr, "[exitos] register fd=%d %s -> declined "
                    "(normal kernel path): %s\n", fd, path, strerror(-rc));
    }
}

int open(const char *path, int flags, ...)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    mode_t m = 0;
    /* O_TMPFILE takes the mode argument too. Reading it only for O_CREAT meant
     * an O_TMPFILE open was forwarded with mode 0, and the file that later got
     * linked into the filesystem had permissions 0000 instead of the ones the
     * caller asked for. O_TMPFILE contains O_DIRECTORY in its encoding, so it
     * has to be tested as a whole value, not as a single bit. */
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    if (!o_open) o_open = (open_fn) sym("open");
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_open(path, flags, m);
    }
    int fd = o_open(path, flags, m);
    maybe_register(admission.entered, fd, path);
    return fd;
}

int openat(int dfd, const char *path, int flags, ...)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    mode_t m = 0;
    /* O_TMPFILE takes the mode argument too. Reading it only for O_CREAT meant
     * an O_TMPFILE open was forwarded with mode 0, and the file that later got
     * linked into the filesystem had permissions 0000 instead of the ones the
     * caller asked for. O_TMPFILE contains O_DIRECTORY in its encoding, so it
     * has to be tested as a whole value, not as a single bit. */
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    if (!o_openat) o_openat = (openat_fn) sym("openat");
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_openat(dfd, path, flags, m);
    }
    int fd = o_openat(dfd, path, flags, m);
    maybe_register(admission.entered, fd, path);
    return fd;
}

/* glibc fortify emits these fixed-arity entry points for an optimized
 * two-argument open whose flags are not known at compile time.  They must not
 * be routed through our variadic wrappers: O_CREAT/O_TMPFILE deliberately
 * reach libc's *_2 implementation so its missing-mode fail-fast semantics are
 * preserved, without ever reading a nonexistent vararg. */
int __open_2(const char *path, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int fd;
    if (!o___open_2)
        o___open_2 = (open2_fn)sym("__open_2");
    if (!o___open_2) {
        errno = ENOSYS;
        return -1;
    }
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o___open_2(path, flags);
    }
    fd = o___open_2(path, flags);
    maybe_register(admission.entered, fd, path);
    return fd;
}

int __open64_2(const char *path, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int fd;
    if (!o___open64_2)
        o___open64_2 = (open2_fn)sym("__open64_2");
    if (!o___open64_2) {
        errno = ENOSYS;
        return -1;
    }
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o___open64_2(path, flags);
    }
    fd = o___open64_2(path, flags);
    maybe_register(admission.entered, fd, path);
    return fd;
}

int __openat_2(int dfd, const char *path, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int fd;
    if (!o___openat_2)
        o___openat_2 = (openat2_fn)sym("__openat_2");
    if (!o___openat_2) {
        errno = ENOSYS;
        return -1;
    }
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o___openat_2(dfd, path, flags);
    }
    fd = o___openat_2(dfd, path, flags);
    maybe_register(admission.entered, fd, path);
    return fd;
}

int __openat64_2(int dfd, const char *path, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int fd;
    if (!o___openat64_2)
        o___openat64_2 = (openat2_fn)sym("__openat64_2");
    if (!o___openat64_2) {
        errno = ENOSYS;
        return -1;
    }
    if (admission.entered && g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o___openat64_2(dfd, path, flags);
    }
    fd = o___openat64_2(dfd, path, flags);
    maybe_register(admission.entered, fd, path);
    return fd;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    exitos_decision d = EXITOS_PASS;
    ssize_t res = 0, rc;
    off_t off;

    if (!o_write) o_write = (write_fn) sym("write");
    if (!admission.entered) return o_write(fd, buf, n);
    if (g_in_hook) {
        int internal = reentrant_internal_begin(fd);
        ssize_t rr = o_write(fd, buf, n);
        reentrant_internal_end(internal);
        return rr;
    }
    if (!g_fastpath)
        return o_write(fd, buf, n);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_write(fd, buf, n);
        g_in_hook = 0;
        return rc;
    }
    off = lseek(fd, 0, SEEK_CUR);
    if (off >= 0 && exitos_fd_txn_registered(&tx))
        d = exitos_txn_on_write(&tx, buf, n, off, &res);
    else if (exitos_fd_txn_registered(&tx))
        exitos_txn_note_kernel_write(&tx);
    if (d == EXITOS_TAKEOVER) {
        (void)lseek(fd, res, SEEK_CUR);
        rc = res;
    } else {
        /* The PASS decision already marked kernel debt while the same-fd lock
         * is held. Keep it through the syscall so a concurrent sync cannot
         * clear that debt just before these bytes reach the kernel. */
        rc = o_write(fd, buf, n);
    }
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    exitos_decision d = EXITOS_PASS;
    ssize_t res = 0, rc;

    if (!o_pwrite) o_pwrite = (pwrite_fn) sym("pwrite");
    if (!admission.entered) return o_pwrite(fd, buf, n, off);
    if (g_in_hook) {
        int internal = reentrant_internal_begin(fd);
        ssize_t rr = o_pwrite(fd, buf, n, off);
        reentrant_internal_end(internal);
        return rr;
    }
    if (!g_fastpath)
        return o_pwrite(fd, buf, n, off);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_pwrite(fd, buf, n, off);
        g_in_hook = 0;
        return rc;
    }
    if (exitos_fd_txn_registered(&tx))
        d = exitos_txn_on_write(&tx, buf, n, off, &res);
    rc = (d == EXITOS_TAKEOVER) ? res : o_pwrite(fd, buf, n, off);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

int fdatasync(int fd)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    exitos_decision d = EXITOS_PASS;
    int res = 0, rc;

    if (!o_fdatasync) o_fdatasync = (fdatasync_fn) sym("fdatasync");
    if (!admission.entered) return o_fdatasync(fd);
    if (g_in_hook) {
        if (g_ctl_depth)
            return o_fdatasync(fd);
        int internal = reentrant_internal_try_begin(fd);
        if (internal) {
            int rr = o_fdatasync(fd);
            reentrant_internal_end(internal);
            return rr;
        }
        /* A signal-handler sync cannot establish durability for raw writes
         * whose owning transaction it interrupted.  Returning the real
         * fdatasync result here used to report success while raw debt remained.
         * Fail this call closed; the next ordinary sync will pay that debt. */
        exitos_ctx_poison(g_ctx);
        errno = EIO;
        return -1;
    }
    if (!g_fastpath)
        return o_fdatasync(fd);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_fdatasync(fd);
        g_in_hook = 0;
        return rc;
    }
    if (exitos_fd_txn_registered(&tx))
        d = exitos_txn_on_fdatasync(&tx, &res);
    if (d == EXITOS_TAKEOVER) {
        exitos_fd_txn_end(&tx);
        g_in_hook = 0;
        /* Core and raw-syscall code use -errno. libc uses -1 plus errno. */
        if (res < 0) {
            errno = -res;
            return -1;
        }
        return res;
    }

    /* The library cannot see whether the kernel's fdatasync worked, and that is
     * the fact it needs: a successful one persists everything the kernel was
     * holding for this file, which is the only reason the shortcut was being
     * declined. Without telling it, one early write that fell back disabled
     * the shortcut for the rest of the descriptor's life, so no later
     * fdatasync on that descriptor was ever taken over in a write-ahead-log
     * workload. On failure the data is still owed, so nothing is cleared. */
    rc = o_fdatasync(fd);
    if (rc == 0)
        exitos_txn_note_kernel_sync(&tx);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

int fsync(int fd)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    int raw_rc, raw_errno = 0, rc, saved_errno;

    if (!o_fsync) o_fsync = (fsync_fn) sym("fsync");
    if (!admission.entered)
        return o_fsync(fd);
    if (g_in_hook) {
        if (g_ctl_depth)
            return o_fsync(fd);
        /* iopath has no internal fsync operation, so every recursive fsync is
         * asynchronous application reentry and cannot safely settle raw debt. */
        exitos_ctx_poison(g_ctx);
        errno = EIO;
        return -1;
    }
    if (!g_fastpath)
        return o_fsync(fd);

    /* fsync cannot be taken over because only the filesystem can persist inode
     * metadata. It still has to pay raw data debt first. Always run the real
     * fsync even when the raw FLUSH fails: metadata durability is an independent
     * obligation. The call as a whole reports the first (raw) failure. */
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_fsync(fd);
        g_in_hook = 0;
        return rc;
    }
    raw_rc = exitos_txn_flush_raw_debt(&tx);
    if (raw_rc < 0)
        raw_errno = -raw_rc;

    rc = o_fsync(fd);
    saved_errno = errno;
    if (rc == 0)
        exitos_txn_note_kernel_sync(&tx);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    if (raw_rc < 0) {
        errno = raw_errno;
        return -1;
    }
    if (rc != 0)
        errno = saved_errno;
    return rc;
}

struct truncate_call {
    ftruncate_fn narrow;
    ftruncate64_fn wide;
    int use_wide;
};

static int truncate_real(const struct truncate_call *call, int fd, off64_t len)
{
    if (call->use_wide)
        return call->wide(fd, len);
    return call->narrow(fd, (off_t)len);
}

static int ftruncate_common(const struct truncate_call *call, int fd,
                            off64_t len)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    int res = 0, rc, raw_rc;

    if ((!call->use_wide && !call->narrow) || (call->use_wide && !call->wide)) {
        errno = ENOSYS;
        return -1;
    }
    if (!admission.entered)
        return truncate_real(call, fd, len);
    if (g_in_hook) {
        if (g_ctl_depth)
            return truncate_real(call, fd, len);
        exitos_ctx_poison(g_ctx);
        errno = EIO;
        return -1;
    }
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = truncate_real(call, fd, len);
        g_in_hook = 0;
        return rc;
    }
    /* Old raw writes must be persistent before the filesystem can release and
     * reallocate their blocks.  On failure retain both mapping and debt and do
     * not enter libc, so the exact operation remains retryable. */
    raw_rc = exitos_txn_flush_raw_debt(&tx);
    if (raw_rc < 0) {
        exitos_fd_txn_end(&tx);
        g_in_hook = 0;
        errno = -raw_rc;
        return -1;
    }
    /* Must run before the real syscall: it invalidates cached mappings so a later write cannot
     * reuse an LBA that no longer belongs to this file. */
    if (exitos_fd_txn_registered(&tx)) {
        if ((off64_t)(off_t)len == len)
            (void)exitos_txn_on_ftruncate(&tx, (off_t)len, &res);
        else
            exitos_txn_invalidate_mapping(&tx);
    }
    rc = truncate_real(call, fd, len);
    if (rc == 0)
        exitos_txn_note_kernel_metadata(&tx); /* size/metadata needs real sync */
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}


int ftruncate(int fd, off_t len)
{
    struct truncate_call call;
    if (!o_ftruncate)
        o_ftruncate = (ftruncate_fn)sym("ftruncate");
    memset(&call, 0, sizeof(call));
    call.narrow = o_ftruncate;
    return ftruncate_common(&call, fd, (off64_t)len);
}

int ftruncate64(int fd, off64_t len)
{
    struct truncate_call call;
    if (!o_ftruncate64)
        o_ftruncate64 = (ftruncate64_fn)sym("ftruncate64");
    memset(&call, 0, sizeof(call));
    call.wide = o_ftruncate64;
    call.use_wide = 1;
    return ftruncate_common(&call, fd, len);
}

struct preload_native_fallocate {
    fallocate_fn fn;
};

static int preload_native_fallocate_call(void *opaque, int fd, int mode,
                                         off_t off, off_t len)
{
    struct preload_native_fallocate *call = opaque;
    int rc = call->fn(fd, mode, off, len);

    if (rc != 0)
        return -(errno ? errno : EIO);
    return 0;
}

static void preload_prepare_maybe_arm(void)
{
    unsigned arrived = 0, expected = 0;

    if (!exitos_frontend_config_prepare_arrive(g_config, &arrived, &expected))
        return;
    fprintf(stderr,
            "PREPARE_STOP_ARMED v=1 frontend=ld_preload arrived=%u "
            "expected=%u stop_after=selected-close\n", arrived, expected);
}

static int preload_prepare_maybe_stop(void)
{
    unsigned expected;

    if (!exitos_frontend_config_prepare_stop_take(g_config))
        return 0;
    expected = exitos_frontend_config_prepare_expected(g_config);
    fprintf(stderr,
            "PREPARE_STOP_READY v=1 frontend=ld_preload arrived=%u "
            "expected=%u signal=SIGSTOP after=selected-close\n",
            expected, expected);
    if (raise(SIGSTOP) != 0)
        return -(errno ? errno : EIO);
    fprintf(stderr,
            "PREPARE_STOP_RELEASED v=1 frontend=ld_preload arrived=%u "
            "expected=%u signal=SIGCONT\n", expected, expected);
    return 0;
}

static int fallocate_common(fallocate_fn real_fn, int fd, int mode,
                            off_t off, off_t len)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_frontend_fallocate_result result;
    struct preload_native_fallocate native = { .fn = real_fn };
    int rc;

    if (!real_fn) {
        errno = ENOSYS;
        return -1;
    }
    if (!admission.entered)
        return real_fn(fd, mode, off, len);
    if (g_in_hook) {
        if (g_ctl_depth)
            return real_fn(fd, mode, off, len);
        /* A signal-time extent mutation cannot safely wait for or flush the
         * interrupted raw transaction.  Never release blocks underneath it. */
        exitos_ctx_poison(g_ctx);
        errno = EIO;
        return -1;
    }
    g_in_hook = 1;
    g_ctl_depth++;
    rc = exitos_frontend_fallocate_execute(
        g_fallocate_runtime, fd, mode, off, len,
        preload_native_fallocate_call, &native, &result);
    fprintf(stderr,
            "PREPARE_OUTCOME v=1 frontend=ld_preload fd=%d mode=%d "
            "off=%lld len=%lld outcome=%s stage=%s rc=%d return_rc=%d "
            "return_errno=%d rdwr_alias=%u prepared=%llu chunks=%u\n",
            fd, mode, (long long)off, (long long)len,
            exitos_frontend_fallocate_outcome_name(result.outcome),
            exitos_frontend_fallocate_stage_name(result.stage), result.rc,
            rc < 0 ? -1 : 0, rc < 0 ? -rc : 0,
            result.used_rdwr_alias ? 1U : 0U,
            (unsigned long long)result.prepared_bytes, result.chunks);
    if (rc == 0 && result.prepared) {
        fprintf(stderr,
                "PREPARE_READY frontend=ld_preload fd=%d mode=%d off=%lld "
                "len=%lld prepared=%llu chunks=%u fiemap=safe "
                "unsafe_flags=0\n",
                fd, mode, (long long)off, (long long)len,
                (unsigned long long)result.prepared_bytes, result.chunks);
        preload_prepare_maybe_arm();
    }
    g_ctl_depth--;
    g_in_hook = 0;
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return 0;
}

int fallocate(int fd, int mode, off_t off, off_t len)
{
    if (!o_fallocate)
        o_fallocate = (fallocate_fn)sym("fallocate");
    return fallocate_common(o_fallocate, fd, mode, off, len);
}

int fallocate64(int fd, int mode, off64_t off, off64_t len)
{
    if (!o_fallocate64)
        o_fallocate64 = (fallocate_fn)sym("fallocate64");
    return fallocate_common(o_fallocate64, fd, mode, (off_t)off, (off_t)len);
}

int close(int fd)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    int rc, prep, saved_errno, selected;

    if (!o_close) o_close = (close_fn) sym("close");
    if (!admission.entered)
        return o_close(fd);
    if (g_in_hook) {
        int internal = reentrant_internal_begin(fd);
        int rr = o_close(fd);
        reentrant_internal_end(internal);
        return rr;
    }
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_close(fd);
        g_in_hook = 0;
        return rc;
    }
    selected = exitos_fd_txn_registered(&tx);
    prep = exitos_fd_txn_prepare_forget(&tx);
    if (prep < 0) {
        exitos_fd_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    rc = o_close(fd);
    saved_errno = errno;
    /* Linux releases the descriptor early in close(). Except for EBADF, an
     * EINTR/EIO/ENOSPC/EDQUOT return still means fd is closed and may already
     * be reused; EBADF proves a retained registration was stale. In every case
     * it is unsafe to restore the old fd-number mapping or retry close. */
    (void)exitos_fd_txn_forget(&tx);
    exitos_fd_txn_end(&tx);
    if (selected && preload_prepare_maybe_stop() < 0) {
        g_in_hook = 0;
        errno = EIO;
        return -1;
    }
    g_in_hook = 0;
    if (rc != 0)
        errno = saved_errno;
    return rc;
}

int close_range(unsigned first, unsigned last, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int prep, rc;

    if (!o_close_range)
        o_close_range = (close_range_fn)sym("close_range");
    if (!admission.entered)
        return o_close_range(first, last, flags);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_close_range(first, last, flags);
    }

    g_in_hook = 1;
    prep = exitos_ctx_flush_all_and_poison(g_ctx);
    if (prep < 0) {
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    /* This deliberately stays poisoned even if libc rejects the range.  Once
     * a bulk fd operation is attempted there is no safe per-number rollback,
     * and a permanent ordinary-kernel path is preferable to stale raw LBAs. */
    rc = o_close_range(first, last, flags);
    g_in_hook = 0;
    return rc;
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int prep;
    FILE *ret;

    if (!o_freopen)
        o_freopen = (freopen_fn)sym("freopen");
    if (!admission.entered || !stream)
        return o_freopen(path, mode, stream);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_freopen(path, mode, stream);
    }

    /* libc may close and replace the stream's fd without routing that internal
     * close through our symbol.  Per-fd rollback cannot make that atomic, so
     * settle every raw domain and permanently use the kernel path afterward. */
    g_in_hook = 1;
    prep = exitos_ctx_flush_all_and_poison(g_ctx);
    if (prep < 0) {
        g_in_hook = 0;
        errno = -prep;
        return NULL;
    }
    ret = o_freopen(path, mode, stream);
    g_in_hook = 0;
    return ret;
}

FILE *freopen64(const char *path, const char *mode, FILE *stream)
{
    return freopen(path, mode, stream);
}

/* exec can retain application fds while discarding this process-local debt
 * ledger. Settle every raw durability domain before the image boundary and
 * permanently disable takeover even if the real exec later fails: rollback
 * cannot prove that loader/audit machinery left every fd identity unchanged. */
static int prepare_exec_boundary(void)
{
    int rc;

    if (g_in_hook) {
        /* A signal handler cannot wait on the interrupted transaction whose
         * debt it needs to settle. Do not replace the image on a false proof. */
        exitos_ctx_poison(g_ctx);
        errno = EIO;
        return -1;
    }
    g_in_hook = 1;
    rc = exitos_ctx_flush_all_and_poison(g_ctx);
    if (rc < 0) {
        g_in_hook = 0;
        errno = -rc;
        return -1;
    }
    return 0;
}

static int finish_failed_exec(int rc)
{
    int saved_errno = errno;
    g_in_hook = 0;
    errno = saved_errno;
    return rc;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int rc;

    if (!o_execve)
        o_execve = (execve_fn)sym("execve");
    if (!o_execve) {
        errno = ENOSYS;
        return -1;
    }
    if (!admission.entered)
        return o_execve(path, argv, envp);
    if (prepare_exec_boundary() != 0)
        return -1;
    rc = o_execve(path, argv, envp);
    return finish_failed_exec(rc);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int rc;

    if (!o_execveat)
        o_execveat = (execveat_fn)sym("execveat");
    if (!o_execveat) {
        errno = ENOSYS;
        return -1;
    }
    if (!admission.entered)
        return o_execveat(dirfd, path, argv, envp, flags);
    if (prepare_exec_boundary() != 0)
        return -1;
    rc = o_execveat(dirfd, path, argv, envp, flags);
    return finish_failed_exec(rc);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    PRELOAD_ADMISSION_SCOPE(admission);
    int rc;

    if (!o_fexecve)
        o_fexecve = (fexecve_fn)sym("fexecve");
    if (!o_fexecve) {
        errno = ENOSYS;
        return -1;
    }
    if (!admission.entered)
        return o_fexecve(fd, argv, envp);
    if (prepare_exec_boundary() != 0)
        return -1;
    rc = o_fexecve(fd, argv, envp);
    return finish_failed_exec(rc);
}

/* close() is not the only way an fd number stops meaning what it meant when it
 * was registered. dup2/dup3 rebind the number to a different file description
 * outright, and fcntl(F_DUPFD) can land on a number that is free only because
 * its file was closed. The registration table is keyed by the number, so a
 * write after a rebind would be serviced against the previous file's map and
 * land on that file's blocks -- raw, on the device, past the filesystem. Each
 * of these therefore drops the registration on the number being overwritten,
 * which costs nothing on the write path. */
typedef int (*dup2_fn)(int, int);
typedef int (*dup3_fn)(int, int, int);
static dup2_fn o_dup2;
static dup3_fn o_dup3;

int dup(int oldfd)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_alias_txn tx;
    int rc, prep, saved_errno;

    if (!o_dup) o_dup = (dup_fn) sym("dup");
    if (!admission.entered)
        return o_dup(oldfd);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_dup(oldfd);
    }
    g_in_hook = 1;
    if (exitos_alias_txn_begin(g_ctx, oldfd, -1, &tx) != 0) {
        rc = o_dup(oldfd);
        g_in_hook = 0;
        return rc;
    }
    prep = exitos_alias_txn_prepare(&tx);
    if (prep < 0) {
        exitos_alias_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    rc = o_dup(oldfd);
    saved_errno = errno;
    if (rc >= 0)
        exitos_alias_txn_commit(&tx);
    else
        exitos_alias_txn_abort(&tx);
    exitos_alias_txn_end(&tx);
    g_in_hook = 0;
    if (rc < 0)
        errno = saved_errno;
    return rc;
}

int dup2(int oldfd, int newfd)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_alias_txn tx;
    int rc, prep, saved_errno;

    if (!o_dup2) o_dup2 = (dup2_fn) sym("dup2");
    if (!admission.entered)
        return o_dup2(oldfd, newfd);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_dup2(oldfd, newfd);
    }
    g_in_hook = 1;
    if (oldfd == newfd) {
        rc = o_dup2(oldfd, newfd);
        g_in_hook = 0;
        return rc;
    }
    if (exitos_alias_txn_begin(g_ctx, oldfd, newfd, &tx) != 0) {
        rc = o_dup2(oldfd, newfd);
        g_in_hook = 0;
        return rc;
    }
    prep = exitos_alias_txn_prepare(&tx);
    if (prep < 0) {
        exitos_alias_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    rc = o_dup2(oldfd, newfd);
    saved_errno = errno;
    if (rc >= 0)
        exitos_alias_txn_commit(&tx);
    else
        exitos_alias_txn_abort(&tx);
    exitos_alias_txn_end(&tx);
    g_in_hook = 0;
    if (rc < 0)
        errno = saved_errno;
    return rc;
}

int dup3(int oldfd, int newfd, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_alias_txn tx;
    int rc, prep, saved_errno;

    if (!o_dup3) o_dup3 = (dup3_fn) sym("dup3");
    if (!admission.entered)
        return o_dup3(oldfd, newfd, flags);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_dup3(oldfd, newfd, flags);
    }
    g_in_hook = 1;
    if (oldfd == newfd) {
        rc = o_dup3(oldfd, newfd, flags);
        g_in_hook = 0;
        return rc;
    }
    if (exitos_alias_txn_begin(g_ctx, oldfd, newfd, &tx) != 0) {
        rc = o_dup3(oldfd, newfd, flags);
        g_in_hook = 0;
        return rc;
    }
    prep = exitos_alias_txn_prepare(&tx);
    if (prep < 0) {
        exitos_alias_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    rc = o_dup3(oldfd, newfd, flags);
    saved_errno = errno;
    if (rc >= 0)
        exitos_alias_txn_commit(&tx);
    else
        exitos_alias_txn_abort(&tx);
    exitos_alias_txn_end(&tx);
    g_in_hook = 0;
    if (rc < 0)
        errno = saved_errno;
    return rc;
}

static int fcntl_has_no_arg(int cmd)
{
    switch (cmd) {
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
#ifdef F_GETSIG
    case F_GETSIG:
#endif
#ifdef F_GETLEASE
    case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
    case F_GET_SEALS:
#endif
        return 1;
    default:
        return 0;
    }
}

static int do_fcntl_control(fcntl_fn orig, int fd, int cmd, unsigned long arg)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_alias_txn tx;
    struct exitos_fd_txn ftx;
    int rc, prep, saved_errno;

    if (cmd == F_SETFL) {
        if (!admission.entered)
            return orig(fd, cmd, arg);
        if (g_in_hook) {
            if (!g_ctl_depth)
                exitos_ctx_poison(g_ctx);
            return orig(fd, cmd, arg);
        }
        g_in_hook = 1;
        if (exitos_fd_txn_begin(g_ctx, fd, &ftx) != 0) {
            rc = orig(fd, cmd, arg);
            g_in_hook = 0;
            return rc;
        }
        prep = exitos_fd_txn_prepare_forget(&ftx);
        if (prep < 0) {
            exitos_fd_txn_end(&ftx);
            g_in_hook = 0;
            errno = -prep;
            return -1;
        }
        rc = orig(fd, cmd, arg);
        saved_errno = errno;
        if (rc == 0)
            (void)exitos_fd_txn_forget(&ftx);
        else
            exitos_fd_txn_abort_forget(&ftx);
        exitos_fd_txn_end(&ftx);
        g_in_hook = 0;
        if (rc != 0)
            errno = saved_errno;
        return rc;
    }

    if (cmd != F_DUPFD
#ifdef F_DUPFD_CLOEXEC
        && cmd != F_DUPFD_CLOEXEC
#endif
        )
        return orig(fd, cmd, arg);
    if (!admission.entered)
        return orig(fd, cmd, arg);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return orig(fd, cmd, arg);
    }

    g_in_hook = 1;
    if (exitos_alias_txn_begin(g_ctx, fd, -1, &tx) != 0) {
        rc = orig(fd, cmd, arg);
        g_in_hook = 0;
        return rc;
    }
    prep = exitos_alias_txn_prepare(&tx);
    if (prep < 0) {
        exitos_alias_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return -1;
    }
    rc = orig(fd, cmd, arg);
    saved_errno = errno;
    if (rc >= 0)
        exitos_alias_txn_commit(&tx);
    else
        exitos_alias_txn_abort(&tx);
    exitos_alias_txn_end(&tx);
    g_in_hook = 0;
    if (rc < 0)
        errno = saved_errno;
    return rc;
}

int fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;

    if (!o_fcntl) o_fcntl = (fcntl_fn) sym("fcntl");
    if (!fcntl_has_no_arg(cmd)) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
        return do_fcntl_control(o_fcntl, fd, cmd, arg);
    }
    return o_fcntl(fd, cmd);
}

int fcntl64(int fd, int cmd, ...)
{
    unsigned long arg = 0;

    if (!o_fcntl64) o_fcntl64 = (fcntl_fn) sym("fcntl64");
    if (!o_fcntl64) {
        if (!o_fcntl) o_fcntl = (fcntl_fn) sym("fcntl");
        o_fcntl64 = o_fcntl;
    }
    if (!fcntl_has_no_arg(cmd)) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
        return do_fcntl_control(o_fcntl64, fd, cmd, arg);
    }
    return o_fcntl64(fd, cmd);
}

/* fclose() closes the underlying fd from inside libc, and that internal call
 * does not go through symbol interposition -- our close() wrapper never sees
 * it. The registration therefore outlived the fd, and the next file handed the
 * same fd number was written through the previous file's map: 17 takeovers
 * where only the 1 write made before the fclose was legitimate. */
typedef int (*fclose_fn)(FILE *);
static fclose_fn o_fclose;

int fclose(FILE *f)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    int fd, rc, prep, saved_errno;

    if (!o_fclose) o_fclose = (fclose_fn) sym("fclose");
    if (!admission.entered || !f)
        return o_fclose(f);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_fclose(f);
    }
    fd = fileno(f);
    if (fd < 0)
        return o_fclose(f);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_fclose(f);
        g_in_hook = 0;
        return rc;
    }
    prep = exitos_fd_txn_prepare_forget(&tx);
    if (prep < 0) {
        exitos_fd_txn_end(&tx);
        g_in_hook = 0;
        errno = -prep;
        return EOF;
    }
    rc = o_fclose(f);
    saved_errno = errno;
    /* fclose may report a buffered-write/close error after consuming the
     * stream and closing its fd. Never retain a mapping keyed by that number. */
    (void)exitos_fd_txn_forget(&tx);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    if (rc != 0)
        errno = saved_errno;
    return rc;
}

/* ---- entry points that must not silently bypass the module ---------------
 * Two families. The large-file aliases (open64, openat64, pwrite64) are what a
 * program built for the 64-bit file ABI actually calls; without them such a
 * program was never intercepted at all. The scatter/gather calls (writev,
 * pwritev) cannot be serviced by the shortcut as they stand, but they must
 * still be declared, or the kernel ends up holding data this layer does not
 * know about and our fdatasync answers for durability nobody established. */
typedef ssize_t (*writev_fn)(int, const struct iovec *, int);
typedef ssize_t (*pwritev_fn)(int, const struct iovec *, int, off_t);
typedef ssize_t (*pwritev2_fn)(int, const struct iovec *, int, off_t, int);
static writev_fn  o_writev;
static pwritev_fn o_pwritev;
static pwritev2_fn o_pwritev2;

int open64(const char *path, int flags, ...)
{
    mode_t m = 0;
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return open(path, flags, m);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t m = 0;
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return openat(dirfd, path, flags, m);
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off)
{
    return pwrite(fd, buf, n, off);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    ssize_t rc;

    if (!o_writev) o_writev = (writev_fn) sym("writev");
    if (!admission.entered)
        return o_writev(fd, iov, cnt);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_writev(fd, iov, cnt);
    }
    if (!g_fastpath)
        return o_writev(fd, iov, cnt);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_writev(fd, iov, cnt);
        g_in_hook = 0;
        return rc;
    }
    exitos_txn_note_kernel_write(&tx);
    rc = o_writev(fd, iov, cnt);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    ssize_t rc;

    if (!o_pwritev) o_pwritev = (pwritev_fn) sym("pwritev");
    if (!admission.entered)
        return o_pwritev(fd, iov, cnt, off);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_pwritev(fd, iov, cnt, off);
    }
    if (!g_fastpath)
        return o_pwritev(fd, iov, cnt, off);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_pwritev(fd, iov, cnt, off);
        g_in_hook = 0;
        return rc;
    }
    exitos_txn_note_kernel_write(&tx);
    rc = o_pwritev(fd, iov, cnt, off);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

ssize_t pwritev2(int fd, const struct iovec *iov, int cnt, off_t off, int flags)
{
    PRELOAD_ADMISSION_SCOPE(admission);
    struct exitos_fd_txn tx;
    ssize_t rc;

    if (!o_pwritev2) o_pwritev2 = (pwritev2_fn) sym("pwritev2");
    if (!admission.entered)
        return o_pwritev2(fd, iov, cnt, off, flags);
    if (g_in_hook) {
        if (!g_ctl_depth)
            exitos_ctx_poison(g_ctx);
        return o_pwritev2(fd, iov, cnt, off, flags);
    }
    if (!g_fastpath)
        return o_pwritev2(fd, iov, cnt, off, flags);
    g_in_hook = 1;
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0) {
        rc = o_pwritev2(fd, iov, cnt, off, flags);
        g_in_hook = 0;
        return rc;
    }
    /* PASS-only, including RWF_DSYNC/RWF_SYNC: the kernel owns both the
     * scatter/gather transfer and per-call durability.  Record that obligation
     * before entering libc while the same-fd transaction is still pinned. */
    if (exitos_fd_txn_registered(&tx))
        exitos_txn_note_kernel_write(&tx);
    rc = o_pwritev2(fd, iov, cnt, off, flags);
    exitos_fd_txn_end(&tx);
    g_in_hook = 0;
    return rc;
}

ssize_t pwritev64(int fd, const struct iovec *iov, int cnt, off64_t off)
{
    return pwritev(fd, iov, cnt, (off_t)off);
}

ssize_t pwritev64v2(int fd, const struct iovec *iov, int cnt, off64_t off,
                    int flags)
{
    return pwritev2(fd, iov, cnt, (off_t)off, flags);
}
