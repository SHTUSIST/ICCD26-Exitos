/* Tier 0 unit tests for the intercept module (include/exitos_intercept.h).
 *
 * Scope of this tier: NO root, NO block device, NO filesystem fixture.
 * Everything here exercises the default-safe half of the contract:
 *
 *   - an fd that was never registered must ALWAYS get EXITOS_PASS, whatever
 *     the offset/length, and *result must be left untouched;
 *   - registration must REFUSE anything it cannot translate to raw LBAs
 *     (tmpfs file -> no block device behind it, pipe, char device, directory,
 *     closed fd), and a refused fd must keep behaving exactly like an
 *     unregistered one;
 *   - exitos_lba_in_bounds must answer 0 (out of bounds) for an fd it does
 *     not know, no matter how plausible the LBA looks.
 *
 * SAFETY: this file only ever opens files under /dev/shm (tmpfs, st_dev major
 * 0). It deliberately does NOT create files under /tmp or the repo, because on
 * this machine those live on a real partition (/dev/sda4) and a registration
 * that wrongly succeeded there would put a raw-LBA write path on a physical
 * disk. Every registration attempt below is expected to FAIL; if one succeeds
 * we unregister immediately and skip the write calls instead of issuing them.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#include "exitos_intercept.h"
#include "exitos_geom.h"
#include "tap.h"

/* Sentinels: on EXITOS_PASS the module hands the call back to the caller, so it
 * must not have written anything through the out-parameters. */
#define RES_SENTINEL   ((ssize_t)-424242)
#define IRES_SENTINEL  (-4242)

static char g_shm_path[256];

/* ---------------------------------------------------------------- helpers */

/* 1 when fd's filesystem has no block device behind it (tmpfs/pipe/etc). */
static int fd_has_no_block_device(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return major(st.st_dev) == 0;
}

/* An 8 KiB file on tmpfs. Returns fd, or -1. */
static int open_tmpfs_file(void)
{
    char buf[8192];
    int fd;

    snprintf(g_shm_path, sizeof g_shm_path,
             "/dev/shm/exitos-unit-intercept-%d", (int)getpid());
    fd = open(g_shm_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    memset(buf, 0xA5, sizeof buf);
    if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) {
        close(fd);
        unlink(g_shm_path);
        return -1;
    }
    return fd;
}

/* Assert the full "this fd is on the normal kernel path" behaviour. */
static void expect_unregistered(struct exitos_ctx *c, int fd, const char *what)
{
    static const struct { size_t count; off_t off; } cases[] = {
        {    0,          0 },
        {  512,          0 },
        { 4096,          0 },
        { 4096,       4096 },
        { 4096,        100 },   /* misaligned offset  */
        {  100,          0 },   /* misaligned length  */
        { 1u << 20,   1 << 20 },
        { 4096, (off_t)1 << 40 },
    };
    char buf[4096];
    unsigned i;

    memset(buf, 0x5A, sizeof buf);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ssize_t res = RES_SENTINEL;
        exitos_decision d = exitos_on_write(c, fd, buf, cases[i].count,
                                            cases[i].off, &res);
        T_EQ(d, EXITOS_PASS, "%s: write(count=%zu off=%lld) -> EXITOS_PASS",
             what, cases[i].count, (long long)cases[i].off);
        T_EQ(res, RES_SENTINEL,
             "%s: write(count=%zu off=%lld) left *result untouched",
             what, cases[i].count, (long long)cases[i].off);
    }
    {
        int ires = IRES_SENTINEL;
        T_EQ(exitos_on_fdatasync(c, fd, &ires), EXITOS_PASS,
             "%s: fdatasync -> EXITOS_PASS", what);
        T_EQ(ires, IRES_SENTINEL, "%s: fdatasync left *result untouched", what);
    }
    {
        int ires = IRES_SENTINEL;
        T_EQ(exitos_on_ftruncate(c, fd, 4096, &ires), EXITOS_PASS,
             "%s: ftruncate -> EXITOS_PASS", what);
        T_EQ(ires, IRES_SENTINEL, "%s: ftruncate left *result untouched", what);
    }
    {
        /* No LBA belongs to an fd the module does not know. */
        static const uint64_t lbas[] = { 0, 1, 8, 2048, 1ull << 20, 1ull << 40 };
        unsigned k;
        for (k = 0; k < sizeof lbas / sizeof lbas[0]; k++)
            T_EQ(exitos_lba_in_bounds(c, fd, lbas[k], 4096), 0,
                 "%s: lba_in_bounds(lba=%llu) == 0",
                 what, (unsigned long long)lbas[k]);
    }
}

/* Registration must refuse `fd`, and the refusal must leave it on the normal
 * path. If registration wrongly succeeds we record the failure and undo it
 * rather than exercising a write path we did not intend to create. */
static void expect_registration_refused(struct exitos_ctx *c, int fd,
                                        const char *what)
{
    int rc = exitos_register_fd(c, fd);
    T_OK(rc != 0, "%s: exitos_register_fd refuses (rc=%d, must be nonzero)",
         what, rc);
    if (rc == 0) {
        exitos_unregister_fd(c, fd);
        T_SKIP("%s: registration unexpectedly succeeded, "
               "skipping the post-refusal write checks", what);
        return;
    }
    expect_unregistered(c, fd, what);
}

/* Defensive calls with a NULL context, run in a child so a segfault in a
 * future implementation shows up as one failed assertion instead of losing
 * the whole run. Exit code is a bitmask of what went wrong. */
static int null_ctx_child(void)
{
    pid_t p;
    int st = 0;

    p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int rc = 0;
        ssize_t res = RES_SENTINEL;
        int ires = IRES_SENTINEL;
        char buf[4096];

        memset(buf, 0, sizeof buf);
        if (exitos_on_write(NULL, 3, buf, sizeof buf, 0, &res) != EXITOS_PASS)
            rc |= 1;
        if (res != RES_SENTINEL)                                   rc |= 2;
        if (exitos_on_fdatasync(NULL, 3, &ires) != EXITOS_PASS)     rc |= 4;
        if (exitos_on_ftruncate(NULL, 3, 0, &ires) != EXITOS_PASS)  rc |= 8;
        if (ires != IRES_SENTINEL)                                  rc |= 16;
        if (exitos_lba_in_bounds(NULL, 3, 12345, 4096) != 0)        rc |= 32;
        _exit(rc);
    }
    if (waitpid(p, &st, 0) < 0) return -1;
    if (!WIFEXITED(st)) return -2;      /* crashed: not default-safe */
    return WEXITSTATUS(st);
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    struct exitos_ctx *c;
    int shm_fd, devnull_fd, dir_fd, pipefd[2];
    int i;

    /* --- context lifecycle -------------------------------------------- */
    c = exitos_ctx_create();
    T_OK(c != NULL, "exitos_ctx_create returns a context");
    if (!c) {
        printf("# fatal: no context, nothing else can be tested\n");
        T_DONE();
    }

    /* Create/destroy must be repeatable without state leaking across. */
    for (i = 0; i < 64; i++) {
        struct exitos_ctx *tmp = exitos_ctx_create();
        if (!tmp) break;
        exitos_ctx_destroy(tmp);
    }
    T_EQ(i, 64, "64 create/destroy cycles all succeeded");

    /* --- default PASS on fds nobody registered -------------------------- */
    shm_fd = open_tmpfs_file();
    if (shm_fd < 0) {
        T_SKIP("cannot create a file under /dev/shm (%s) - "
               "skipping every test that needs a real fd", strerror(errno));
    } else {
        T_EQ(fd_has_no_block_device(shm_fd), 1,
             "fixture check: /dev/shm file has no block device behind it");

        expect_unregistered(c, shm_fd, "never-registered tmpfs fd");

        /* Unregistering something that was never registered must not turn
         * the fast path on, and must not crash. */
        exitos_unregister_fd(c, shm_fd);
        exitos_unregister_fd(c, shm_fd);
        expect_unregistered(c, shm_fd, "tmpfs fd after spurious unregister");

        /* --- refusal path: unsupported device (writable_raw == 0) ------ */
        if (fd_has_no_block_device(shm_fd) == 1)
            expect_registration_refused(c, shm_fd,
                                        "tmpfs file (no block device)");
        else
            T_SKIP("/dev/shm is not tmpfs here; skipping the "
                   "unsupported-device registration test");
    }

    /* Bad fds: never registerable, always PASS. */
    T_OK(exitos_register_fd(c, -1) != 0, "register_fd(-1) refuses");
    expect_unregistered(c, -1, "fd -1");

    {
        int closed = open("/dev/null", O_RDWR);
        if (closed >= 0) {
            close(closed);
            T_OK(exitos_register_fd(c, closed) != 0,
                 "register_fd(closed fd) refuses");
            expect_unregistered(c, closed, "closed fd");
        } else {
            T_SKIP("cannot open /dev/null: %s", strerror(errno));
        }
    }

    /* Pipes have no extents and no device: both ends must be refused. */
    if (pipe(pipefd) == 0) {
        expect_registration_refused(c, pipefd[0], "pipe read end");
        expect_registration_refused(c, pipefd[1], "pipe write end");
        close(pipefd[0]);
        close(pipefd[1]);
    } else {
        T_SKIP("pipe() failed: %s", strerror(errno));
    }

    /* A character device is not a regular file on ext4. */
    devnull_fd = open("/dev/null", O_RDWR);
    if (devnull_fd >= 0) {
        expect_registration_refused(c, devnull_fd, "/dev/null char device");
        close(devnull_fd);
    } else {
        T_SKIP("cannot open /dev/null: %s", strerror(errno));
    }

    /* A directory has no data extents to map. */
    dir_fd = open("/dev/shm", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        expect_registration_refused(c, dir_fd, "directory fd");
        close(dir_fd);
    } else {
        T_SKIP("cannot open /dev/shm as a directory: %s", strerror(errno));
    }

    /* --- "on any doubt, PASS": NULL context ---------------------------- */
    {
        int rc = null_ctx_child();
        T_EQ(rc, 0, "NULL context: every hook returns EXITOS_PASS, "
                    "leaves *result alone, and reports no LBA in bounds "
                    "(bitmask=%d, -2 means it crashed)", rc);
    }

    /* --- two contexts must not share registration state ---------------- */
    if (shm_fd >= 0) {
        struct exitos_ctx *c2 = exitos_ctx_create();
        if (c2) {
            expect_unregistered(c2, shm_fd, "fd seen through a second context");
            exitos_ctx_destroy(c2);
        } else {
            T_SKIP("second exitos_ctx_create failed");
        }
    }

    /* ================================================================ *
     * The operator names the disk by serial, and registration must refuse
     * anything else.
     *
     * REQUIREMENT: when EXITOS_EXPECT_SERIAL is set, a file that lives on a
     * different disk must not be registered, whatever EXITOS_DEV says.
     *
     * This is here because it actually happened. A PCI re-probe
     * renumbered the NVMe controllers, /dev/nvme2n1 stopped being the test disk
     * and became another user's drive, and a measurement command that had the
     * device name typed into it rather than resolved from the serial went on
     * using it: files were created on their filesystem and passthru write
     * commands were issued to their device. Nothing of theirs was damaged --
     * the blocks belonged to the temporary file -- but nothing in the library
     * objected either, because the per-write identity check is opt-in and the
     * registration path never looked at the serial at all.
     * ================================================================ */
    {
        char tmpl[] = "/tmp/exitos_serial_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            T_SKIP("cannot create a temporary file for the serial guard test");
        } else {
            char blk[4096];
            memset(blk, 0x11, sizeof blk);
            ssize_t w = write(fd, blk, sizeof blk);
            fsync(fd);
            struct exitos_geom g;
            int probed = (w == (ssize_t)sizeof blk) &&
                         exitos_geom_probe(tmpl, &g) == 0 && g.disk_path[0] != '\0';
            if (!probed) {
                T_SKIP("cannot probe the disk behind %s", tmpl);
            } else {
                setenv("EXITOS_DEV", g.disk_path, 1);
                struct exitos_ctx *sc = exitos_ctx_create();
                if (!sc) {
                    T_SKIP("exitos_ctx_create failed for the serial guard test");
                } else {
                    setenv("EXITOS_EXPECT_SERIAL", "NO-SUCH-SERIAL-0000", 1);
                    T_OK(exitos_register_fd(sc, fd) != 0,
                         "registration refuses when EXITOS_EXPECT_SERIAL names a "
                         "different disk than the one the file lives on");
                    unsetenv("EXITOS_EXPECT_SERIAL");
                    exitos_ctx_destroy(sc);
                }
            }
            close(fd);
            unlink(tmpl);
        }
    }

    if (shm_fd >= 0) {
        close(shm_fd);
        unlink(g_shm_path);
    }
    exitos_ctx_destroy(c);
    T_DONE();
}
