/* Why is the fast path not engaging?
 *
 * Written after a session where the interception layer reported success, the
 * data came out byte-correct, and the fast path had in fact never run once --
 * every write had quietly gone to the kernel. A correct checksum cannot tell
 * those apart; only the counters can. This probe walks the decision path one
 * step at a time and prints where it stops.
 *
 * Usage: probe <mountpoint>        (needs a writable ext4 mount)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include "exitos_intercept.h"
#include "exitos_stats.h"
#include "exitos_extent.h"
#include "exitos_geom.h"
#include "exitos_durability.h"

static void show_extents(const char *tag, int fd)
{
    struct extent_run r[16];
    int n = exitos_extent_read(fd, r, 16), u = 0;
    for (int i = 0; i < n; i++) if (r[i].flags & FIEMAP_EXTENT_UNWRITTEN) u++;
    printf("   %-32s %d extent(s), %d unwritten%s\n", tag, n, u,
           u ? "  <-- unwritten ranges are refused on purpose" : "");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <mountpoint>\n", argv[0]); return 2; }
    char path[512];
    size_t BS = 4096; int N = 32;
    snprintf(path, sizeof path, "%s/probe.log", argv[1]);

    struct exitos_geom g;
    printf("1. geom_probe(%s)\n", argv[1]);
    if (exitos_geom_probe(argv[1], &g) != 0) { printf("   FAILED -> this path cannot be accelerated\n"); return 0; }
    printf("   class=%s writable_raw=%d lbs=%u fs_bs=%u disk=%s\n",
           exitos_devclass_str(g.devclass), g.writable_raw, g.lbs, g.fs_bs, g.disk_path);
    if (!g.writable_raw) { printf("   -> refused by design; nothing further to check\n"); return 0; }

    printf("2. durability policy for %s: %s\n", g.disk_path,
           exitos_durability_str(exitos_durability_policy_for(g.disk_path)));

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (fallocate(fd, 0, 0, (off_t)BS * N) != 0) perror("fallocate");
    show_extents("3. after fallocate:", fd);

    void *b; if (posix_memalign(&b, 4096, BS)) return 1;
    memset(b, 0, BS);
    for (int i = 0; i < N; i++) if (pwrite(fd, b, BS, (off_t)i * BS) != (ssize_t)BS) break;
    fdatasync(fd);
    show_extents("4. after initialising:", fd);

    struct exitos_ctx *c = exitos_ctx_create();
    int rc = exitos_register_fd(c, fd);
    printf("5. register_fd -> %d (%s)\n", rc, rc == 0 ? "registered" : "declined");
    if (rc != 0) { printf("   -> registration is the blocker\n"); return 0; }

    memset(b, 0xA5, BS);
    ssize_t res = -1;
    exitos_decision d = exitos_on_write(c, fd, b, BS, 0, &res);
    printf("6. on_write -> %s\n", d == EXITOS_TAKEOVER ? "TAKEOVER" : "PASS  <-- fast path declined");
    int ir = -1;
    exitos_decision ds = exitos_on_fdatasync(c, fd, &ir);
    printf("7. on_fdatasync -> %s\n", ds == EXITOS_TAKEOVER ? "TAKEOVER" :
           "PASS (expected when any write on this fd went to the kernel)");
    printf("8. counters: FAST_WRITE=%llu FAST_SYNC=%llu PASS=%llu DECLINED=%llu\n",
        (unsigned long long)exitos_stat_get(EXITOS_STAT_FAST_WRITE),
        (unsigned long long)exitos_stat_get(EXITOS_STAT_FAST_SYNC),
        (unsigned long long)exitos_stat_get(EXITOS_STAT_PASS),
        (unsigned long long)exitos_stat_get(EXITOS_STAT_DECLINED));
    exitos_ctx_destroy(c); close(fd); unlink(path); free(b);
    return 0;
}
