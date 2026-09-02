/* geom: translate ext4 filesystem block numbers -> device LBA, and refuse
 * to do so on any stacked block device where that translation is invalid.
 * This is the safety-critical module: a wrong LBA overwrites unrelated data. */
#ifndef EXITOS_GEOM_H
#define EXITOS_GEOM_H
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    EXITOS_DEV_OK = 0,        /* plain partition (or whole disk) on nvme/sd  */
    EXITOS_DEV_DM,            /* device-mapper stacked (LVM/dm-crypt/...)    */
    EXITOS_DEV_MD,            /* md software RAID                            */
    EXITOS_DEV_LOOP,          /* loop device: allowed, test fixture          */
    EXITOS_DEV_UNKNOWN,       /* could not determine -> must refuse          */
} exitos_dev_class;

struct exitos_geom {
    char     part_name[64];   /* "nvme0n1p1" / "loop0"                       */
    char     disk_path[96];   /* "/dev/nvme0n1" / "/dev/loop0"               */
    uint64_t part_start_sect; /* /sys/class/block/<part>/start, 512B units   */
    uint32_t lbs;             /* device logical block size (bytes)           */
    uint32_t fs_bs;           /* filesystem block size (bytes)               */
    exitos_dev_class devclass;
    int      writable_raw;    /* 1 only when devclass is OK or LOOP          */
};

/* Populate geom for the filesystem that `path` lives on. Returns 0 on success.
 * Never returns writable_raw=1 for DM/MD/UNKNOWN. */
int exitos_geom_probe(const char *path, struct exitos_geom *out);

/* Core translation. fs_block is an ext4 block number (fs_bs units). The
 * sysfs partition start is converted from its fixed 512-byte-sector unit to
 * native device LBAs before it is added.
 * Returns 0 and sets *lba (in device logical blocks) on success.
 * Returns -EPERM when geom->writable_raw == 0. */
int exitos_geom_fsblock_to_lba(const struct exitos_geom *g,
                               uint64_t fs_block, uint64_t *lba);

const char *exitos_devclass_str(exitos_dev_class c);
#endif
