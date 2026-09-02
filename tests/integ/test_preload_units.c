/* test_preload_units.c -- adversarial unit-level coverage of src/preload.c,
 * the LD_PRELOAD interposer.  Until now the only test of this file was the
 * end-to-end shell script tests/integ/test_preload.sh, which checks one happy
 * path (md5 of a file written through the fast path) and nothing else.
 *
 * preload.c cannot be linked into a normal unit test: its whole job is to
 * shadow libc symbols in a process that was started with LD_PRELOAD.  So this
 * test is a driver.  It builds a loop-backed ext4 fixture, then re-executes
 * ITSELF as a child ("<self> child <scenario> ...") with LD_PRELOAD and the
 * EXITOS_* environment set per scenario, and asserts, from the parent, on
 *   (a) the bytes and the st_mode that ended up on disk, and
 *   (b) the counters the library dumps through EXITOS_STATS.
 * (b) matters because, as include/exitos_stats.h says, "A correct checksum
 * alone cannot distinguish 'the fast path ran and was right' from 'the fast
 * path never ran at all'".
 *
 * SAFETY: the only block device touched is a loop device over an image file in
 * /tmp whose name contains getpid().  Both are torn down by an atexit handler
 * and by SIGINT/SIGTERM handlers, so the failure paths clean up too.  No
 * physical device is ever opened.
 *
 * HOW TO BUILD -- THIS MATTERS.  libexitos.a contains preload.o, and preload.o
 * defines open/write/pwrite/close/fsync/fdatasync/ftruncate.  A test that calls
 * open() and lists libexitos.a on the link line therefore has those archive
 * members pulled in ahead of libc, and the TEST BINARY ITSELF becomes an
 * interposer with its own g_ctx and its own copy of the counters.  Two
 * constructors then run, EXITOS_STATS is written twice (the second dump wins),
 * and every fast-path assertion silently reads the wrong copy.  Build with -lc
 * ahead of the archive so open() binds to libc:
 *
 *   gcc -O2 -Wall -Wextra -D_GNU_SOURCE -Iinclude -Itests/harness \
 *       tests/integ/test_preload_units.c -lc libexitos.a -o /tmp/t_preload_units -lpthread
 *
 * NOTE ON UNWRITTEN EXTENTS.  src/intercept.c refuses to map extents that
 * fallocate() left UNWRITTEN, so a freshly preallocated file takes the kernel
 * path until its extents are converted.  Every scenario that wants to exercise
 * the fast path therefore pre-creates and fully writes its file from the
 * PARENT (which is not preloaded) first, so that the child's open() snapshots a
 * fully mapped file and `passed` stays 0.  Getting this wrong would make the
 * fast-path assertions vacuous.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

#define BS        4096u
#define MAXBLK    64

/* ------------------------------------------------------------------ *
 * shared between parent and child
 * ------------------------------------------------------------------ */

static void fill_block(void *b, const char *tag, int i)
{
    memset(b, 0, BS);
    snprintf((char *)b, BS, "EXITOS-%s-BLOCK-%06d-payload", tag, i);
}

static void *abuf(void)
{
    void *p = NULL;
    if (posix_memalign(&p, BS, BS) != 0)
        return NULL;
    return p;
}

/* ------------------------------------------------------------------ *
 * child side: scenarios executed under LD_PRELOAD
 * ------------------------------------------------------------------ */

static FILE *g_out;
static void kv(const char *k, long long v) { if (g_out) fprintf(g_out, "%s=%lld\n", k, v); }
static void kvs(const char *k, const char *v) { if (g_out) fprintf(g_out, "%s=%s\n", k, v); }

/* Write nblk blocks of `tag` into an already-existing, already-converted file.
 * direct: add O_DIRECT.  sync: 0 none, 1 fdatasync, 2 fsync. */
static int c_write(const char *path, int nblk, const char *tag, int direct, int sync,
                   int create)
{
    int flags = O_RDWR | (direct ? O_DIRECT : 0) | (create ? O_CREAT : 0);
    int fd = open(path, flags, 0644);
    void *b;
    int i;

    if (fd < 0) { kv("open_errno", errno); return 11; }
    kv("fd", fd);
    if (create && fallocate(fd, 0, 0, (off_t)BS * nblk) != 0) { kv("fallocate_errno", errno); return 12; }
    b = abuf();
    if (!b) return 13;
    for (i = 0; i < nblk; i++) {
        fill_block(b, tag, i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) { kv("pwrite_errno", errno); return 14; }
    }
    if (sync == 1) kv("sync_rc", fdatasync(fd));
    if (sync == 2) kv("sync_rc", fsync(fd));
    free(b);
    return close(fd) == 0 ? 0 : 15;
}

/* Every open() flavour that carries a mode argument, plus the two that do not.
 * The parent compares these against a baseline run with no LD_PRELOAD at all. */
static int c_modes(const char *dir)
{
    char p[PATH_MAX];
    struct stat st;
    int fd, dfd;

    umask(0);                          /* so the requested mode is the final mode */

    snprintf(p, sizeof p, "%s/m_open", dir);
    fd = open(p, O_RDWR | O_CREAT | O_EXCL, 0642);
    if (fd < 0) return 21;
    if (write(fd, "hello-open", 10) != 10) return 22;
    close(fd);

    snprintf(p, sizeof p, "%s/m_openat", dir);
    fd = openat(AT_FDCWD, p, O_RDWR | O_CREAT | O_EXCL, 0611);
    if (fd < 0) return 23;
    if (write(fd, "hello-openat", 12) != 12) return 24;
    close(fd);

    dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return 25;
    fd = openat(dfd, "m_openat_rel", O_RDWR | O_CREAT | O_EXCL, 0604);
    if (fd < 0) return 26;
    close(fd);
    close(dfd);

    snprintf(p, sizeof p, "%s/m_zero", dir);
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0000);
    if (fd < 0) return 27;
    close(fd);

    snprintf(p, sizeof p, "%s/m_all", dir);
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0777);
    if (fd < 0) return 28;
    close(fd);

    snprintf(p, sizeof p, "%s/m_setgid", dir);
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 02640);
    if (fd < 0) return 29;
    close(fd);

    /* No O_CREAT: no mode argument is passed AT ALL by the caller.  An
     * interposer that reads a vararg here reads garbage off the stack. */
    snprintf(p, sizeof p, "%s/m_open", dir);
    fd = open(p, O_RDWR);
    if (fd < 0) return 30;
    if (pwrite(fd, "Z", 1, 0) != 1) return 31;
    close(fd);
    if (stat(p, &st) == 0) kv("nocreat_mode", (long long)(st.st_mode & 07777));

    /* O_TMPFILE also needs a mode, and does NOT contain O_CREAT.  glibc's own
     * open() knows this: __OPEN_NEEDS_MODE(oflag) is
     *   ((oflag) & O_CREAT) != 0 || ((oflag) & __O_TMPFILE) == __O_TMPFILE   */
    fd = open(dir, O_TMPFILE | O_RDWR, 0642);
    if (fd < 0) {
        kv("tmpfile_errno", errno);
    } else {
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
        snprintf(p, sizeof p, "%s/m_tmpfile", dir);
        if (linkat(AT_FDCWD, proc, AT_FDCWD, p, AT_SYMLINK_FOLLOW) != 0)
            kv("tmpfile_link_errno", errno);
        close(fd);
    }
    return 0;
}

/* fd-number recycling.  `how` selects the route by which fd_b stops referring
 * to file B: 0 = close() (interposed), 1 = fclose() (glibc closes the
 * descriptor internally, never through the interposed close()), 2 = dup2()
 * (the number is reused with no close at all). */
static int c_fdreuse(const char *pathB, const char *pathA, int nblk, int how)
{
    void *b = abuf();
    int fdb, fda, i;

    if (!b) return 41;
    fdb = open(pathB, O_RDWR | O_DIRECT);
    if (fdb < 0) { kv("openB_errno", errno); return 42; }
    kv("fdb", fdb);

    /* one write so the registration is definitely live and mapped */
    fill_block(b, "BBB", 0);
    if (pwrite(fdb, b, BS, 0) != (ssize_t)BS) return 43;

    if (how == 0) {
        if (close(fdb) != 0) return 44;
        fda = open(pathA, O_RDWR | O_DIRECT);
        if (fda < 0) { kv("openA_errno", errno); return 45; }
    } else if (how == 1) {
        FILE *f = fdopen(fdb, "r+");
        if (!f) return 46;
        if (fclose(f) != 0) return 47;
        fda = open(pathA, O_RDWR | O_DIRECT);
        if (fda < 0) { kv("openA_errno", errno); return 48; }
    } else {
        int tmp = open(pathA, O_RDWR | O_DIRECT);
        if (tmp < 0) { kv("openA_errno", errno); return 49; }
        if (dup2(tmp, fdb) < 0) return 50;
        close(tmp);
        fda = fdb;
    }
    kv("fda", fda);
    kv("reused", fda == fdb);

    for (i = 0; i < nblk; i++) {
        fill_block(b, "AAA", i);
        if (pwrite(fda, b, BS, (off_t)i * BS) != (ssize_t)BS) { kv("pwriteA_errno", errno); return 51; }
    }
    kv("sync_rc", fdatasync(fda));
    close(fda);
    free(b);
    return 0;
}

/* O_APPEND.  The kernel ignores the file offset for an O_APPEND fd and always
 * lands the bytes at EOF; preload.c's write() computes the target offset from
 * lseek(fd, 0, SEEK_CUR) instead. */
static int c_append(const char *path)
{
    void *b = abuf();
    struct stat st;
    ssize_t n;
    int fd;

    if (!b) return 61;
    fd = open(path, O_RDWR | O_APPEND | O_DIRECT);
    if (fd < 0) { kv("open_errno", errno); return 62; }
    if (fstat(fd, &st) != 0) return 63;
    kv("size_before", (long long)st.st_size);
    fill_block(b, "APPENDED", 0);
    n = write(fd, b, BS);
    kv("write_rc", (long long)n);
    kv("pos_after", (long long)lseek(fd, 0, SEEK_CUR));
    if (fstat(fd, &st) != 0) return 64;
    kv("size_after", (long long)st.st_size);
    kv("sync_rc", fdatasync(fd));
    close(fd);
    free(b);
    return 0;
}

/* ftruncate() must invalidate the cached mapping before the truncation happens,
 * "so a later write cannot reuse an LBA that no longer belongs to this file"
 * (src/preload.c).  A victim file grabs the freed blocks in between. */
static int c_trunc(const char *log, const char *victim, int aligned)
{
    void *b = abuf();
    int fd, vfd, i;
    off_t keep = aligned ? (off_t)8 * BS : (off_t)8 * BS + 1000;
    struct stat st;

    if (!b) return 71;
    fd = open(log, O_RDWR | O_DIRECT);
    if (fd < 0) { kv("open_errno", errno); return 72; }

    for (i = 0; i < 32; i++) {                    /* pass 1: fast path */
        fill_block(b, "T1", i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) return 73;
    }
    if (fdatasync(fd) != 0) return 74;

    kv("trunc_rc", ftruncate(fd, keep));
    if (fstat(fd, &st) != 0) return 75;
    kv("size_after_trunc", (long long)st.st_size);

    /* victim takes the just-released blocks */
    vfd = open(victim, O_RDWR | O_CREAT, 0644);
    if (vfd < 0) return 76;
    if (fallocate(vfd, 0, 0, (off_t)24 * BS) != 0) return 77;
    for (i = 0; i < 24; i++) {
        fill_block(b, "VIC", i);
        if (pwrite(vfd, b, BS, (off_t)i * BS) != (ssize_t)BS) return 78;
    }
    if (fdatasync(vfd) != 0) return 79;
    close(vfd);

    /* log grows back and is rewritten end to end */
    if (fallocate(fd, 0, 0, (off_t)32 * BS) != 0) return 80;
    for (i = 0; i < 32; i++) {
        fill_block(b, "T2", i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) return 81;
    }
    if (fdatasync(fd) != 0) return 82;
    close(fd);
    free(b);
    return 0;
}

/* fsync() is never answered locally: the wrapper first pays any raw-data debt,
 * then always calls the real metadata syscall. Prove the real call remains by
 * checking its error behaviour too. */
static int c_synck(const char *path, int nblk, int which)
{
    void *b = abuf();
    int fd, i, rc;

    if (!b) return 91;
    fd = open(path, O_RDWR | O_DIRECT);
    if (fd < 0) { kv("open_errno", errno); return 92; }
    for (i = 0; i < nblk; i++) {
        fill_block(b, "SYN", i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) return 93;
    }
    errno = 0;
    rc = which ? fdatasync(fd) : fsync(fd);
    kv("sync_rc", rc);
    kv("sync_errno", errno);

    errno = 0; rc = fsync(-1);        kv("fsync_bad_rc", rc);      kv("fsync_bad_errno", errno);
    errno = 0; rc = fdatasync(-1);    kv("fdsync_bad_rc", rc);     kv("fdsync_bad_errno", errno);
    errno = 0; rc = ftruncate(-1, 0); kv("ftrunc_bad_rc", rc);     kv("ftrunc_bad_errno", errno);
    errno = 0; rc = close(-1);        kv("close_bad_rc", rc);      kv("close_bad_errno", errno);
    close(fd);
    free(b);
    return 0;
}

/* A file with no block device behind it (tmpfs) must be declined and must keep
 * working through the ordinary kernel path. */
static int c_plainwrite(const char *path, int nblk)
{
    void *b = abuf();
    int fd, i;

    if (!b) return 101;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { kv("open_errno", errno); return 102; }
    for (i = 0; i < nblk; i++) {
        fill_block(b, "TMP", i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) { kv("pwrite_errno", errno); return 103; }
    }
    kv("sync_rc", fdatasync(fd));
    close(fd);
    free(b);
    return 0;
}

static int c_devnull(void)
{
    char buf[BS];
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0) { kv("open_errno", errno); return 111; }
    memset(buf, 'x', sizeof buf);
    errno = 0;
    kv("write_rc", (long long)write(fd, buf, sizeof buf));
    kv("write_errno", errno);
    errno = 0;
    kv("sync_rc", fsync(fd));
    kv("sync_errno", errno);
    close(fd);
    return 0;
}

/* path_selected() calls strtok() on every open() while armed.  strtok keeps
 * static state that belongs to the CALLER, so an interposer that uses it
 * destroys any tokenisation the application had in progress. */
static int c_strtok(const char *path)
{
    char s[] = "alpha:beta:gamma";
    char *t1, *t2, *t3;
    int fd;

    t1 = strtok(s, ":");
    kvs("tok1", t1 ? t1 : "(null)");

    fd = open(path, O_RDONLY);
    kv("open_ok", fd >= 0);
    if (fd >= 0) close(fd);

    t2 = strtok(NULL, ":");
    kvs("tok2", t2 ? t2 : "(null)");
    t3 = strtok(NULL, ":");
    kvs("tok3", t3 ? t3 : "(null)");
    return 0;
}

/* The large-file ABI.  A translation unit compiled with -D_FILE_OFFSET_BITS=64
 * -- the default for anything using autoconf's AC_SYS_LARGEFILE -- emits calls
 * to open64/pwrite64/ftruncate64, which preload.c does not define. */
static int c_lfs(const char *path, int nblk)
{
    void *b = abuf();
    int fd, i;

    if (!b) return 121;
    fd = open64(path, O_RDWR | O_DIRECT);
    if (fd < 0) { kv("open_errno", errno); return 122; }
    for (i = 0; i < nblk; i++) {
        fill_block(b, "LFS", i);
        if (pwrite64(fd, b, BS, (off64_t)i * BS) != (ssize_t)BS) return 123;
    }
    kv("sync_rc", fdatasync(fd));
    close(fd);
    free(b);
    return 0;
}

/* Durability.  exitos_on_fdatasync() answers the call itself only when every
 * write on the fd took the fast path; it decides that from a counter that only
 * counts writes the interposer SAW.  mode: 0 = control (all writes interposed),
 * 1 = one writev() slipped in, 2 = one pwrite64() slipped in. */
static int c_bypass(const char *path, int nblk, int mode)
{
    void *b = abuf();
    int fd, i, last = nblk - 1;

    if (!b) return 131;
    fd = open(path, O_RDWR);                     /* buffered on purpose */
    if (fd < 0) { kv("open_errno", errno); return 132; }
    for (i = 0; i < last; i++) {
        fill_block(b, "BYP", i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) return 133;
    }
    fill_block(b, "BYP", last);
    if (mode == 0) {
        if (pwrite(fd, b, BS, (off_t)last * BS) != (ssize_t)BS) return 134;
    } else if (mode == 1) {
        struct iovec iov = { b, BS };
        if (lseek(fd, (off_t)last * BS, SEEK_SET) < 0) return 135;
        if (writev(fd, &iov, 1) != (ssize_t)BS) { kv("writev_errno", errno); return 136; }
    } else {
        if (pwrite64(fd, b, BS, (off64_t)last * BS) != (ssize_t)BS) return 137;
    }
    kv("sync_rc", fdatasync(fd));
    close(fd);
    free(b);
    return 0;
}

/* Two files open at once; only one of them is named by EXITOS_FILES. */
static int c_twofiles(const char *pathA, const char *pathB, int nblk)
{
    void *b = abuf();
    int fda, fdb, i;

    if (!b) return 141;
    fdb = open(pathB, O_RDWR | O_DIRECT);          /* the one the pattern names */
    if (fdb < 0) { kv("openB_errno", errno); return 142; }
    fda = open(pathA, O_RDWR | O_DIRECT);          /* the one it does not */
    if (fda < 0) { kv("openA_errno", errno); return 143; }
    for (i = 0; i < nblk; i++) {
        fill_block(b, "AAA", i);
        if (pwrite(fda, b, BS, (off_t)i * BS) != (ssize_t)BS) return 144;
    }
    kv("sync_rc", fdatasync(fda));
    close(fda); close(fdb);
    free(b);
    return 0;
}

/* One write the layer must hand back, a kernel fdatasync, then a write it can
 * service and a second fdatasync. The second sync is the one under test. */
static int c_relatch(const char *path)
{
    void *b = abuf();
    struct stat st;
    int fd;

    if (!b) return 101;
    fd = open(path, O_RDWR | O_DIRECT);
    if (fd < 0) { kv("open_errno", errno); return 102; }
    if (fstat(fd, &st) != 0) { free(b); close(fd); return 103; }

    /* Past the end of the file: the filesystem has to allocate, so this one
     * goes to the kernel and records that the kernel holds data we did not
     * write. */
    fill_block(b, "GROW", 0);
    kv("grow_rc", (long long)pwrite(fd, b, BS, st.st_size));
    kv("grow_sync_rc", fdatasync(fd));

    /* Inside the file and already mapped: this one is ours. */
    fill_block(b, "OURS", 0);
    kv("own_rc", (long long)pwrite(fd, b, BS, 0));
    kv("own_sync_rc", fdatasync(fd));

    close(fd);
    free(b);
    return 0;
}

static int child_main(int argc, char **argv)
{
    const char *sc, *res;
    int rc;

    if (argc < 2) return 2;
    sc  = argv[0];
    res = argv[1];
    g_out = fopen(res, "w");

    if      (!strcmp(sc, "write"))     rc = c_write(argv[2], atoi(argv[3]), argv[4], atoi(argv[5]), atoi(argv[6]), atoi(argv[7]));
    else if (!strcmp(sc, "modes"))     rc = c_modes(argv[2]);
    else if (!strcmp(sc, "fdreuse"))   rc = c_fdreuse(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    else if (!strcmp(sc, "append"))    rc = c_append(argv[2]);
    else if (!strcmp(sc, "trunc"))     rc = c_trunc(argv[2], argv[3], atoi(argv[4]));
    else if (!strcmp(sc, "synck"))     rc = c_synck(argv[2], atoi(argv[3]), atoi(argv[4]));
    else if (!strcmp(sc, "plain"))     rc = c_plainwrite(argv[2], atoi(argv[3]));
    else if (!strcmp(sc, "devnull"))   rc = c_devnull();
    else if (!strcmp(sc, "strtok"))    rc = c_strtok(argv[2]);
    else if (!strcmp(sc, "lfs"))       rc = c_lfs(argv[2], atoi(argv[3]));
    else if (!strcmp(sc, "bypass"))    rc = c_bypass(argv[2], atoi(argv[3]), atoi(argv[4]));
    else if (!strcmp(sc, "twofiles"))  rc = c_twofiles(argv[2], argv[3], atoi(argv[4]));
    else if (!strcmp(sc, "relatch"))   rc = c_relatch(argv[2]);
    else rc = 3;

    kv("rc", rc);
    if (g_out) { fflush(g_out); fclose(g_out); g_out = NULL; }
    return rc;
}

/* ------------------------------------------------------------------ *
 * parent side: fixture + assertions
 * ------------------------------------------------------------------ */

static char g_self[PATH_MAX];
static char g_so[PATH_MAX];
static char g_img[256];
static char g_mnt[256];
static char g_loop[64];
static char g_res[256];
static char g_stats[256];
static char g_shm[256];

static int sh(const char *fmt, ...)
{
    char cmd[1024];
    va_list a; int rc;
    va_start(a, fmt); vsnprintf(cmd, sizeof cmd, fmt, a); va_end(a);
    rc = system(cmd);
    return (rc == -1) ? -1 : WEXITSTATUS(rc);
}

static void teardown(void)
{
    if (g_mnt[0])  (void)sh("mountpoint -q %s && umount %s", g_mnt, g_mnt);
    if (g_loop[0]) (void)sh("losetup -d %s 2>/dev/null", g_loop);
    if (g_mnt[0])  (void)rmdir(g_mnt);
    if (g_img[0])  (void)unlink(g_img);
    if (g_shm[0])  (void)unlink(g_shm);
    g_loop[0] = g_mnt[0] = g_img[0] = g_shm[0] = '\0';
}

static void on_signal(int s) { teardown(); _exit(128 + s); }

/* Run this same binary as a child under (optionally) LD_PRELOAD.
 * Returns the child's exit status, or -1 if it did not exit normally. */
static int run(int preload, const char *files, const char *statsfile, ...)
{
    char *av[16]; int na = 0;
    char *env[8]; int ne = 0;
    char e_pre[PATH_MAX + 16], e_files[4200], e_stats[PATH_MAX + 16];
    static char e_path[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    va_list a; char *s; pid_t p; int st;

    av[na++] = g_self;
    av[na++] = (char *)"child";
    va_start(a, statsfile);
    while ((s = va_arg(a, char *)) != NULL && na < 15) av[na++] = s;
    va_end(a);
    av[na] = NULL;

    env[ne++] = e_path;
    if (preload) { snprintf(e_pre, sizeof e_pre, "LD_PRELOAD=%s", g_so); env[ne++] = e_pre; }
    if (files)   { snprintf(e_files, sizeof e_files, "EXITOS_FILES=%s", files); env[ne++] = e_files; }
    if (statsfile) {
        (void)unlink(statsfile);
        snprintf(e_stats, sizeof e_stats, "EXITOS_STATS=%s", statsfile);
        env[ne++] = e_stats;
    }
    env[ne] = NULL;

    p = fork();
    if (p == 0) {
        dup2(2, 1);                       /* keep the TAP stream clean */
        execve(g_self, av, env);
        _exit(127);
    }
    if (p < 0) return -1;
    if (waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* stats file: four decimal lines, in exitos_stat_id order. */
enum { S_FAST_WRITE = 0, S_FAST_SYNC, S_PASS, S_DECLINED, S_N };
static int read_stats(const char *path, unsigned long long v[S_N])
{
    FILE *f = fopen(path, "r");
    int i;
    for (i = 0; i < S_N; i++) v[i] = 0;
    if (!f) return -1;
    for (i = 0; i < S_N; i++)
        if (fscanf(f, "%llu", &v[i]) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int kv_get(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t kl = strlen(key);
    int found = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '\n'); if (e) *e = '\0';
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            snprintf(out, n, "%s", line + kl + 1);
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static long long kv_num(const char *path, const char *key, long long dflt)
{
    char buf[128];
    if (kv_get(path, key, buf, sizeof buf) != 0) return dflt;
    return strtoll(buf, NULL, 10);
}

static long mode_of(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)(st.st_mode & 07777);
}

/* Read a file back.  direct=1 is mandatory for anything the fast path wrote:
 * src/intercept.c documents that a taken-over write bypasses the page cache,
 * so a buffered read can be served a stale page. */
static int check_blocks(const char *path, int nblk, const char *tag, int direct)
{
    void *b = abuf(), *e = abuf();
    int fd, i, bad = 0;

    if (!b || !e) { free(b); free(e); return -1; }
    fd = open(path, O_RDONLY | (direct ? O_DIRECT : 0));
    if (fd < 0) { free(b); free(e); return -1; }
    for (i = 0; i < nblk; i++) {
        fill_block(e, tag, i);
        if (pread(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) {
            if (bad == 0)
                printf("# %s: block %d could not be read back at all\n", path, i);
            bad++; continue;
        }
        if (memcmp(b, e, BS) != 0) {
            /* Name the first bad block and show what is there instead. A count
             * of mismatches says a rewrite went wrong; it does not say whether
             * the block holds an older payload, another file's bytes, or
             * zeroes, and those have different causes. */
            if (bad == 0) {
                const unsigned char *gb = b, *gw = e;
                printf("# %s: first bad block %d of %d: got %02x%02x%02x%02x"
                       " '%.16s' want %02x%02x%02x%02x '%.16s'\n",
                       path, i, nblk, gb[0], gb[1], gb[2], gb[3], (const char *)b,
                       gw[0], gw[1], gw[2], gw[3], (const char *)e);
            }
            bad++;
        }
    }
    close(fd);
    free(b); free(e);
    return bad;
}

/* Create the file and write every block, from the parent (never preloaded), so
 * the extents are real and CONVERTED before any child opens it. */
static int precreate(const char *path, int nblk, const char *tag)
{
    void *b = abuf();
    int fd, i, rc = 0;

    if (!b) return -1;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { free(b); return -1; }
    if (fallocate(fd, 0, 0, (off_t)BS * nblk) != 0) rc = -1;
    for (i = 0; !rc && i < nblk; i++) {
        fill_block(b, tag, i);
        if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) rc = -1;
    }
    if (!rc && fsync(fd) != 0) rc = -1;
    close(fd);
    free(b);
    return rc;
}

static int cmp_files(const char *a, const char *b)
{
    FILE *fa = fopen(a, "r"), *fb = fopen(b, "r");
    int ca, cb, diff = 0;
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return -1; }
    do { ca = fgetc(fa); cb = fgetc(fb); if (ca != cb) { diff = 1; break; } } while (ca != EOF);
    fclose(fa); fclose(fb);
    return diff;
}

int main(int argc, char **argv)
{
    unsigned long long s[S_N];
    char P[PATH_MAX], A[PATH_MAX], B[PATH_MAX], V[PATH_MAX];
    char dbase[512], darm[512], dinert[512];
    char buf[256];
    FILE *fp;
    ssize_t k;
    int rc;

    if (argc >= 2 && !strcmp(argv[1], "child"))
        return child_main(argc - 2, argv + 2);

    if (!t_need_root()) {
        T_SKIP("needs root: the fixture is a loop device over an image in /tmp");
        T_DONE();
    }

    k = readlink("/proc/self/exe", g_self, sizeof g_self - 1);
    if (k <= 0) { T_SKIP("cannot resolve /proc/self/exe"); T_DONE(); }
    g_self[k] = '\0';

    /* Derive the library from where this binary sits (repo/tests/integ/<name>),
     * not from an absolute path baked in on the machine the test was written on.
     * The baked-in path made every one of the 145 assertions below skip on any
     * other checkout -- and a skipped group still reports "0 failed", so a run
     * that exercised nothing was indistinguishable from a run that passed. */
    if (getenv("EXITOS_PRELOAD_SO")) {
        snprintf(g_so, sizeof g_so, "%s", getenv("EXITOS_PRELOAD_SO"));
    } else {
        char base[PATH_MAX];
        char *slash;
        snprintf(base, sizeof base, "%s", g_self);
        for (k = 0; k < 3; k++) {          /* strip <name>, integ, tests */
            slash = strrchr(base, '/');
            if (!slash) break;
            *slash = '\0';
        }
        snprintf(g_so, sizeof g_so, "%s/libexitos_preload.so", base);
    }
    if (access(g_so, R_OK) != 0) {
        /* Not a skip. The library is built by the same make target that runs
         * this test, so its absence means the run is not measuring what it
         * claims to and must say so. */
        T_OK(0, "libexitos_preload.so is readable at %s", g_so);
        T_DONE();
    }

    snprintf(g_img,   sizeof g_img,   "/tmp/exitos-preload-units-%d.img", (int)getpid());
    snprintf(g_mnt,   sizeof g_mnt,   "/tmp/exitos-preload-units-mnt-%d", (int)getpid());
    snprintf(g_res,   sizeof g_res,   "/tmp/exitos-preload-units-res-%d", (int)getpid());
    snprintf(g_stats, sizeof g_stats, "/tmp/exitos-preload-units-stats-%d", (int)getpid());
    snprintf(g_shm,   sizeof g_shm,   "/dev/shm/exitos-preload-units-%d", (int)getpid());

    atexit(teardown);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);

    if (sh("truncate -s 256M %s", g_img) != 0) { T_SKIP("cannot create image"); T_DONE(); }
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "losetup --find --show %s 2>/dev/null", g_img);
        fp = popen(cmd, "r");
        if (!fp || !fgets(g_loop, sizeof g_loop, fp)) { if (fp) pclose(fp); T_SKIP("no free loop device"); T_DONE(); }
        pclose(fp);
        { char *e = strchr(g_loop, '\n'); if (e) *e = '\0'; }
    }
    if (sh("mkfs.ext4 -q -F -b 4096 %s", g_loop) != 0) { T_SKIP("mkfs.ext4 failed"); T_DONE(); }
    if (mkdir(g_mnt, 0755) != 0)                       { T_SKIP("mkdir mnt failed"); T_DONE(); }
    if (sh("mount %s %s", g_loop, g_mnt) != 0)         { T_SKIP("mount failed"); T_DONE(); }
    printf("# fixture: %s -> %s on %s\n", g_img, g_loop, g_mnt);

    /* ============================================================== *
     * GROUP A -- open()/openat() varargs.
     *
     * Property defended: the interposer must pass the CALLER's mode through
     * untouched, and must not invent one.  It matters because getting varargs
     * wrong here silently changes the permissions of every file an application
     * creates -- a security regression that no data checksum would ever catch.
     * Method: run the identical scenario three ways (no library, library but
     * inert, library armed) and diff the resulting st_mode.  A differential
     * test cannot be fooled by a local umask or filesystem policy.
     * ============================================================== */
    snprintf(dbase,  sizeof dbase,  "%s/base",  g_mnt);
    snprintf(dinert, sizeof dinert, "%s/inert", g_mnt);
    snprintf(darm,   sizeof darm,   "%s/armed", g_mnt);
    mkdir(dbase, 0755); mkdir(dinert, 0755); mkdir(darm, 0755);

    rc = run(0, NULL, NULL, "modes", g_res, dbase, NULL);
    T_EQ(rc, 0, "A: baseline modes child (no LD_PRELOAD) succeeded");
    rc = run(1, NULL, NULL, "modes", g_res, dinert, NULL);
    T_EQ(rc, 0, "A: inert modes child (library loaded, EXITOS_FILES unset) succeeded");
    rc = run(1, "m_", g_stats, "modes", g_res, darm, NULL);
    T_EQ(rc, 0, "A: armed modes child (EXITOS_FILES=m_) succeeded");

    {
        static const char *names[] = { "m_open", "m_openat", "m_openat_rel",
                                       "m_zero", "m_all", "m_setgid", "m_tmpfile" };
        static const char *why[] = {
            "open(O_CREAT, 0642)", "openat(AT_FDCWD, O_CREAT, 0611)",
            "openat(dirfd, rel, O_CREAT, 0604)", "open(O_CREAT, 0000)",
            "open(O_CREAT, 0777)", "open(O_CREAT, 02640)", "open(O_TMPFILE, 0642)" };
        size_t i;
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            long mb, mi, ma;
            snprintf(P, sizeof P, "%s/%s", dbase,  names[i]); mb = mode_of(P);
            snprintf(P, sizeof P, "%s/%s", dinert, names[i]); mi = mode_of(P);
            snprintf(P, sizeof P, "%s/%s", darm,   names[i]); ma = mode_of(P);
            if (mb < 0) { T_SKIP("A: %s not created even at baseline", names[i]); continue; }
            T_EQ(mi, mb, "A: INERT %s -> mode %04lo, baseline %04lo", why[i], (unsigned long)mi, (unsigned long)mb);
            T_EQ(ma, mb, "A: ARMED %s -> mode %04lo, baseline %04lo", why[i], (unsigned long)ma, (unsigned long)mb);
        }
    }
    /* open() with no O_CREAT passes no mode argument at all: the mode of the
     * existing file must be untouched and no vararg may be read. */
    T_EQ(kv_num(g_res, "nocreat_mode", -1), 0642,
         "A: armed open(existing, O_RDWR) left mode 0642 and read no vararg");
    {
        snprintf(P, sizeof P, "%s/m_open", dbase);
        snprintf(A, sizeof A, "%s/m_open", darm);
        T_EQ(cmp_files(P, A), 0, "A: armed run wrote the same bytes as baseline for m_open");
    }

    /* ============================================================== *
     * GROUP B -- inertness.
     *
     * Property defended: src/preload.c's header says "Nothing is accelerated
     * unless EXITOS_FILES names it, so loading this library is inert by default
     * -- that is deliberate: a bug here corrupts data silently."  Inert must
     * mean byte-identical content, identical mode, and every counter at zero,
     * because a counter that moves proves the decision path ran.
     * ============================================================== */
    snprintf(P, sizeof P, "%s/inert_base.log", g_mnt);
    snprintf(A, sizeof A, "%s/inert_pre.log",  g_mnt);
    T_EQ(run(0, NULL, NULL, "write", g_res, P, "16", "INE", "1", "1", "1", NULL), 0,
         "B: baseline writer succeeded");
    T_EQ(run(1, NULL, g_stats, "write", g_res, A, "16", "INE", "1", "1", "1", NULL), 0,
         "B: inert writer (library loaded, EXITOS_FILES unset) succeeded");
    T_EQ(cmp_files(P, A), 0, "B: inert output is byte-identical to baseline");
    T_EQ(mode_of(A), mode_of(P), "B: inert output has the same mode as baseline");
    T_EQ(check_blocks(A, 16, "INE", 1), 0, "B: inert output has all 16 blocks correct");
    T_EQ(read_stats(g_stats, s), 0, "B: inert run still dumped EXITOS_STATS");
    T_EQ(s[S_FAST_WRITE], 0, "B: inert run took over 0 writes");
    T_EQ(s[S_FAST_SYNC],  0, "B: inert run took over 0 fdatasyncs");
    T_EQ(s[S_PASS],       0, "B: inert run recorded 0 PASS decisions (no decision path ran)");
    T_EQ(s[S_DECLINED],   0, "B: inert run recorded 0 DECLINED registrations");

    /* EXITOS_FILES set but empty must be exactly as inert. */
    snprintf(B, sizeof B, "%s/inert_empty.log", g_mnt);
    T_EQ(run(1, "", g_stats, "write", g_res, B, "16", "INE", "1", "1", "1", NULL), 0,
         "B: EXITOS_FILES=\"\" writer succeeded");
    T_EQ(cmp_files(P, B), 0, "B: EXITOS_FILES=\"\" is byte-identical to baseline");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 0, "B: EXITOS_FILES=\"\" took over 0 writes");

    /* ============================================================== *
     * GROUP C -- EXITOS_FILES matching.
     *
     * Property defended: matching is substring-based and colon-separated, and
     * an EMPTY pattern must never behave like a wildcard.  A pattern list of
     * nothing but separators arming every file would hand raw-LBA writes to
     * files nobody opted in for -- the exact opposite of "act only on
     * explicitly registered files" (include/exitos_intercept.h).
     * ============================================================== */
    snprintf(P, sizeof P, "%s/sel_hit.log", g_mnt);
    T_OK(precreate(P, 16, "OLD") == 0, "C: fixture sel_hit.log created and converted");

    T_EQ(run(1, "sel_hit", g_stats, "write", g_res, P, "16", "M1", "1", "1", "0", NULL), 0,
         "C: matching-pattern writer succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0, "C: a matching pattern actually armed the fast path (%llu takeovers)", s[S_FAST_WRITE]);
    T_EQ(check_blocks(P, 16, "M1", 1), 0, "C: matching-pattern data is correct");

    T_EQ(run(1, "no_such_file", g_stats, "write", g_res, P, "16", "M2", "1", "1", "0", NULL), 0,
         "C: non-matching-pattern writer succeeded");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 0, "C: a non-matching pattern took over nothing");
    T_EQ(s[S_DECLINED],   0, "C: a non-matching pattern did not even attempt registration");
    T_EQ(check_blocks(P, 16, "M2", 1), 0, "C: non-matching-pattern data is correct");

    T_EQ(run(1, "aaa:bbb:sel_hit:ccc", g_stats, "write", g_res, P, "16", "M3", "1", "1", "0", NULL), 0,
         "C: multi-pattern writer succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0, "C: the third of four colon-separated patterns matched");

    T_EQ(run(1, "::sel_hit::", g_stats, "write", g_res, P, "16", "M4", "1", "1", "0", NULL), 0,
         "C: pattern list with empty fields succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0, "C: empty fields around a real pattern do not break matching");

    T_EQ(run(1, ":::", g_stats, "write", g_res, P, "16", "M5", "1", "1", "0", NULL), 0,
         "C: all-empty pattern list succeeded");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 0, "C: an all-empty pattern list matches NOTHING (no wildcard)");
    T_EQ(s[S_DECLINED],   0, "C: an all-empty pattern list attempts no registration");
    T_EQ(check_blocks(P, 16, "M5", 1), 0, "C: all-empty pattern list data is correct");

    /* the pattern names a DIFFERENT open file than the one being written */
    snprintf(A, sizeof A, "%s/other_a.log",  g_mnt);
    snprintf(B, sizeof B, "%s/named_b.log",  g_mnt);
    T_OK(precreate(A, 16, "OLD") == 0, "C: fixture other_a.log created");
    T_OK(precreate(B, 16, "OLD") == 0, "C: fixture named_b.log created");
    T_EQ(run(1, "named_b", g_stats, "twofiles", g_res, A, B, "16", NULL), 0,
         "C: two-open child succeeded");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 0, "C: a pattern matching the OTHER open file took over no write on this one");
    T_EQ(check_blocks(A, 16, "AAA", 1), 0, "C: the unnamed file has correct data");
    T_EQ(check_blocks(B, 16, "OLD", 1), 0, "C: the named-but-unwritten file was left untouched");

    /* path_selected() runs strtok() on the caller's behalf on EVERY armed
     * open().  strtok keeps static state that belongs to the application, so
     * the control below (same child, library inert) pins the blame precisely:
     * if inert is clean and armed is not, the interposer destroyed it. */
    T_EQ(run(1, NULL, NULL, "strtok", g_res, P, NULL), 0, "C: strtok control child (inert) succeeded");
    kv_get(g_res, "tok2", buf, sizeof buf);
    T_OK(!strcmp(buf, "beta"), "C: control -- with the library inert, strtok() still yields 'beta' (got '%s')", buf);
    kv_get(g_res, "tok3", buf, sizeof buf);
    T_OK(!strcmp(buf, "gamma"), "C: control -- with the library inert, strtok() still yields 'gamma' (got '%s')", buf);

    T_EQ(run(1, "sel_hit", g_stats, "strtok", g_res, P, NULL), 0, "C: strtok child (armed, matching) succeeded");
    T_OK(kv_get(g_res, "tok1", buf, sizeof buf) == 0 && !strcmp(buf, "alpha"),
         "C: strtok() first token is 'alpha' (got '%s')", buf);
    kv_get(g_res, "tok2", buf, sizeof buf);
    T_OK(!strcmp(buf, "beta"),
         "C: an armed open() between strtok() calls must not destroy the caller's strtok state (got '%s', want 'beta')", buf);
    kv_get(g_res, "tok3", buf, sizeof buf);
    T_OK(!strcmp(buf, "gamma"), "C: third strtok() token is 'gamma' (got '%s')", buf);

    /* Even a pattern that matches nothing still tokenises the pattern list. */
    T_EQ(run(1, "no_such_file", g_stats, "strtok", g_res, P, NULL), 0, "C: strtok child (armed, non-matching) succeeded");
    kv_get(g_res, "tok2", buf, sizeof buf);
    T_OK(!strcmp(buf, "beta"),
         "C: an armed open() that matches NOTHING must still not touch the caller's strtok state (got '%s', want 'beta')", buf);

    /* ============================================================== *
     * GROUP D -- close() must unregister, and fd numbers get recycled.
     *
     * Property defended: a registration is a licence to write raw LBAs that
     * belong to ONE file.  If the registration outlives the fd, the next file
     * to be handed that fd number inherits somebody else's device blocks, and
     * its data is written on top of them.  There is no checksum in the system
     * that would notice -- both files are "successfully written".
     * ============================================================== */
    snprintf(A, sizeof A, "%s/reuse_a.log", g_mnt);
    snprintf(B, sizeof B, "%s/reuse_b.log", g_mnt);

    T_OK(precreate(B, 16, "BBB") == 0 && precreate(A, 16, "ZZZ") == 0, "D: close() fixtures created");
    T_EQ(run(1, "reuse_b", g_stats, "fdreuse", g_res, B, A, "16", "0", NULL), 0, "D: close() child succeeded");
    T_EQ(kv_num(g_res, "reused", -1), 1, "D: the fd number really was recycled (precondition)");
    T_EQ(check_blocks(A, 16, "AAA", 1), 0, "D: after close(), the recycled fd wrote into its OWN file");
    T_EQ(check_blocks(B, 16, "BBB", 1), 0, "D: after close(), the old file was not overwritten");

    T_OK(precreate(B, 16, "BBB") == 0 && precreate(A, 16, "ZZZ") == 0, "D: fclose() fixtures created");
    T_EQ(run(1, "reuse_b", g_stats, "fdreuse", g_res, B, A, "16", "1", NULL), 0, "D: fclose() child succeeded");
    T_EQ(kv_num(g_res, "reused", -1), 1, "D: fclose() recycled the fd number (precondition)");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 1,
         "D: after fclose() only the 1 write made before it may be taken over (was %llu)", s[S_FAST_WRITE]);
    T_EQ(check_blocks(A, 16, "AAA", 1), 0, "D: after fclose(), the recycled fd wrote into its OWN file");
    T_EQ(check_blocks(B, 16, "BBB", 1), 0, "D: after fclose(), the old file was not overwritten");

    T_OK(precreate(B, 16, "BBB") == 0 && precreate(A, 16, "ZZZ") == 0, "D: dup2() fixtures created");
    T_EQ(run(1, "reuse_b", g_stats, "fdreuse", g_res, B, A, "16", "2", NULL), 0, "D: dup2() child succeeded");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_WRITE], 1,
         "D: after dup2() onto a registered fd, only the 1 pre-dup2 write may be taken over (was %llu)", s[S_FAST_WRITE]);
    T_EQ(check_blocks(A, 16, "AAA", 1), 0, "D: after dup2() onto a registered fd, writes went to the NEW file");
    T_EQ(check_blocks(B, 16, "BBB", 1), 0, "D: after dup2() onto a registered fd, the old file was not overwritten");

    /* ============================================================== *
     * GROUP E -- ftruncate() invalidates the cached mapping.
     *
     * Property defended, quoting src/preload.c: "Must run first: it invalidates
     * cached mappings so a later write cannot reuse an LBA that no longer
     * belongs to this file."  The victim file below deliberately claims the
     * blocks the truncation released, so a stale mapping does not merely write
     * to dead space -- it writes into another file.
     * ============================================================== */
    snprintf(P, sizeof P, "%s/trunc.log",    g_mnt);
    snprintf(V, sizeof V, "%s/trunc_victim", g_mnt);
    T_OK(precreate(P, 32, "OLD") == 0, "E: truncate fixture created and converted");
    unlink(V);
    T_EQ(run(1, "trunc.log", g_stats, "trunc", g_res, P, V, "1", NULL), 0, "E: aligned-truncate child succeeded");
    T_EQ(kv_num(g_res, "trunc_rc", -1), 0, "E: ftruncate() returned 0");
    T_EQ(kv_num(g_res, "size_after_trunc", -1), (long long)8 * BS, "E: ftruncate() actually shortened the file");
    T_EQ(check_blocks(P, 32, "T2", 1), 0, "E: every block of the regrown log holds the post-truncate payload");
    T_EQ(check_blocks(V, 24, "VIC", 0), 0, "E: the file that claimed the released blocks was not corrupted");

    snprintf(P, sizeof P, "%s/trunc2.log",    g_mnt);
    snprintf(V, sizeof V, "%s/trunc2_victim", g_mnt);
    T_OK(precreate(P, 32, "OLD") == 0, "E: unaligned-truncate fixture created");
    unlink(V);
    T_EQ(run(1, "trunc2.log", g_stats, "trunc", g_res, P, V, "0", NULL), 0, "E: unaligned-truncate child succeeded");
    T_EQ(kv_num(g_res, "size_after_trunc", -1), (long long)8 * BS + 1000,
         "E: ftruncate() to a non-block-aligned length shortened the file exactly");
    T_EQ(check_blocks(P, 32, "T2", 1), 0, "E: unaligned truncate then rewrite still yields correct data");
    T_EQ(check_blocks(V, 24, "VIC", 0), 0, "E: unaligned truncate did not corrupt the victim");

    /* ============================================================== *
     * GROUP F -- fsync() always retains the real metadata syscall.
     *
     * A fast path that answered fsync() locally would drop inode updates that
     * only the filesystem can persist. The wrapper may add a raw device FLUSH,
     * but it must never replace the real fsync: FAST_SYNC stays zero, success
     * still returns zero, and fsync(-1) still reports EBADF.
     * ============================================================== */
    snprintf(P, sizeof P, "%s/sync.log", g_mnt);
    T_OK(precreate(P, 16, "OLD") == 0, "F: fsync fixture created and converted");
    T_EQ(run(1, "sync.log", g_stats, "synck", g_res, P, "16", "0", NULL), 0, "F: fsync child succeeded");
    T_EQ(kv_num(g_res, "sync_rc", -1), 0, "F: fsync() on a registered fd returned 0");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0, "F: the fsync child did use the fast path for its writes (%llu)", s[S_FAST_WRITE]);
    T_EQ(s[S_FAST_SYNC], 0, "F: fsync() was NOT taken over (FAST_SYNC stayed 0)");
    T_EQ(kv_num(g_res, "fsync_bad_rc", 0), -1, "F: fsync(-1) still returns -1 while armed");
    T_EQ(kv_num(g_res, "fsync_bad_errno", 0), EBADF, "F: fsync(-1) still sets EBADF while armed");
    T_EQ(kv_num(g_res, "fdsync_bad_rc", 0), -1, "F: fdatasync(-1) still returns -1 while armed");
    T_EQ(kv_num(g_res, "fdsync_bad_errno", 0), EBADF, "F: fdatasync(-1) still sets EBADF while armed");
    T_EQ(kv_num(g_res, "ftrunc_bad_rc", 0), -1, "F: ftruncate(-1,0) still returns -1 while armed");
    T_EQ(kv_num(g_res, "ftrunc_bad_errno", 0), EBADF, "F: ftruncate(-1,0) still sets EBADF while armed");
    T_EQ(kv_num(g_res, "close_bad_rc", 0), -1, "F: close(-1) still returns -1 while armed");
    T_EQ(kv_num(g_res, "close_bad_errno", 0), EBADF, "F: close(-1) still sets EBADF while armed");
    T_EQ(check_blocks(P, 16, "SYN", 1), 0, "F: the fsync child's data is correct");

    /* the deliberate asymmetry: fdatasync IS eligible for takeover */
    T_OK(precreate(P, 16, "OLD") == 0, "F: fdatasync fixture re-created");
    T_EQ(run(1, "sync.log", g_stats, "synck", g_res, P, "16", "1", NULL), 0, "F: fdatasync child succeeded");
    T_EQ(kv_num(g_res, "sync_rc", -1), 0, "F: fdatasync() on a registered fd returned 0");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_SYNC] > 0, "F: fdatasync() WAS taken over (%llu), which is what makes the fsync result meaningful", s[S_FAST_SYNC]);

    /* ============================================================== *
     * GROUP G -- a file with no block device behind it must be declined.
     *
     * Property defended, quoting include/exitos_intercept.h: registration
     * "Refuses on any unsupported device stack, in which case the fd simply
     * keeps the normal path."  tmpfs has no block device at all, so there is no
     * LBA to write; declining must be silent, counted, and lossless.
     * ============================================================== */
    T_EQ(run(1, "exitos-preload-units", g_stats, "plain", g_res, g_shm, "8", NULL), 0,
         "G: tmpfs child succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_DECLINED] > 0, "G: registration on tmpfs was DECLINED (%llu)", s[S_DECLINED]);
    T_EQ(s[S_FAST_WRITE], 0, "G: no write on a tmpfs file was taken over");
    T_EQ(check_blocks(g_shm, 8, "TMP", 0), 0, "G: the tmpfs write still produced correct data");
    T_EQ(kv_num(g_res, "sync_rc", -1), 0, "G: fdatasync on the tmpfs file still returned 0");

    T_EQ(run(0, NULL, NULL, "devnull", g_res, NULL), 0, "G: baseline /dev/null child succeeded");
    {
        long long bw = kv_num(g_res, "write_rc", -1);
        long long bs_rc = kv_num(g_res, "sync_rc", -99);
        long long bs_er = kv_num(g_res, "sync_errno", -99);
        T_EQ(run(1, "/dev/null", g_stats, "devnull", g_res, NULL), 0, "G: armed /dev/null child succeeded");
        read_stats(g_stats, s);
        T_OK(s[S_DECLINED] > 0, "G: a matching non-regular file was DECLINED (%llu)", s[S_DECLINED]);
        T_EQ(s[S_FAST_WRITE], 0, "G: no write to a matching /dev/null was taken over");
        T_EQ(kv_num(g_res, "write_rc", -1), bw, "G: write() to a matching /dev/null returns what libc returns (%lld)", bw);
        T_EQ(kv_num(g_res, "sync_rc", -99), bs_rc, "G: fsync() on /dev/null returns what libc returns (%lld)", bs_rc);
        T_EQ(kv_num(g_res, "sync_errno", -99), bs_er, "G: fsync() on /dev/null reports the same errno as libc (%lld)", bs_er);
    }

    /* ============================================================== *
     * GROUP H -- O_APPEND.
     *
     * Property defended: write() on an O_APPEND fd lands at end-of-file; the
     * kernel ignores the file offset entirely (open(2): "the file offset is
     * first set to the end of the file before writing").  preload.c's write()
     * derives the target offset from lseek(fd, 0, SEEK_CUR) instead, which for
     * a freshly opened O_APPEND fd is 0.  This is the flag a log writer -- the
     * workload this whole library exists for -- opens its log with.
     * ============================================================== */
    snprintf(P, sizeof P, "%s/append_base.log", g_mnt);
    snprintf(A, sizeof A, "%s/append_arm.log",  g_mnt);
    T_OK(precreate(P, 8, "OLD") == 0 && precreate(A, 8, "OLD") == 0, "H: append fixtures created");
    T_EQ(run(0, NULL, NULL, "append", g_res, P, NULL), 0, "H: baseline append child succeeded");
    {
        long long base_size = kv_num(g_res, "size_after", -1);
        long long base_rc   = kv_num(g_res, "write_rc", -1);
        T_EQ(base_rc, (long long)BS, "H: baseline append wrote a full block");
        T_EQ(base_size, (long long)9 * BS, "H: baseline append extended the file to 9 blocks");

        long long base_pos = kv_num(g_res, "pos_after", -1);
        T_EQ(base_pos, (long long)9 * BS, "H: baseline append left the file position at 9 blocks");

        T_EQ(run(1, "append_arm", g_stats, "append", g_res, A, NULL), 0, "H: armed append child succeeded");
        T_EQ(kv_num(g_res, "write_rc", -1), base_rc, "H: armed append returned the same byte count as baseline");
        T_EQ(kv_num(g_res, "pos_after", -1), base_pos,
             "H: armed O_APPEND write left the file position where the kernel leaves it");
        T_EQ(kv_num(g_res, "size_after", -1), base_size,
             "H: armed O_APPEND write extended the file exactly as the kernel does");
        T_EQ(check_blocks(A, 8, "OLD", 1), 0,
             "H: armed O_APPEND write did NOT overwrite the existing blocks of the file");
        {
            void *b = abuf(), *e = abuf();
            int fd = open(A, O_RDONLY | O_DIRECT);
            int same = -1;
            if (b && e && fd >= 0) {
                fill_block(e, "APPENDED", 0);
                same = (pread(fd, b, BS, (off_t)8 * BS) == (ssize_t)BS && !memcmp(b, e, BS)) ? 0 : 1;
            }
            if (fd >= 0) close(fd);
            free(b); free(e);
            T_EQ(same, 0, "H: the appended block landed at end-of-file (block 8), not at offset 0");
        }
    }

    /* ============================================================== *
     * GROUP I -- which writes the interposer can SEE.
     *
     * Property defended, quoting include/exitos_intercept.h on
     * exitos_fd_all_writes_took_fast_path(): "If even one write was handed back
     * to the kernel, the kernel still holds data that only its own fdatasync
     * persists, so ours must not answer the call."  The counter behind that
     * decision only counts writes that reached preload.c.  writev() and
     * pwrite64() are ordinary libc calls that preload.c does not define, so
     * their data sits in the page cache while the counter still reads zero.
     * pwrite64() is not exotic: every translation unit built with
     * -D_FILE_OFFSET_BITS=64 (autoconf's AC_SYS_LARGEFILE) calls it in place of
     * pwrite().
     * ============================================================== */
    snprintf(P, sizeof P, "%s/bypass.log", g_mnt);
    T_OK(precreate(P, 8, "OLD") == 0, "I: bypass fixture created and converted");
    T_EQ(run(1, "bypass.log", g_stats, "bypass", g_res, P, "8", "0", NULL), 0, "I: control child succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0, "I: control -- all 8 writes were interposed (%llu takeovers)", s[S_FAST_WRITE]);
    T_OK(s[S_FAST_SYNC] > 0, "I: control -- fdatasync was taken over, as designed");

    T_OK(precreate(P, 8, "OLD") == 0, "I: writev fixture re-created");
    T_EQ(run(1, "bypass.log", g_stats, "bypass", g_res, P, "8", "1", NULL), 0, "I: writev child succeeded");
    read_stats(g_stats, s);
    T_EQ(s[S_FAST_SYNC], 0,
         "I: after one writev() the kernel holds data, so fdatasync() must NOT be answered by the library");

    T_OK(precreate(P, 8, "OLD") == 0, "I: pwrite64 fixture re-created");
    T_EQ(run(1, "bypass.log", g_stats, "bypass", g_res, P, "8", "2", NULL), 0, "I: pwrite64 child succeeded");
    read_stats(g_stats, s);
    /* pwrite64 is now interposed (see the large-file assertion below, which
     * requires exactly that), so the write is serviced by the shortcut and the
     * kernel is left holding nothing. Asserting the opposite here contradicted
     * that assertion: a pwrite64 cannot be both intercepted and a bypass. */
    T_EQ(s[S_FAST_SYNC], 1,
         "I: pwrite64() is interposed like pwrite(), so fdatasync() is answered by the library");

    /* The large-file entry points are not interposed at all. */
    snprintf(P, sizeof P, "%s/lfs.log", g_mnt);
    T_OK(precreate(P, 8, "OLD") == 0, "I: LFS fixture created and converted");
    T_EQ(run(1, "lfs.log", g_stats, "lfs", g_res, P, "8", NULL), 0, "I: open64/pwrite64 child succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_FAST_WRITE] > 0,
         "I: a program built for the large-file ABI (open64/pwrite64) is still intercepted (%llu takeovers)",
         s[S_FAST_WRITE]);
    T_EQ(check_blocks(P, 8, "LFS", 1), 0, "I: open64/pwrite64 data is correct either way");

    /* ================================================================ *
     * GROUP J -- a kernel fdatasync pays the debt that one fallback created.
     *
     * REQUIREMENT: the shortcut must be able to engage again after the kernel
     * has persisted whatever it was holding. Measured before this was fixed: a
     * write-ahead-log workload of 1600 writes and 801 fdatasync calls answered
     * ZERO syncs itself, because the pass that lays the file out always
     * contains a write the layer hands back, and the flag that records the debt
     * was never cleared. The interposer has to tell the library when the real
     * fdatasync returned success -- the library cannot see that by itself.
     * ================================================================ */
    snprintf(P, sizeof P, "%s/relatch.log", g_mnt);
    T_OK(precreate(P, 8, "OLD") == 0, "J: fixture created and converted");
    T_EQ(run(1, "relatch.log", g_stats, "relatch", g_res, P, NULL), 0,
         "J: child succeeded");
    read_stats(g_stats, s);
    T_OK(s[S_PASS] > 0,
         "J: control -- the growing write really was handed back (%llu passes)",
         s[S_PASS]);
    T_OK(s[S_FAST_WRITE] > 0,
         "J: control -- the later write really was serviced by us (%llu)",
         s[S_FAST_WRITE]);
    T_OK(s[S_FAST_SYNC] > 0,
         "J: the fdatasync after the kernel had been synced was answered by us "
         "(%llu), instead of the one early fallback disabling the shortcut for "
         "the rest of the descriptor's life", s[S_FAST_SYNC]);

    (void)unlink(g_res);
    (void)unlink(g_stats);
    T_DONE();
}
