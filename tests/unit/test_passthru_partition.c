/* NVMe passthru addresses the NAMESPACE, not the partition.
 *
 * Learned by destroying a partition table: a benchmark was told
 * EXITOS_DEV=/dev/nvme2n1p2 with EXITOS_LBA_START=0 and used the NVMe passthru
 * ioctl. pwrite() on a partition fd is partition-relative and stays inside it,
 * but NVME_IOCTL_IO_CMD carries an absolute namespace LBA and the kernel does
 * NOT clamp it to the partition. LBA 0 therefore meant sector 0 of the whole
 * disk -- the GPT -- which was overwritten with the benchmark's fill byte.
 *
 * So: passthru on a partition node must be refused unless the caller has
 * explicitly translated to namespace LBAs. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "tap.h"
#include "exitos_devguard.h"

static int actual_whole(const char *name)
{
    char path[256];
    struct stat st;
    if (strchr(name, '/')) snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "/dev/%s", name);
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode) &&
           exitos_devguard_whole_block_at("/sys", major(st.st_rdev), minor(st.st_rdev));
}

int main(void)
{
    T_EQ(exitos_devguard_passthru_ok("nvme0n1"), actual_whole("nvme0n1"),
         "whole namespace is allowed only when that actual node exists and is whole");
    T_EQ(exitos_devguard_passthru_ok("nvme0n1p1"), 0,
         "partition node: passthru would escape the partition -> refuse");
    T_EQ(exitos_devguard_passthru_ok("nvme12n3p45"), 0,
         "multi-digit partition node still recognised as a partition");
    T_EQ(exitos_devguard_passthru_ok("sda"), actual_whole("sda"),
         "whole disk name is checked against its actual device node");
    T_EQ(exitos_devguard_passthru_ok("sda3"), 0,
         "sd-style partition refused too");
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme2n1p2"), 0,
         "full path form is handled");
    T_EQ(exitos_devguard_passthru_ok(0), 0, "NULL refused");

    /* ---- the window-containment gate -------------------------------------
     * These are the refusal paths, which need no device: every one of them is a
     * way the caller could be wrong about where a raw write is about to land.
     * The accepting path needs a real partitioned disk and is exercised by the
     * Tier-2 run, not here. */
    {
        char why[256];
        T_OK(exitos_devguard_window_in_partition(NULL, "nvme0n1p1", 0, 8, 512,
                                                 why, sizeof why) != 0,
             "containment: NULL disk refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", NULL, 0, 8, 512,
                                                 why, sizeof why) != 0,
             "containment: NULL partition name refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", "nvme0n1p1", 0, 0, 512,
                                                 why, sizeof why) != 0,
             "containment: a zero-length window is refused, not vacuously accepted");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", "nvme0n1", 0, 8, 512,
                                                 why, sizeof why) != 0,
             "containment: naming the whole disk instead of a partition is refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1",
                                                 "no_such_partition_xyz", 0, 8, 512,
                                                 why, sizeof why) != 0,
             "containment: a partition that does not exist is refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", "nvme0n1p1",
                                                 UINT64_MAX, 8, 512,
                                                 why, sizeof why) != 0,
             "containment: a window whose end overflows is refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", "nvme0n1p1",
                                                 0, 8, 0, why, sizeof why) != 0,
             "containment: a zero logical block size is refused");
        T_OK(exitos_devguard_window_in_partition("/dev/nvme0n1", "nvme0n1p1",
                                                 0, 8, 999, why, sizeof why) != 0,
             "containment: a logical block size that is not a multiple of 512 is refused");
        why[0] = 0;
        (void)exitos_devguard_window_in_partition("/dev/nvme0n1", NULL, 0, 8, 512,
                                                  why, sizeof why);
        T_OK(why[0] != 0, "containment: a refusal always says why");
    }
    T_DONE();
}
