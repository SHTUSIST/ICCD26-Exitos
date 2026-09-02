/* Setup-time mapping refresh contract.
 *
 * This is a device-free white-box test.  It includes the production
 * interception core and replaces only extent discovery, fstat, and the raw
 * iopath.  The layout oracle deliberately exposes both a contiguous 8 KiB run
 * and two logically adjacent blocks at physically discontiguous LBAs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tap.h"
#include "exitos_extent.h"
#include "exitos_geom.h"
#include "exitos_intercept.h"
#include "exitos_iopath.h"
#include "exitos_maco.h"
#include "exitos_stats.h"

enum { TEST_FD = 71, BS = 4096 };
enum layout_kind {
    LAYOUT_NONE = 0,
    LAYOUT_CONTIG_8K,
    LAYOUT_SPLIT_8K,
    LAYOUT_ONE_BLOCK,
};

static enum layout_kind g_layout;
static int g_read_rc;
static int g_alias_owned_lba;
static unsigned g_load_calls;
static int g_stat_enabled;
static off_t g_stat_size;
static int g_stat_errno;
static unsigned g_raw_writes;

static int mock_extent_read_sync(int fd, struct extent_run *out, int cap,
                                 int want_sync)
{
    (void)fd;
    (void)want_sync;
    if (g_read_rc != 0)
        return g_read_rc;
    if (g_layout == LAYOUT_CONTIG_8K && cap >= 1) {
        memset(out, 0, sizeof(*out));
        out[0].file_off = 0;
        out[0].fs_block = 200;
        out[0].len = 2u * BS;
        return 1;
    }
    if (g_layout == LAYOUT_SPLIT_8K && cap >= 2) {
        memset(out, 0, 2u * sizeof(*out));
        out[0].file_off = 0;
        out[0].fs_block = 200;
        out[0].len = BS;
        out[1].file_off = BS;
        out[1].fs_block = 500;
        out[1].len = BS;
        return 2;
    }
    if (g_layout == LAYOUT_ONE_BLOCK && cap >= 1) {
        memset(out, 0, sizeof(*out));
        out[0].file_off = 0;
        out[0].fs_block = 200;
        out[0].len = BS;
        return 1;
    }
    return 0;
}

static int __attribute__((unused))
mock_extent_load_maco_sync(int fd, const struct exitos_geom *g,
                           struct maco *m, int want_sync)
{
    uint64_t second = g_alias_owned_lba ? 200u : 500u;
    int rc;

    (void)fd;
    (void)g;
    (void)want_sync;
    g_load_calls++;
    if (g_layout == LAYOUT_CONTIG_8K)
        return maco_insert(m, 0, 200, 2u * BS);
    if (g_layout == LAYOUT_SPLIT_8K) {
        rc = maco_insert(m, 0, 200, BS);
        if (rc != 0)
            return rc;
        rc = maco_insert(m, BS, second, BS);
        return rc == 0 ? 2 : rc;
    }
    if (g_layout == LAYOUT_ONE_BLOCK)
        return maco_insert(m, 0, 200, BS);
    return 0;
}

static int mock_fstat(int fd, struct stat *st)
{
    if (g_stat_enabled && fd == TEST_FD) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0600;
        st->st_size = g_stat_size;
        return 0;
    }
    errno = g_stat_errno;
    return -1;
}

static int mock_iopath_write(struct iopath *p, uint64_t lba,
                             const void *buf, size_t len)
{
    (void)p;
    (void)lba;
    (void)buf;
    (void)len;
    g_raw_writes++;
    return 0;
}

static int mock_iopath_flush(struct iopath *p)
{
    (void)p;
    return 0;
}

static void mock_iopath_close(struct iopath *p)
{
    (void)p;
}

/* Declared explicitly so the RED build fails at link time solely because the
 * new production API is absent, rather than because of an implicit prototype. */
int exitos_txn_refresh_mapping_range(struct exitos_fd_txn *tx,
                                     uint64_t off, uint64_t len);

#define exitos_extent_read_sync       mock_extent_read_sync
#define exitos_extent_load_maco_sync  mock_extent_load_maco_sync
#define iopath_write                  mock_iopath_write
#define iopath_flush                  mock_iopath_flush
#define iopath_close                  mock_iopath_close
#define fstat                         mock_fstat
#include "../../src/intercept.c"
#undef fstat
#undef iopath_close
#undef iopath_flush
#undef iopath_write
#undef exitos_extent_load_maco_sync
#undef exitos_extent_read_sync

static void reset_fixture(void)
{
    g_layout = LAYOUT_NONE;
    g_read_rc = 0;
    g_alias_owned_lba = 0;
    g_load_calls = 0;
    g_stat_enabled = 0;
    g_stat_size = 0;
    g_stat_errno = EBADF;
    g_raw_writes = 0;
}

static struct exitos_ctx *empty_registered_ctx(void)
{
    struct exitos_ctx *c = exitos_ctx_create();
    struct reg *r;

    if (!c)
        return NULL;
    c->r = calloc(1, sizeof(*c->r));
    if (!c->r) {
        exitos_ctx_destroy(c);
        return NULL;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        exitos_ctx_destroy(c);
        return NULL;
    }
    if (pthread_mutex_init(&r->mu, NULL) != 0) {
        free(r);
        exitos_ctx_destroy(c);
        return NULL;
    }
    r->active = 1;
    r->fd = TEST_FD;
    r->io = (struct iopath *)(uintptr_t)1;
    r->g.lbs = BS;
    r->g.fs_bs = BS;
    r->g.devclass = EXITOS_DEV_LOOP;
    r->g.writable_raw = 1;
    r->m = maco_create(BS);
    if (!r->m) {
        reg_release(r);
        exitos_ctx_destroy(c);
        return NULL;
    }
    c->r[0] = r;
    c->n = c->cap = 1;
    return c;
}

int main(void)
{
    static unsigned char buf[2u * BS] __attribute__((aligned(BS)));
    struct exitos_ctx *c;
    struct exitos_fd_txn tx;
    uint64_t lba = 0, contig = 0;
    uint64_t after_setup, after_write;
    ssize_t result = -1;

    memset(&tx, 0, sizeof(tx));
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), -ENOENT,
         "a token without a registration returns stable -ENOENT");

    reset_fixture();
    c = empty_registered_ctx();
    T_OK(c != NULL, "constructed an empty registered map");
    if (!c)
        T_DONE();
    T_EQ(exitos_fd_txn_begin(c, TEST_FD, &tx), 0,
         "began a held setup transaction");
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 0), -EINVAL,
         "empty range returns stable -EINVAL");
    T_EQ(exitos_txn_refresh_mapping_range(&tx, UINT64_MAX - BS + 1u, BS),
         -EOVERFLOW, "wrapping range returns stable -EOVERFLOW");
    exitos_fd_txn_end(&tx);
    T_EQ(exitos_fd_txn_begin(c, TEST_FD + 1, &tx), 0,
         "began a transaction on an unregistered fd slot");
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, BS), -ENOENT,
         "unregistered fd returns stable -ENOENT");
    exitos_fd_txn_end(&tx);
    exitos_ctx_destroy(c);

    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_CONTIG_8K;
    g_stat_errno = 0; /* adversarial syscall seam: never return false success */
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), -EIO,
         "fstat failure without errno is normalized to stable -EIO");
    exitos_fd_txn_end(&tx);
    exitos_ctx_destroy(c);

    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_CONTIG_8K;
    g_read_rc = -EIO;
    g_stat_enabled = 1;
    g_stat_size = 2u * BS;
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), -EIO,
         "refresh failure preserves its negative errno");
    exitos_fd_txn_end(&tx);
    exitos_ctx_destroy(c);

    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_ONE_BLOCK;
    g_stat_enabled = 1;
    g_stat_size = 2u * BS;
    T_EQ(maco_insert(c->r[0]->m, 0, 777, BS), 0,
         "seeded a prior safe mapping before an incomplete refresh");
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), -ENXIO,
         "a logical hole returns stable -ENXIO");
    T_EQ(maco_lookup(c->r[0]->m, 0, &lba, &contig), 0,
         "failed range proof retains the prior published mapping");
    T_EQ(lba, 777,
         "failed range proof never publishes its incomplete candidate");
    exitos_fd_txn_end(&tx);
    exitos_ctx_destroy(c);

    /* A bounds-only cross-check cannot detect this adversarial two-snapshot
     * mismatch: LBA 200 belongs to the same file, but it belongs to logical
     * block zero, not logical block one.  The correlated Maco and iv views
     * must therefore come from the one extent snapshot, not from a second
     * FIEMAP whose answer merely happens to stay inside the file footprint. */
    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_SPLIT_8K;
    g_alias_owned_lba = 1;
    g_stat_enabled = 1;
    g_stat_size = 2u * BS;
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), 0,
         "refresh succeeds from one internally consistent extent snapshot");
    T_EQ(maco_lookup(c->r[0]->m, BS, &lba, &contig), 0,
         "second logical block remains mapped after consistent refresh");
    T_EQ(lba, 500,
         "a second FIEMAP cannot alias block one onto block zero's owned LBA");
    T_EQ(g_load_calls, 0,
         "refresh does not query an independent Maco extent snapshot");
    exitos_fd_txn_end(&tx);
    exitos_ctx_destroy(c);

    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_SPLIT_8K;
    g_stat_enabled = 1;
    g_stat_size = 2u * BS;
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), 0,
         "8 KiB is fully validated across discontiguous physical extents");
    T_EQ(c->r[0]->isize, 2u * BS,
         "successful setup refresh updates cached file size");
    T_EQ(maco_lookup(c->r[0]->m, 0, &lba, &contig), 0,
         "first logical block is mapped");
    T_EQ(lba, 200, "first block uses the first physical run");
    T_EQ(maco_lookup(c->r[0]->m, BS, &lba, &contig), 0,
         "second logical block is mapped");
    T_EQ(lba, 500, "second block may use a discontiguous physical run");
    exitos_fd_txn_end(&tx);
    after_setup = exitos_stat_get(EXITOS_STAT_REFRESH);
    result = -1;
    T_EQ(exitos_on_write(c, TEST_FD, buf, sizeof(buf), 0, &result),
         EXITOS_PASS,
         "one raw command cannot cross the physical discontinuity, so the 8 KiB write passes");
    after_write = exitos_stat_get(EXITOS_STAT_REFRESH);
    T_EQ(after_write, after_setup,
         "even a split-extent first 8 KiB write does not repeat the authoritative setup refresh");
    exitos_ctx_destroy(c);

    reset_fixture();
    c = empty_registered_ctx();
    g_layout = LAYOUT_CONTIG_8K;
    g_stat_enabled = 1;
    g_stat_size = 2u * BS;
    exitos_fd_txn_begin(c, TEST_FD, &tx);
    T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 2u * BS), 0,
         "setup refresh validates a contiguous 8 KiB run");
    exitos_fd_txn_end(&tx);
    after_setup = exitos_stat_get(EXITOS_STAT_REFRESH);
    T_EQ(exitos_on_write(c, TEST_FD, buf, sizeof(buf), 0, &result),
         EXITOS_TAKEOVER,
         "first 8 KiB write after setup refresh takes the fast path");
    T_EQ(result, (ssize_t)sizeof(buf), "first 8 KiB write returns all bytes");
    after_write = exitos_stat_get(EXITOS_STAT_REFRESH);
    T_EQ(after_write, after_setup,
         "first timed 8 KiB write adds no mapping refresh");
    T_EQ(g_raw_writes, 1, "first timed write submits exactly one raw command");
    exitos_ctx_destroy(c);

    T_DONE();
}
