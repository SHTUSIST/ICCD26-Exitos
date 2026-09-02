/* Donor files are INITIALIZED, not merely allocated: each one is a large
 * contiguous range of blocks that has been prefilled with zeros.
 *
 * That detail is what makes the whole mechanism work. src/extent.c refuses to
 * map FIEMAP_EXTENT_UNWRITTEN ranges on purpose: ext4 reads such a range back as
 * zeros until a normal write converts it, so a raw LBA write there would be
 * invisible through the filesystem. If the donor hands over unwritten extents,
 * every donated block is therefore unusable by the fast path, and the log file
 * silently falls back to the kernel for its entire life.
 *
 * So: after donation, the target's extents must carry no UNWRITTEN flag. */
#include "tap.h"
#include "exitos_donor.h"
#include "exitos_extent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include <sys/stat.h>
#include <errno.h>

int main(int argc, char **argv)
{
    /* Self-provision a loop-backed ext4 so this runs under `make integ` rather
     * than skipping forever. A test that always skips is a test that does not
     * exist. */
    char mnt[256], img[256];
    const char *dir;
    if (argc > 1) {
        dir = argv[1];
    } else {
        if (geteuid() != 0) { T_SKIP("needs root for a loop device"); printf("1..0  (0 failed)\n"); return 0; }
        snprintf(img, sizeof img, "/tmp/exitos-dinit-%d.img", (int)getpid());
        snprintf(mnt, sizeof mnt, "/tmp/exitos-dinit-mnt-%d", (int)getpid());
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
            "truncate -s 256M %s && L=$(losetup --find --show %s) && mkfs.ext4 -qF -b 4096 $L && "
            "mkdir -p %s && mount $L %s && echo $L > %s.loop", img, img, mnt, mnt, img);
        if (system(cmd) != 0) { T_OK(0, "loop fixture setup"); T_DONE(); }
        dir = mnt;
    }

    char dpath[512], tpath[512];
    snprintf(dpath, sizeof dpath, "%s/donors", dir);
    if (mkdir(dpath, 0700) != 0 && errno != EEXIST) { T_OK(0, "mkdir donors"); T_DONE(); }
    snprintf(tpath, sizeof tpath, "%s/growing.log", dir);

    struct donor_pool *p = donor_pool_create(dpath, 2, 4u << 20);
    T_OK(p != 0, "donor pool created");
    if (!p) { T_DONE(); }

    int fd = open(tpath, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    T_OK(fd >= 0, "growing log file created with O_DIRECT");

    int64_t moved = donor_extend(p, fd, 0, 1u << 20);
    T_OK(moved > 0, "donor_extend moved %lld bytes", (long long)moved);

    struct donor_prepare_report report;
    int prepared = donor_prepare_exact(p, fd, 1u << 20, 1u << 20, &report);
    T_EQ(prepared, 0, "donor_prepare_exact completed on real ext4");
    T_OK(report.prepared_bytes == (1u << 20) && report.chunks >= 1 &&
             (report.flags & (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                              DONOR_PREPARE_VERIFIED)) ==
                 (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                  DONOR_PREPARE_VERIFIED),
         "exact report records 1 MiB mutation, fdatasync, and FIEMAP verification");

    struct extent_run runs[64];
    int n = exitos_extent_read(fd, runs, 64);
    T_OK(n > 0, "donated region shows %d extent(s)", n);

    int unwritten = 0;
    for (int i = 0; i < n; i++)
        if (runs[i].flags & FIEMAP_EXTENT_UNWRITTEN) unwritten++;
    T_EQ(unwritten, 0,
         "no donated or exact-prepared extent is UNWRITTEN (the paper prefills zeros; unwritten ones "
         "are refused by extent.c and would make the fast path permanently unusable)");

    close(fd); unlink(tpath);
    donor_pool_destroy(p);
    if (argc <= 1) {
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
            "umount %s 2>/dev/null; losetup -d $(cat %s.loop 2>/dev/null) 2>/dev/null; "
            "rm -rf %s %s %s.loop", mnt, img, img, mnt, img);
        if (system(cmd) != 0) { /* cleanup is best effort */ }
    }
    T_DONE();
}
