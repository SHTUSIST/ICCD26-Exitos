/* Tier-0 adversarial tests for the two safety gates: src/devguard.c and
 * src/geom.c (plus src/durability.c, whose policy is the third gate a raw
 * write passes through).  No root, no device node is ever opened, nothing is
 * written outside a /tmp directory whose name contains getpid().
 *
 * These functions decide whether a raw write happens at all, so the failure
 * that matters is not "wrong answer" but "wrong answer in the permissive
 * direction": a partition approved as a whole disk, an unprovable path
 * approved as a certified device, a refused geom that still yields an LBA.
 * Every group below states the property it defends and why it matters.
 *
 * Build (does NOT rebuild the library; a concurrent build may be running):
 *   gcc -O2 -Wall -Wextra -D_GNU_SOURCE -Iinclude -Itests/harness \
 *       tests/unit/test_guards_hard.c libexitos.a -o /tmp/t_guards_hard -lpthread
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include "exitos_devguard.h"
#include "exitos_geom.h"
#include "exitos_durability.h"
#include "tap.h"

/* A value no legal translation can produce, so "the out pointer was not
 * written" is an observable fact rather than a lucky coincidence. */
#define SENTINEL 0xDEADBEEFCAFEBABEULL

static int actual_whole(const char *name)
{
    char path[PATH_MAX];
    struct stat st;
    if (!name) return 0;
    if (strchr(name, '/')) snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "/dev/%s", name);
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode) &&
           exitos_devguard_whole_block_at("/sys", major(st.st_rdev), minor(st.st_rdev));
}

/* ================================================================== *
 * GROUP 1 - exitos_devguard_passthru_ok(): whole disk vs partition
 *
 * Property: 1 for a whole disk / namespace node, 0 for a partition node,
 * on EVERY naming scheme Linux uses.
 *
 * Why it matters (include/exitos_devguard.h, verbatim): "passthru commands
 * carry an ABSOLUTE namespace LBA and the kernel does not clamp them to the
 * partition, so an LBA that looks partition-relative lands elsewhere on the
 * disk. This exact mistake overwrote a GPT."  A 0 that should
 * be 1 costs availability; a 1 that should be 0 costs somebody's partition
 * table.  Both directions are asserted, and each assertion says which
 * direction it is guarding so a failure can be triaged without re-deriving it.
 * ================================================================== */
static void g1_passthru_names(void)
{
    /* --- whole disks: must ALLOW (returning 0 here is over-refusal) --- */
    T_EQ(exitos_devguard_passthru_ok("nvme0n1"), actual_whole("nvme0n1"),
         "passthru_ok(nvme0n1) follows the actual node and sysfs topology");
    T_EQ(exitos_devguard_passthru_ok("nvme12n3"), actual_whole("nvme12n3"),
         "passthru_ok(nvme12n3) fails closed unless the actual node is whole");
    T_EQ(exitos_devguard_passthru_ok("sda"), actual_whole("sda"),
         "passthru_ok(sda) follows the actual node and sysfs topology");
    T_EQ(exitos_devguard_passthru_ok("vdb"), actual_whole("vdb"),
         "passthru_ok(vdb) fails closed unless the actual node is whole");
    T_EQ(exitos_devguard_passthru_ok("loop0"), actual_whole("loop0"),
         "passthru_ok(loop0) follows the actual node and sysfs topology");
    T_EQ(exitos_devguard_passthru_ok("mmcblk0"), actual_whole("mmcblk0"),
         "passthru_ok(mmcblk0) fails closed unless the actual node is whole");

    /* --- partitions: must REFUSE (returning 1 here is the GPT bug) --- */
    T_EQ(exitos_devguard_passthru_ok("nvme0n1p1"), 0,
         "passthru_ok(nvme0n1p1)=0: partition, refuse");
    T_EQ(exitos_devguard_passthru_ok("nvme12n3p45"), 0,
         "passthru_ok(nvme12n3p45)=0: multi-digit partition, refuse");
    T_EQ(exitos_devguard_passthru_ok("sda3"), 0,
         "passthru_ok(sda3)=0: partition, refuse");
    T_EQ(exitos_devguard_passthru_ok("vdb12"), 0,
         "passthru_ok(vdb12)=0: two-digit partition, refuse");
    T_EQ(exitos_devguard_passthru_ok("loop0p1"), 0,
         "passthru_ok(loop0p1)=0: loop partition, refuse");
    T_EQ(exitos_devguard_passthru_ok("mmcblk0p1"), 0,
         "passthru_ok(mmcblk0p1)=0: eMMC partition, refuse");

    /* Stacked devices are never a passthru target. */
    T_EQ(exitos_devguard_passthru_ok("dm-0"), 0,
         "passthru_ok(dm-0)=0: device-mapper is never a namespace");
    T_EQ(exitos_devguard_passthru_ok("md0"), 0,
         "passthru_ok(md0)=0: md RAID is never a namespace");
}

/* ================================================================== *
 * GROUP 2 - exitos_devguard_passthru_ok(): path forms and hostile strings
 *
 * Property: the verdict is taken from the device NAME, so every way of
 * writing the same name must give the same verdict, and anything that cannot
 * be proven to be a whole disk must return 0.
 *
 * Why it matters: device paths reach this function from config files,
 * /proc/mounts and argv.  A name carrying a trailing newline (fgets) or a
 * trailing space must not be promoted from "partition" to "whole disk" - that
 * turns the exact GPT overwrite the header describes back on.
 * ================================================================== */
static void g2_passthru_paths(void)
{
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme0n1"), actual_whole("/dev/nvme0n1"),
         "passthru_ok(/dev/nvme0n1) follows the actual opened node");
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme0n1p1"), 0,
         "passthru_ok(/dev/nvme0n1p1)=0: full path, partition");
    T_EQ(exitos_devguard_passthru_ok("/dev/sda"), actual_whole("/dev/sda"),
         "passthru_ok(/dev/sda) follows the actual opened node");
    T_EQ(exitos_devguard_passthru_ok("/dev/sda3"), 0,
         "passthru_ok(/dev/sda3)=0: full path, partition");
    T_EQ(exitos_devguard_passthru_ok("/dev/loop0"), actual_whole("/dev/loop0"),
         "passthru_ok(/dev/loop0) follows the actual opened node");
    T_EQ(exitos_devguard_passthru_ok("/dev/loop0p1"), 0,
         "passthru_ok(/dev/loop0p1)=0: full path, loop partition");
    T_EQ(exitos_devguard_passthru_ok("/dev/mmcblk0"), actual_whole("/dev/mmcblk0"),
         "passthru_ok(/dev/mmcblk0) follows the actual opened node");
    T_EQ(exitos_devguard_passthru_ok("/dev/mmcblk0p1"), 0,
         "passthru_ok(/dev/mmcblk0p1)=0: full path, eMMC partition");
    T_EQ(exitos_devguard_passthru_ok("//dev//nvme0n1p1"), 0,
         "passthru_ok(//dev//nvme0n1p1)=0: doubled slashes, still a partition");

    /* Trailing slash: the basename is empty, nothing is proven -> refuse. */
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme0n1/"), 0,
         "passthru_ok(/dev/nvme0n1/)=0: trailing slash proves nothing, refuse");
    T_EQ(exitos_devguard_passthru_ok("/dev/sda3/"), 0,
         "passthru_ok(/dev/sda3/)=0: trailing slash on a partition, refuse");

    T_EQ(exitos_devguard_passthru_ok(""), 0,
         "passthru_ok(\"\")=0: empty string names no disk, refuse");
    T_EQ(exitos_devguard_passthru_ok(NULL), 0,
         "passthru_ok(NULL)=0: NULL names no disk, refuse (and must not crash)");

    /* Hostile-but-ordinary strings: a name read with fgets() keeps its '\n'. */
    T_EQ(exitos_devguard_passthru_ok("/dev/sda3\n"), 0,
         "passthru_ok(/dev/sda3 + newline)=0: a trailing newline must not "
         "promote a PARTITION to a whole disk");
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme0n1p1 "), 0,
         "passthru_ok(/dev/nvme0n1p1 + space)=0: a trailing space must not "
         "promote a PARTITION to a whole disk");
    T_EQ(exitos_devguard_passthru_ok("/dev/nvme0n1p1\t"), 0,
         "passthru_ok(/dev/nvme0n1p1 + tab)=0: trailing tab, still a partition");
}

/* ================================================================== *
 * GROUP 3 - exitos_devguard_check(): hostile inputs must not crash and must
 * never return DEVGUARD_OK for something the guard cannot prove.
 *
 * Why it matters (include/exitos_devguard.h, verbatim): "refuse to hand out a
 * raw-writable device unless it is provably the disk the operator meant [...]
 * Returns DEVGUARD_OK only when every check passes."  DEVGUARD_OK is the
 * single token that authorises a raw write.
 * ================================================================== */
static void g3_check_hostile(void)
{
    char sn[64];

    memset(sn, 0, sizeof sn);
    T_EQ(exitos_devguard_check(NULL, NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(NULL) = NOT_FOUND (no crash, no OK)");
    T_OK(exitos_devguard_check(NULL, NULL, NULL, 0) != DEVGUARD_OK,
         "check(NULL, serial_out=NULL, len=0) does not return OK and does not crash");

    /* The empty string names no block device, so the enum's own comment for
     * DEVGUARD_NOT_FOUND ("no such block device") is the correct verdict. */
    memset(sn, 0, sizeof sn);
    T_EQ(exitos_devguard_check("", NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(\"\") = NOT_FOUND: an empty string is not a block device");
    T_OK(exitos_devguard_check("", NULL, sn, sizeof sn) != DEVGUARD_OK,
         "check(\"\") is at least not OK (fails closed)");

    /* A path that does not exist anywhere. */
    char nope[128];
    snprintf(nope, sizeof nope, "/dev/exitos_no_such_dev_%d", (int)getpid());
    T_EQ(exitos_devguard_check(nope, NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(nonexistent device path) = NOT_FOUND");

    /* A directory that is not device-named. */
    T_EQ(exitos_devguard_check("/tmp", NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(/tmp) = NOT_FOUND: a directory is not a block device");
    T_EQ(exitos_devguard_check("/", NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(/) = NOT_FOUND: the root directory is not a block device");

    /* A regular file that is not device-named. */
    T_EQ(exitos_devguard_check("/etc/hostname", NULL, sn, sizeof sn), DEVGUARD_NOT_FOUND,
         "check(/etc/hostname) = NOT_FOUND: a regular file is not a block device");

    /* Path traversal in the middle of the path. */
    T_OK(exitos_devguard_check("/dev/../etc/passwd", NULL, sn, sizeof sn) != DEVGUARD_OK,
         "check(/dev/../etc/passwd) is not OK");
    T_OK(exitos_devguard_check("../../../../dev/sda", NULL, sn, sizeof sn) != DEVGUARD_OK,
         "check(../../../../dev/sda) is not OK: sda is partitioned here");
    T_OK(exitos_devguard_check("/dev/sda/../sda", NULL, sn, sizeof sn) != DEVGUARD_OK,
         "check(/dev/sda/../sda) is not OK");

    /* A pathologically long name must not overflow the 256-byte sysfs buffer. */
    char big[4096];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    memcpy(big, "/dev/", 5);
    T_OK(exitos_devguard_check(big, NULL, sn, sizeof sn) != DEVGUARD_OK,
         "check(4KB device name) is not OK and does not overflow");
}

/* Find a block device that this guard currently certifies (returns
 * DEVGUARD_OK) using only its bare kernel name.  `want` selects which kind of
 * identity it must report: 0 = any, 1 = a synthetic "(loop:...)" identity,
 * 2 = a real hardware serial/wwid.  Returns 1 on success.  Read-only: the
 * device itself is never opened. */
#define CERT_ANY 0
#define CERT_LOOP 1
#define CERT_HW 2
static int find_certified_dev_kind(char *name, size_t n, int want)
{
    DIR *d = opendir("/sys/class/block");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    char sn[64];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (strlen(e->d_name) >= n)
            continue;
        sn[0] = '\0';
        if (exitos_devguard_check(e->d_name, NULL, sn, sizeof sn) != DEVGUARD_OK)
            continue;
        int is_loop = (strncmp(sn, "(loop:", 6) == 0);
        if (want == CERT_LOOP && !is_loop)
            continue;
        if (want == CERT_HW && (is_loop || sn[0] == '\0'))
            continue;
        snprintf(name, n, "%s", e->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

static int find_certified_dev(char *name, size_t n)
{
    return find_certified_dev_kind(name, n, CERT_ANY);
}

/* ================================================================== *
 * GROUP 4 - exitos_devguard_check(): the verdict must describe the object at
 * dev_path, not merely a device that happens to share its basename.
 *
 * Property: a path that is a regular file or a directory must never be
 * certified, whatever it is called.
 *
 * Why it matters: the header promises the device is "provably the disk the
 * operator meant".  If only the basename is consulted, then a typo, a symlink
 * farm, or a stale path under a different directory is certified as a real
 * disk, and the pin/serial machinery downstream is reasoning about a device
 * that the caller never opens.  Nothing here opens or writes a device: the
 * decoys are plain files under /tmp/<name>_<pid>/.
 * ================================================================== */
static void g4_check_basename_only(const char *tmpdir)
{
    char devname[64];
    if (!find_certified_dev(devname, sizeof devname)) {
        T_SKIP("no block device on this host currently returns DEVGUARD_OK; "
               "cannot build a same-basename decoy");
        return;
    }
    printf("# using certified device name '%s' as the decoy basename\n", devname);

    char fdir[256], ddir[256], fpath[512], dpath[512];
    snprintf(fdir, sizeof fdir, "%s/f", tmpdir);
    snprintf(ddir, sizeof ddir, "%s/d", tmpdir);
    if (mkdir(fdir, 0700) != 0 || mkdir(ddir, 0700) != 0) {
        T_SKIP("cannot create decoy directories under %s", tmpdir);
        return;
    }
    snprintf(fpath, sizeof fpath, "%s/%s", fdir, devname);
    snprintf(dpath, sizeof dpath, "%s/%s", ddir, devname);

    FILE *f = fopen(fpath, "w");
    if (!f) {
        T_SKIP("cannot create decoy file %s", fpath);
        return;
    }
    fputs("not a device\n", f);
    fclose(f);
    if (mkdir(dpath, 0700) != 0) {
        T_SKIP("cannot create decoy directory %s", dpath);
        unlink(fpath);
        return;
    }

    /* Control: the real name is certified, so the decoys differ from it only
     * in that they are not the device. */
    T_EQ(exitos_devguard_check(devname, NULL, NULL, 0), DEVGUARD_OK,
         "control: check(%s) = OK for the real device name", devname);

    devguard_verdict vf = exitos_devguard_check(fpath, NULL, NULL, 0);
    T_OK(vf != DEVGUARD_OK,
         "check(%s) must NOT be OK: that path is a REGULAR FILE, not the block "
         "device (got '%s')", fpath, exitos_devguard_str(vf));

    devguard_verdict vd = exitos_devguard_check(dpath, NULL, NULL, 0);
    T_OK(vd != DEVGUARD_OK,
         "check(%s) must NOT be OK: that path is a DIRECTORY, not the block "
         "device (got '%s')", dpath, exitos_devguard_str(vd));

    /* Same trick without any decoy on disk at all: a path under /nonexistent
     * that merely ends in the device's name. */
    char ghost[256];
    snprintf(ghost, sizeof ghost, "/nonexistent_%d/%s", (int)getpid(), devname);
    devguard_verdict vg = exitos_devguard_check(ghost, NULL, NULL, 0);
    T_OK(vg != DEVGUARD_OK,
         "check(%s) must NOT be OK: nothing exists at that path (got '%s')",
         ghost, exitos_devguard_str(vg));

    /* Trailing slash on a certified device name: the basename is empty, so
     * nothing has been proven and the answer must not be OK. */
    char slashed[128];
    snprintf(slashed, sizeof slashed, "/dev/%s/", devname);
    T_OK(exitos_devguard_check(slashed, NULL, NULL, 0) != DEVGUARD_OK,
         "check(%s) is not OK", slashed);

    /* serial_out must never be written past serial_len. */
    struct { char buf[8]; unsigned char canary[8]; } s;
    memset(&s, 0xAA, sizeof s);
    exitos_devguard_check(devname, NULL, s.buf, sizeof s.buf);
    int canary_ok = 1;
    for (unsigned i = 0; i < sizeof s.canary; i++)
        if (s.canary[i] != 0xAA)
            canary_ok = 0;
    T_OK(canary_ok, "check(%s) with an 8-byte serial buffer does not write past it",
         devname);
    T_OK(memchr(s.buf, '\0', sizeof s.buf) != NULL,
         "check(%s) NUL-terminates a short serial buffer", devname);
    /* serial_len == 0 must be treated as "no buffer". */
    memset(&s, 0xAA, sizeof s);
    exitos_devguard_check(devname, NULL, s.buf, 0);
    canary_ok = 1;
    for (unsigned i = 0; i < sizeof s.buf; i++)
        if ((unsigned char)s.buf[i] != 0xAA)
            canary_ok = 0;
    T_OK(canary_ok, "check(%s) with serial_len=0 writes nothing at all", devname);

    rmdir(dpath);
    unlink(fpath);
    rmdir(ddir);
    rmdir(fdir);
}

/* Find a device the guard REFUSES because it is partitioned, and one of that
 * device's own partition nodes.  Returns 1 when both were found. */
static int find_partitioned_pair(char *disk, size_t dn, char *part, size_t pn)
{
    DIR *d = opendir("/sys/class/block");
    if (!d)
        return 0;
    struct dirent *e;
    int have_disk = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= dn)
            continue;
        if (exitos_devguard_check(e->d_name, NULL, NULL, 0) == DEVGUARD_HAS_PARTITIONS) {
            snprintf(disk, dn, "%s", e->d_name);
            have_disk = 1;
            break;
        }
    }
    closedir(d);
    if (!have_disk)
        return 0;

    d = opendir("/sys/class/block");
    if (!d)
        return 0;
    size_t dl = strlen(disk);
    int have_part = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= pn)
            continue;
        if (strncmp(e->d_name, disk, dl) != 0 || strlen(e->d_name) <= dl)
            continue;
        char pf[512];
        snprintf(pf, sizeof pf, "/sys/class/block/%s/partition", e->d_name);
        if (access(pf, F_OK) != 0)
            continue;
        if (exitos_devguard_check(e->d_name, NULL, NULL, 0) == DEVGUARD_OK) {
            snprintf(part, pn, "%s", e->d_name);   /* the interesting case */
            have_part = 1;
            break;
        }
        if (!have_part) {
            snprintf(part, pn, "%s", e->d_name);
            have_part = 1;
        }
    }
    closedir(d);
    return have_part;
}

/* ================================================================== *
 * GROUP 4b - naming a PARTITION must not launder away the refusal that the
 * whole disk earned.
 *
 * Property: if exitos_devguard_check() refuses a disk with
 * DEVGUARD_HAS_PARTITIONS ("partitioned -> almost certainly someone's data"),
 * then it must not answer DEVGUARD_OK for a partition node of that same disk.
 *
 * Why it matters: the partition IS the someone's data the whole-disk refusal
 * was protecting.  has_partitions() only ever looks for children, and a
 * partition has none, so the strongest refusal in the module is bypassed by a
 * one-character typo - "nvme0n1p1" instead of "nvme0n1".  That typo is
 * exactly the accident the header describes, only one step
 * earlier: the identity check hands back the PARENT disk's serial (see
 * src/devguard.c: "the identity lives on the parent disk"), so a caller that
 * pinned the whole disk's serial is told this is the disk it meant.
 * Nothing here opens or writes any device: only sysfs is read.
 * ================================================================== */
static void g4b_partition_escalation(void)
{
    char disk[64], part[64], sn[64];
    if (!find_partitioned_pair(disk, sizeof disk, part, sizeof part)) {
        T_SKIP("no partitioned disk with a partition node on this host");
        return;
    }
    devguard_verdict vdisk = exitos_devguard_check(disk, NULL, NULL, 0);
    sn[0] = '\0';
    devguard_verdict vpart = exitos_devguard_check(part, NULL, sn, sizeof sn);
    printf("# whole disk '%s' -> '%s'; partition '%s' -> '%s' (serial '%s')\n",
           disk, exitos_devguard_str(vdisk), part, exitos_devguard_str(vpart), sn);

    T_EQ(vdisk, DEVGUARD_HAS_PARTITIONS,
         "control: check(%s) refuses the partitioned whole disk", disk);
    T_OK(vpart != DEVGUARD_OK,
         "check(%s) must NOT be OK: it is a partition of %s, which was just "
         "refused as PARTITIONED; a partition is the data that refusal protects "
         "(got '%s')", part, disk, exitos_devguard_str(vpart));

    char full[128];
    snprintf(full, sizeof full, "/dev/%s", part);
    T_OK(exitos_devguard_check(full, NULL, NULL, 0) != DEVGUARD_OK,
         "check(%s) must NOT be OK either (same device, full path)", full);

    /* And the gate that exists because of the GPT overwrite must agree with
     * the verdict: nothing that passthru_ok() rejects should be certified. */
    if (vpart == DEVGUARD_OK)
        T_OK(exitos_devguard_passthru_ok(full) == 1,
             "check(%s)=OK yet passthru_ok=%d: the two gates disagree about the "
             "same device", full, exitos_devguard_passthru_ok(full));
    else
        T_OK(1, "check(%s) refused, so the two gates cannot disagree", full);

    /* The two assertions below replace the ones that used to run only when a
     * partition was certified. A partition is now refused by name with its own
     * verdict, so that branch is unreachable and the property it defended --
     * the two gates agreeing about one device -- would otherwise stop being
     * checked at all. Stated directly instead: both gates refuse, and the
     * refusal names the reason rather than being an accident of some other
     * check happening to fire first. */
    T_OK(vpart == DEVGUARD_IS_PARTITION || vpart == DEVGUARD_MOUNTED,
         "check(%s) refuses a partition with a verdict that names a reason true "
         "of it (got '%s'); a mounted partition reports MOUNTED first, which is "
         "the more urgent of the two and is equally a refusal",
         part, exitos_devguard_str(vpart));
    T_EQ(exitos_devguard_passthru_ok(full), 0,
         "passthru_ok(%s) refuses the same partition: a passthru LBA is "
         "namespace-absolute and the kernel does not clamp it to the partition",
         full);
}

/* ================================================================== *
 * GROUP 4c - expect_serial must be an identity test, not a hint.
 *
 * Property (include/exitos_devguard.h, verbatim): "Identity must be the
 * serial", DEVGUARD_SERIAL_MISMATCH means "not the disk the operator named",
 * and "Returns DEVGUARD_OK only when every check passes."
 *
 * Why it matters: the operator's ONLY defence against the kernel renaming
 * disks across a reboot (nvme3n1 becoming the root disk, per the header) is
 * the serial they pass in.  If that comparison is a substring test, a short or
 * truncated expectation matches a disk it was never meant to match, and the
 * defence silently degrades to "some disk whose serial contains these
 * characters".
 * ================================================================== */
static void g4c_serial_matching(void)
{
    char name[64], sn[64];
    if (!find_certified_dev_kind(name, sizeof name, CERT_HW)) {
        T_SKIP("no certified device with a hardware serial/wwid on this host");
        return;
    }
    sn[0] = '\0';
    exitos_devguard_check(name, NULL, sn, sizeof sn);
    printf("# serial-matching tests use device '%s', serial '%s'\n", name, sn);

    T_EQ(exitos_devguard_check(name, sn, NULL, 0), DEVGUARD_OK,
         "check(%s, exact serial) = OK", name);
    T_EQ(exitos_devguard_check(name, "ZZZZ-no-such-serial", NULL, 0),
         DEVGUARD_SERIAL_MISMATCH,
         "check(%s, a serial that is not this disk) = SERIAL_MISMATCH", name);

    /* A one-character expectation is not an identity: it matches this disk and
     * very nearly every other disk in the world. */
    if (strlen(sn) > 3) {
        char one[2] = { sn[0], '\0' };
        T_OK(exitos_devguard_check(name, one, NULL, 0) != DEVGUARD_OK,
             "check(%s, expect_serial=\"%s\" - ONE character) must NOT be OK: "
             "identity is the serial, and a single character is not one", name, one);
        char half[64];
        snprintf(half, sizeof half, "%.*s", (int)(strlen(sn) / 2), sn);
        T_OK(exitos_devguard_check(name, half, NULL, 0) != DEVGUARD_OK,
             "check(%s, expect_serial=\"%s\" - half the serial, truncated) must "
             "NOT be OK", name, half);
        /* An expectation the disk does not contain at all must be refused even
         * when it shares a prefix with the real serial. */
        char wrong[80];
        snprintf(wrong, sizeof wrong, "%sZZZ", sn);
        T_EQ(exitos_devguard_check(name, wrong, NULL, 0), DEVGUARD_SERIAL_MISMATCH,
             "check(%s, real serial + suffix) = SERIAL_MISMATCH", name);
    } else {
        T_SKIP("serial too short for a substring test");
    }
}

/* ================================================================== *
 * GROUP 4d - a loop device must not be certified against somebody else's
 * expected serial.
 *
 * Property (include/exitos_devguard.h, verbatim): "Returns DEVGUARD_OK only
 * when every check passes", and DEVGUARD_SERIAL_MISMATCH exists for "not the
 * disk the operator named".
 *
 * Why it matters: an operator who pins a run to the serial of a real disk and
 * accidentally points the tool at a loop device is told OK - the one answer
 * that authorises raw writes - even though the expectation they passed was
 * never compared against anything.
 * ================================================================== */
static void g4d_loop_ignores_expectation(void)
{
    char name[64], sn[64];
    if (!find_certified_dev_kind(name, sizeof name, CERT_LOOP)) {
        T_SKIP("no certified loop device on this host");
        return;
    }
    sn[0] = '\0';
    exitos_devguard_check(name, NULL, sn, sizeof sn);
    printf("# loop expectation test uses '%s', reported identity '%s'\n", name, sn);

    T_EQ(exitos_devguard_check(name, sn, NULL, 0), DEVGUARD_OK,
         "check(%s, its own reported identity) = OK", name);
    T_OK(exitos_devguard_check(name, "naa.0000000000000000", NULL, 0) != DEVGUARD_OK,
         "check(%s, expect_serial of a DIFFERENT disk) must NOT be OK: the "
         "operator named another device", name);
    T_OK(exitos_devguard_check(name, "ZZZZ-no-such-serial", NULL, 0) != DEVGUARD_OK,
         "check(%s, expect_serial that matches nothing) must NOT be OK", name);
}

/* ================================================================== *
 * GROUP 5 - verdict/class label functions.
 *
 * Property: never NULL (they are printed straight into operator-facing
 * refusal messages), distinct for every enum value, and safe for values
 * outside the enum range.
 *
 * Why it matters: two verdicts sharing a label makes a refusal reason
 * unreadable, and a NULL return turns a refusal into a segfault in the very
 * error path that was supposed to protect the disk.  An out-of-range value
 * must NOT be labelled with one of the allowed labels - src/geom.c says so
 * itself: "never claim it is writable by returning one of the allowed labels".
 * ================================================================== */
static void g5_label_functions(void)
{
    const devguard_verdict vs[] = {
        DEVGUARD_OK, DEVGUARD_NOT_FOUND, DEVGUARD_HAS_PARTITIONS,
        DEVGUARD_MOUNTED, DEVGUARD_SERIAL_MISMATCH, DEVGUARD_NO_SERIAL,
        (devguard_verdict)(DEVGUARD_NO_SERIAL + 1), (devguard_verdict)99,
        (devguard_verdict)-1,
    };
    const unsigned nv = sizeof vs / sizeof vs[0];
    unsigned i, j;
    int all_nonnull = 1, in_range_distinct = 1, oob_not_aliased = 1;

    for (i = 0; i < nv; i++)
        if (exitos_devguard_str(vs[i]) == NULL)
            all_nonnull = 0;
    T_OK(all_nonnull, "devguard_str: non-NULL for all 6 enum values and 3 out-of-range values");

    for (i = 0; i < 6 && in_range_distinct; i++)
        for (j = i + 1; j < 6; j++)
            if (strcmp(exitos_devguard_str(vs[i]), exitos_devguard_str(vs[j])) == 0)
                in_range_distinct = 0;
    T_OK(in_range_distinct, "devguard_str: all 6 in-range verdicts have distinct labels");

    for (i = 6; i < nv; i++)
        for (j = 0; j < 6; j++)
            if (strcmp(exitos_devguard_str(vs[i]), exitos_devguard_str(vs[j])) == 0)
                oob_not_aliased = 0;
    T_OK(oob_not_aliased,
         "devguard_str: an out-of-range verdict is not labelled as a real verdict "
         "(it must not read as 'OK')");
    T_OK(strcmp(exitos_devguard_str(DEVGUARD_OK), "OK") == 0,
         "devguard_str(DEVGUARD_OK) is the OK label");

    const exitos_dev_class cs[] = {
        EXITOS_DEV_OK, EXITOS_DEV_DM, EXITOS_DEV_MD, EXITOS_DEV_LOOP,
        EXITOS_DEV_UNKNOWN,
        (exitos_dev_class)(EXITOS_DEV_UNKNOWN + 1), (exitos_dev_class)77,
        (exitos_dev_class)-5,
    };
    const unsigned nc = sizeof cs / sizeof cs[0];
    all_nonnull = 1; in_range_distinct = 1; oob_not_aliased = 1;

    for (i = 0; i < nc; i++)
        if (exitos_devclass_str(cs[i]) == NULL)
            all_nonnull = 0;
    T_OK(all_nonnull, "devclass_str: non-NULL for all 5 enum values and 3 out-of-range values");

    for (i = 0; i < 5 && in_range_distinct; i++)
        for (j = i + 1; j < 5; j++)
            if (strcmp(exitos_devclass_str(cs[i]), exitos_devclass_str(cs[j])) == 0)
                in_range_distinct = 0;
    T_OK(in_range_distinct, "devclass_str: all 5 in-range classes have distinct labels");

    for (i = 5; i < nc; i++)
        for (j = 0; j < 5; j++)
            if (strcmp(exitos_devclass_str(cs[i]), exitos_devclass_str(cs[j])) == 0)
                oob_not_aliased = 0;
    T_OK(oob_not_aliased,
         "devclass_str: an out-of-range class is not labelled EXITOS_DEV_OK or "
         "EXITOS_DEV_LOOP (the two classes that authorise a raw write)");
}

/* ------------------------------------------------------------------ */
static struct exitos_geom mkgeom(exitos_dev_class c, int writable_raw,
                                 uint64_t start, uint32_t fs_bs, uint32_t lbs)
{
    struct exitos_geom g;
    memset(&g, 0, sizeof g);
    snprintf(g.part_name, sizeof g.part_name, "%s", "nvme0n1p1");
    snprintf(g.disk_path, sizeof g.disk_path, "%s", "/dev/nvme0n1");
    g.part_start_sect = start;
    g.fs_bs = fs_bs;
    g.lbs = lbs;
    g.devclass = c;
    g.writable_raw = writable_raw;
    return g;
}

/* ================================================================== *
 * GROUP 6 - exitos_geom_fsblock_to_lba(): the arithmetic itself.
 *
 * Property (include/exitos_geom.h, verbatim):
 *     lba = part_start_sect * 512 / lbs + fs_block * (fs_bs / lbs)
 *
 * Why it matters: this number is fed to a raw write.  An off-by-one in the
 * partition offset writes one sector before the file; a wrong scale factor
 * writes megabytes away from it, onto data the filesystem believes it owns.
 * The expected values below are computed by hand, not by re-running the
 * implementation's own expression.
 * ================================================================== */
static void expect_lba(const char *what, const struct exitos_geom *g,
                       uint64_t fs_block, uint64_t want)
{
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(g, fs_block, &lba);
    T_EQ(rc, 0, "%s: blk=%llu returns 0 (got %d)", what,
         (unsigned long long)fs_block, rc);
    T_OK(lba == want, "%s: blk=%llu -> lba=%llu (got %llu)", what,
         (unsigned long long)fs_block, (unsigned long long)want,
         (unsigned long long)lba);
}

static void g6_arithmetic(void)
{
    /* 4K fs block on a 512B-sector disk, partition starting at 2048. */
    struct exitos_geom a = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    expect_lba("start=2048 fs_bs=4096 lbs=512", &a, 0,        2048);
    expect_lba("start=2048 fs_bs=4096 lbs=512", &a, 1,        2056);
    expect_lba("start=2048 fs_bs=4096 lbs=512", &a, 100,      2848);
    expect_lba("start=2048 fs_bs=4096 lbs=512", &a, 1000000,  8002048ULL);

    /* fs_bs == lbs: the filesystem-block factor is 1. The 272-sector sysfs
     * start is 34 native 4 KiB LBAs. */
    struct exitos_geom b = mkgeom(EXITOS_DEV_OK, 1, 272, 4096, 4096);
    expect_lba("fs_bs == lbs == 4096, start=272 sectors", &b, 0,   34);
    expect_lba("fs_bs == lbs == 4096, start=272 sectors", &b, 1,   35);
    expect_lba("fs_bs == lbs == 4096, start=272 sectors", &b, 999, 1033);

    struct exitos_geom c = mkgeom(EXITOS_DEV_LOOP, 1, 0, 512, 512);
    expect_lba("loop, fs_bs == lbs == 512, start=0", &c, 0,    0);
    expect_lba("loop, fs_bs == lbs == 512, start=0", &c, 4095, 4095);

    /* 1K fs block (mke2fs -b 1024) on a 512B disk: factor 2. */
    struct exitos_geom d = mkgeom(EXITOS_DEV_OK, 1, 63, 1024, 512);
    expect_lba("start=63 fs_bs=1024 lbs=512", &d, 0, 63);
    expect_lba("start=63 fs_bs=1024 lbs=512", &d, 7, 77);

    /* 64K fs block on a 4K-native disk: factor 16. */
    struct exitos_geom e = mkgeom(EXITOS_DEV_OK, 1, 256, 65536, 4096);
    expect_lba("start=256 sectors fs_bs=65536 lbs=4096", &e, 0,   32);
    expect_lba("start=256 sectors fs_bs=65536 lbs=4096", &e, 3,   80);
    expect_lba("start=256 sectors fs_bs=65536 lbs=4096", &e, 512, 8224);

    /* A loop device: whole device, no partition offset. */
    struct exitos_geom l = mkgeom(EXITOS_DEV_LOOP, 1, 0, 4096, 512);
    expect_lba("loop, start=0 fs_bs=4096 lbs=512", &l, 0,   0);
    expect_lba("loop, start=0 fs_bs=4096 lbs=512", &l, 123, 984);

    /* Non-power-of-two but exact ratio (520-byte-sector drives exist). */
    struct exitos_geom f = mkgeom(EXITOS_DEV_OK, 1, 65, 4160, 520);
    expect_lba("start=65 sectors fs_bs=4160 lbs=520", &f, 5, 104);

    /* fs_bs < lbs is not expressible as a whole number of device blocks: the
     * translation would silently address the wrong place, so it must refuse
     * rather than round.  -EINVAL is the code's own answer for an
     * inexpressible geometry; the load-bearing part is "not 0". */
    struct exitos_geom bad = mkgeom(EXITOS_DEV_OK, 1, 2048, 512, 4096);
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(&bad, 1, &lba);
    T_OK(rc != 0, "fs_bs=512 < lbs=4096 must NOT translate (got rc=%d)", rc);
    T_OK(lba == SENTINEL,
         "fs_bs < lbs: out pointer untouched (still sentinel)");

    struct exitos_geom bad2 = mkgeom(EXITOS_DEV_OK, 1, 2048, 4608, 4096);
    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&bad2, 1, &lba);
    T_OK(rc != 0, "fs_bs=4608 not a multiple of lbs=4096 must NOT translate (got rc=%d)", rc);
    T_OK(lba == SENTINEL, "non-multiple geometry: out pointer untouched");
}

/* ================================================================== *
 * GROUP 7 - overflow.
 *
 * Property (src/geom.c, verbatim): "Wrapping past 2^64 would produce a small,
 * plausible-looking LBA that points at the start of the device. Refuse
 * instead."  The start of the device is where the partition table lives, so a
 * wrapped LBA is the worst possible wrong answer.
 * ================================================================== */
static void g7_overflow(void)
{
    struct exitos_geom g = mkgeom(EXITOS_DEV_OK, 1, 0, 4096, 512); /* factor 8 */
    uint64_t lba, want;
    int rc;

    /* Largest block that still fits: UINT64_MAX/8, start 0. */
    lba = SENTINEL;
    want = (UINT64_MAX / 8) * 8;
    rc = exitos_geom_fsblock_to_lba(&g, UINT64_MAX / 8, &lba);
    T_EQ(rc, 0, "factor=8 start=0: blk=UINT64_MAX/8 is representable (rc=%d)", rc);
    T_OK(lba == want, "factor=8 start=0: blk=UINT64_MAX/8 -> %llu (got %llu)",
         (unsigned long long)want, (unsigned long long)lba);

    /* One more block wraps: must refuse and must not write the out pointer. */
    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&g, UINT64_MAX / 8 + 1, &lba);
    T_OK(rc != 0, "factor=8: blk=UINT64_MAX/8+1 must be refused (got rc=%d)", rc);
    T_OK(lba == SENTINEL,
         "overflowing multiply: out pointer untouched - a wrapped LBA points at "
         "the partition table");

    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&g, UINT64_MAX, &lba);
    T_OK(rc != 0, "factor=8: blk=UINT64_MAX must be refused (got rc=%d)", rc);
    T_OK(lba == SENTINEL, "blk=UINT64_MAX: out pointer untouched");

    /* Overflow contributed by part_start_sect rather than by the multiply. */
    struct exitos_geom h = mkgeom(EXITOS_DEV_OK, 1, UINT64_MAX - 10, 512, 512);
    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&h, 10, &lba);
    T_EQ(rc, 0, "start=UINT64_MAX-10 factor=1: blk=10 lands exactly on UINT64_MAX (rc=%d)", rc);
    T_OK(lba == UINT64_MAX, "start=UINT64_MAX-10: blk=10 -> UINT64_MAX (got %llu)",
         (unsigned long long)lba);

    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&h, 11, &lba);
    T_OK(rc != 0, "start=UINT64_MAX-10: blk=11 overflows the ADD, must refuse (got %d)", rc);
    T_OK(lba == SENTINEL, "additive overflow: out pointer untouched");

    /* Factor 1 with start 0: every block is representable, nothing to refuse. */
    struct exitos_geom i = mkgeom(EXITOS_DEV_OK, 1, 0, 512, 512);
    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&i, UINT64_MAX, &lba);
    T_EQ(rc, 0, "factor=1 start=0: blk=UINT64_MAX is representable (rc=%d)", rc);
    T_OK(lba == UINT64_MAX, "factor=1 start=0: blk=UINT64_MAX -> UINT64_MAX (got %llu)",
         (unsigned long long)lba);
}

/* ================================================================== *
 * GROUP 8 - the refusal path.
 *
 * Property (include/exitos_geom.h, verbatim): "Returns -EPERM when
 * geom->writable_raw == 0", and (src/geom.c) "the caller's output variable is
 * left untouched so that a caller which ignores the return code cannot
 * silently use a stale or half-written LBA."
 *
 * Why it matters: this is the whole defence for LVM / dm-crypt / md.  A
 * caller that checks only *lba must be unable to obtain a plausible number.
 * ================================================================== */
static void expect_perm(const char *what, const struct exitos_geom *g, uint64_t blk)
{
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(g, blk, &lba);
    T_EQ(rc, -EPERM, "%s: refuses with -EPERM (got %d)", what, rc);
    T_OK(lba == SENTINEL, "%s: out pointer still holds the sentinel", what);
}

static void g8_refusal(void)
{
    struct exitos_geom dm = mkgeom(EXITOS_DEV_DM, 0, 2048, 4096, 512);
    expect_perm("DM, writable_raw=0", &dm, 42);
    struct exitos_geom md = mkgeom(EXITOS_DEV_MD, 0, 2048, 4096, 512);
    expect_perm("MD, writable_raw=0", &md, 42);
    struct exitos_geom un = mkgeom(EXITOS_DEV_UNKNOWN, 0, 2048, 4096, 512);
    expect_perm("UNKNOWN, writable_raw=0", &un, 42);

    /* blk=0 is the tempting case: 0*factor is 0 whatever the geometry, so an
     * implementation that computed first and checked later would still emit
     * part_start_sect here. */
    expect_perm("DM, blk=0", &dm, 0);

    /* An inconsistent struct (someone set writable_raw by hand): the class is
     * the header's invariant, so it must be enforced independently. */
    struct exitos_geom lying = mkgeom(EXITOS_DEV_DM, 1, 2048, 4096, 512);
    expect_perm("DM but writable_raw=1 (inconsistent struct)", &lying, 7);
    struct exitos_geom lying2 = mkgeom(EXITOS_DEV_UNKNOWN, 1, 2048, 4096, 512);
    expect_perm("UNKNOWN but writable_raw=1", &lying2, 7);
    struct exitos_geom oob = mkgeom((exitos_dev_class)42, 1, 2048, 4096, 512);
    expect_perm("class out of enum range but writable_raw=1", &oob, 7);

    /* writable_raw is a flag with one permitted true value; anything else is
     * not a grant. */
    struct exitos_geom two = mkgeom(EXITOS_DEV_OK, 2, 2048, 4096, 512);
    expect_perm("writable_raw=2 (not the documented 1)", &two, 7);
    struct exitos_geom neg = mkgeom(EXITOS_DEV_OK, -1, 2048, 4096, 512);
    expect_perm("writable_raw=-1", &neg, 7);

    /* The permission gate must come before the geometry validation: a refused
     * geom legitimately carries lbs=0, and the caller must still be told
     * -EPERM (why it was refused), not -EINVAL (that its numbers were odd). */
    struct exitos_geom zero = mkgeom(EXITOS_DEV_UNKNOWN, 0, 0, 0, 0);
    expect_perm("refused geom with lbs=0 and fs_bs=0", &zero, 1);

    /* NULL arguments: no crash, no translation. */
    uint64_t lba = SENTINEL;
    T_EQ(exitos_geom_fsblock_to_lba(NULL, 1, &lba), -EINVAL,
         "fsblock_to_lba(NULL geom) = -EINVAL");
    T_OK(lba == SENTINEL, "NULL geom: out pointer untouched");
    struct exitos_geom ok = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    T_EQ(exitos_geom_fsblock_to_lba(&ok, 1, NULL), -EINVAL,
         "fsblock_to_lba(NULL out) = -EINVAL (must not crash)");

    /* A zeroed struct is what a caller gets from a failed probe. */
    struct exitos_geom zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(&zeroed, 1, &lba);
    T_OK(rc != 0, "all-zero geom (failed probe) must not translate (got %d)", rc);
    T_OK(lba == SENTINEL, "all-zero geom: out pointer untouched");
}

/* ================================================================== *
 * GROUP 9 - exitos_geom_probe() invariants (read-only: stat/statvfs/sysfs).
 *
 * Property (include/exitos_geom.h, verbatim): "Never returns writable_raw=1
 * for DM/MD/UNKNOWN."  Also, a grant is worthless unless the geometry can
 * express the translation, so writable_raw=1 must imply a usable lbs/fs_bs.
 * ================================================================== */
static void g9_probe_invariants(void)
{
    struct exitos_geom g;

    T_EQ(exitos_geom_probe(NULL, &g), -EINVAL, "probe(NULL path) = -EINVAL");
    T_EQ(exitos_geom_probe("/tmp", NULL), -EINVAL, "probe(NULL out) = -EINVAL (no crash)");
    T_EQ(exitos_geom_probe("", &g), -EINVAL, "probe(\"\") = -EINVAL");
    T_OK(g.writable_raw == 0 && g.devclass == EXITOS_DEV_UNKNOWN,
         "probe(\"\") leaves a refusing geom behind");

    char nope[128];
    snprintf(nope, sizeof nope, "/no_such_path_%d", (int)getpid());
    memset(&g, 0xFF, sizeof g);
    T_EQ(exitos_geom_probe(nope, &g), -ENOENT, "probe(nonexistent path) = -ENOENT");
    T_OK(g.writable_raw == 0 && g.devclass == EXITOS_DEV_UNKNOWN,
         "probe(nonexistent) overwrites the caller's struct with a refusing one");

    /* Pseudo-filesystems have no block device behind them: nothing to write. */
    memset(&g, 0xFF, sizeof g);
    if (exitos_geom_probe("/proc/self", &g) == 0) {
        T_EQ(g.writable_raw, 0, "probe(/proc/self): procfs is never raw-writable");
        T_EQ(g.devclass, EXITOS_DEV_UNKNOWN, "probe(/proc/self) class = UNKNOWN");
    } else {
        T_SKIP("probe(/proc/self) failed; procfs not available");
    }
    memset(&g, 0xFF, sizeof g);
    if (exitos_geom_probe("/dev/shm", &g) == 0) {
        T_EQ(g.writable_raw, 0, "probe(/dev/shm): tmpfs is never raw-writable");
    } else {
        T_SKIP("probe(/dev/shm) failed; tmpfs not available");
    }

    /* Whatever this host is, the grant must be self-consistent. */
    memset(&g, 0, sizeof g);
    if (exitos_geom_probe("/tmp", &g) == 0) {
        printf("# probe(/tmp): class=%s part=%s disk=%s start=%llu lbs=%u fs_bs=%u raw=%d\n",
               exitos_devclass_str(g.devclass), g.part_name, g.disk_path,
               (unsigned long long)g.part_start_sect, g.lbs, g.fs_bs, g.writable_raw);
        T_OK(g.writable_raw == 0 ||
             g.devclass == EXITOS_DEV_OK || g.devclass == EXITOS_DEV_LOOP,
             "probe(/tmp): writable_raw=1 only for class OK or LOOP");
        T_OK(g.writable_raw == 0 ||
             (g.lbs != 0 && g.fs_bs != 0 && g.fs_bs % g.lbs == 0),
             "probe(/tmp): a grant implies an expressible geometry "
             "(lbs!=0, fs_bs%%lbs==0)");
        T_OK(g.writable_raw == 0 || g.disk_path[0] == '/',
             "probe(/tmp): a grant implies a usable disk_path");
    } else {
        T_SKIP("probe(/tmp) failed on this host");
    }
}

/* ================================================================== *
 * GROUP 10 - exitos_durability_policy() and policy_for().
 *
 * Property (include/exitos_durability.h, verbatim): "FAILS CLOSED: anything
 * unreadable yields EXITOS_DUR_FLUSH, because wrongly skipping a flush loses
 * data while a needless flush only costs time."
 *
 * Why it matters: EXITOS_DUR_NONE means "issue nothing".  Returning it for a
 * device we could not interrogate turns an unknown drive into a silently
 * non-durable one, which is a data-loss defect, not a performance one.
 * ================================================================== */
static void g10_durability(void)
{
    T_EQ(exitos_durability_policy(0, 0), EXITOS_DUR_NONE,
         "policy(cache=0, fua=0) = NONE: nothing volatile to flush");
    T_EQ(exitos_durability_policy(0, 1), EXITOS_DUR_NONE,
         "policy(cache=0, fua=1) = NONE: FUA is pointless with no cache to bypass");
    T_EQ(exitos_durability_policy(1, 1), EXITOS_DUR_FUA,
         "policy(cache=1, fua=1) = FUA: cheapest correct option");
    T_EQ(exitos_durability_policy(1, 0), EXITOS_DUR_FLUSH,
         "policy(cache=1, fua=0) = FLUSH: only remaining correct option");

    /* Non-canonical booleans must not fall into the NONE branch. */
    T_OK(exitos_durability_policy(2, 0) != EXITOS_DUR_NONE,
         "policy(cache=2, fua=0) is not NONE: a nonzero cache flag means a cache");
    T_OK(exitos_durability_policy(-1, 0) != EXITOS_DUR_NONE,
         "policy(cache=-1, fua=0) is not NONE");

    /* policy_for on things that cannot be interrogated. */
    char nope[128];
    snprintf(nope, sizeof nope, "/dev/exitos_no_such_dev_%d", (int)getpid());
    T_EQ(exitos_durability_policy_for(nope), EXITOS_DUR_FLUSH,
         "policy_for(missing device) = FLUSH (fail closed, never NONE)");
    T_EQ(exitos_durability_policy_for(NULL), EXITOS_DUR_FLUSH,
         "policy_for(NULL) = FLUSH (fail closed, no crash)");
    T_EQ(exitos_durability_policy_for(""), EXITOS_DUR_FLUSH,
         "policy_for(\"\") = FLUSH (fail closed)");
    T_EQ(exitos_durability_policy_for("/tmp"), EXITOS_DUR_FLUSH,
         "policy_for(a directory) = FLUSH (fail closed)");
    T_EQ(exitos_durability_policy_for("/dev/"), EXITOS_DUR_FLUSH,
         "policy_for(/dev/) = FLUSH (fail closed on an empty basename)");
    T_EQ(exitos_durability_policy_for("/dev/../etc/passwd"), EXITOS_DUR_FLUSH,
         "policy_for(path traversal) = FLUSH (fail closed)");

    /* probe() must not report success while leaving the caller's variables
     * unset, and policy_for() must agree with policy() on the probed facts. */
    int vol = -7, fua = -7;
    if (exitos_durability_probe("/dev/sda", &vol, &fua) == 0) {
        T_OK(vol != -7 && fua != -7,
             "probe(/dev/sda) returned 0 and set both outputs (vol=%d fua=%d)", vol, fua);
        T_EQ(exitos_durability_policy_for("/dev/sda"),
             exitos_durability_policy(vol, fua),
             "policy_for(/dev/sda) matches policy() on the probed facts");
    } else {
        T_SKIP("no readable queue/write_cache for /dev/sda on this host");
    }
    T_EQ(exitos_durability_probe(NULL, &vol, &fua), -1,
         "probe(NULL) = -1 (no crash)");

    const exitos_durability ds[] = {
        EXITOS_DUR_NONE, EXITOS_DUR_FUA, EXITOS_DUR_FLUSH,
        (exitos_durability)3, (exitos_durability)-1,
    };
    int nonnull = 1, distinct = 1, oob_ok = 1;
    for (unsigned i = 0; i < 5; i++)
        if (exitos_durability_str(ds[i]) == NULL)
            nonnull = 0;
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = i + 1; j < 3; j++)
            if (strcmp(exitos_durability_str(ds[i]), exitos_durability_str(ds[j])) == 0)
                distinct = 0;
    for (unsigned i = 3; i < 5; i++)
        for (unsigned j = 0; j < 3; j++)
            if (strcmp(exitos_durability_str(ds[i]), exitos_durability_str(ds[j])) == 0)
                oob_ok = 0;
    T_OK(nonnull, "durability_str: non-NULL for all 3 enum values and 2 out-of-range values");
    T_OK(distinct, "durability_str: all 3 policies have distinct labels");
    T_OK(oob_ok, "durability_str: an out-of-range policy is not labelled as a real one");
}


/* ================================================================== *
 * A named serial must be compared BEFORE any other verdict.
 *
 * Property: exitos_devguard_check() with a serial the operator named answers
 * DEVGUARD_SERIAL_MISMATCH when it does not match, whatever else is true of
 * the device.
 *
 * Why it matters: the checks used to run in the order partitioned -> mounted ->
 * serial, so a partitioned or mounted disk returned its own verdict and the
 * named serial was never compared at all. On a host where a reboot had renamed
 * the disks -- which is the entire reason the serial is the identity -- an
 * operator naming the correct serial could be pointed at a different device and
 * told only that it was "partitioned". Any caller that then decided partitioned
 * was acceptable for its purpose would be working on the wrong disk with the
 * identity check silently skipped.
 * ================================================================== */
static void g4b2_serial_precedence(void)
{
    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    int checked = 0;

    if (!d) { T_SKIP("cannot read /sys/class/block"); return; }
    while ((e = readdir(d)) != NULL && checked < 2) {
        char pf[512];
        devguard_verdict v;
        if (e->d_name[0] == '.') continue;
        snprintf(pf, sizeof pf, "/sys/class/block/%s/partition", e->d_name);
        if (access(pf, F_OK) == 0) continue;          /* want a whole disk */
        snprintf(pf, sizeof pf, "/sys/class/block/%s1", e->d_name);
        if (access(pf, F_OK) != 0) {
            snprintf(pf, sizeof pf, "/sys/class/block/%sp1", e->d_name);
            if (access(pf, F_OK) != 0) continue;      /* ...that has partitions */
        }
        v = exitos_devguard_check(e->d_name, "SERIAL_THAT_MATCHES_NOTHING_XY", NULL, 0);
        T_OK(v == DEVGUARD_SERIAL_MISMATCH || v == DEVGUARD_NO_SERIAL,
             "check(%s, a serial that matches nothing) answers about the IDENTITY "
             "first, not about the partition table (got '%s')",
             e->d_name, exitos_devguard_str(v));
        checked++;
    }
    closedir(d);
    if (!checked)
        T_SKIP("no partitioned whole disk on this host to test serial precedence");
}

int main(void)
{
    char tmpdir[128];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/exitos_guards_hard_%d", (int)getpid());
    if (mkdir(tmpdir, 0700) != 0)
        tmpdir[0] = '\0';

    g1_passthru_names();
    g2_passthru_paths();
    g3_check_hostile();
    if (tmpdir[0])
        g4_check_basename_only(tmpdir);
    else
        T_SKIP("cannot create %s; skipping the same-basename decoy group", tmpdir);
    g4b_partition_escalation();
    g4c_serial_matching();
    g4d_loop_ignores_expectation();
    g5_label_functions();
    g6_arithmetic();
    g7_overflow();
    g4b2_serial_precedence();
    g8_refusal();
    g9_probe_invariants();
    g10_durability();

    if (tmpdir[0])
        rmdir(tmpdir);
    T_DONE();
}
