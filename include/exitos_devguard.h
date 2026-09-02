/* devguard: refuse to hand out a raw-writable device unless it is provably the
 * disk the operator meant. Kernel NVMe names (nvme0n1, nvme3n1...) are assigned
 * in probe order and CAN CHANGE ACROSS REBOOTS -- a reboot of a shared host
 * rotated the free 1.92TB disk from nvme3n1 to nvme2n1 while the ROOT
 * FILESYSTEM took over the name nvme3n1. Writing raw LBAs to a remembered
 * device name would have destroyed the system disk. Identity must be stable for
 * the namespace: its WWID, or a controller identifier combined with its NSID. */
#ifndef EXITOS_DEVGUARD_H
#define EXITOS_DEVGUARD_H
#include <stdint.h>

#define EXITOS_DEVGUARD_IDENTITY_MAX 256

struct devguard_device_info {
    /* Namespace-stable identity used by pins (WWID, or controller+NSID). */
    char identity[EXITOS_DEVGUARD_IDENTITY_MAX];
    /* Operator-facing controller serial expectation, kept separate because
     * one controller can own multiple namespaces with distinct identities. */
    char controller_serial[EXITOS_DEVGUARD_IDENTITY_MAX];
    uint32_t logical_block_size;
    unsigned dev_major;
    unsigned dev_minor;
    unsigned parent_major;
    unsigned parent_minor;
    int is_partition;
};

typedef enum {
    DEVGUARD_OK = 0,
    DEVGUARD_NOT_FOUND,       /* no such block device                          */
    DEVGUARD_HAS_PARTITIONS,  /* partitioned -> almost certainly someone's data */
    DEVGUARD_MOUNTED,         /* it or a partition of it is mounted            */
    DEVGUARD_SERIAL_MISMATCH, /* not the disk the operator named               */
    DEVGUARD_NO_SERIAL,       /* serial unreadable -> cannot prove identity    */
    DEVGUARD_IS_PARTITION,    /* a partition node: the data the refusal above  */
                              /* protects. Never OK by default; a caller that  */
                              /* deliberately uses one must accept this verdict*/
                              /* by name, and must not send passthru to it.    */
    DEVGUARD_NOT_A_DEVICE,    /* the path given is not a block device node     */
} devguard_verdict;

/* expect_serial is compared to the actual controller serial, while pins use
 * the namespace-stable identity above.  serial_out returns that controller
 * serial.  expect_serial may be NULL only when the caller has no expectation. */
devguard_verdict exitos_devguard_check(const char *dev_path, const char *expect_serial,
                                       char *serial_out, unsigned serial_len);
devguard_verdict exitos_devguard_check_fd(int fd, const char *expect_serial,
                                          char *serial_out, unsigned serial_len);
devguard_verdict exitos_devguard_check_fd_at(int fd, const char *sysfs_root,
                                             const char *mountinfo_path,
                                             const char *expect_serial,
                                             char *serial_out,
                                             unsigned serial_len);
const char *exitos_devguard_str(devguard_verdict v);

/* Resolve only from the already-open block object.  The `_at` form exists so
 * unit tests can supply an isolated sysfs tree; production always uses /sys. */
int exitos_devguard_resolve_fd(int fd, struct devguard_device_info *out);
int exitos_devguard_resolve_fd_at(int fd, const char *sysfs_root,
                                  struct devguard_device_info *out);

/* Absolute native-LBA span of the already-open block object.  Sysfs reports
 * start and size in fixed 512-byte sectors; these helpers convert them to the
 * logical block size proved by the fd-bound resolver. */
int exitos_devguard_lba_span_fd(int fd, uint64_t *base_lba,
                                uint64_t *count_lba);
int exitos_devguard_lba_span_fd_at(int fd, const char *sysfs_root,
                                   uint64_t *base_lba, uint64_t *count_lba);

/* Prove that the already-open block object's whole-disk queue supports
 * io_uring polling.  The fd's BLKGETDISKSEQ is bound to a retained parent
 * sysfs directory, so a reused major:minor cannot lend capability metadata to
 * an older open file description. */
int exitos_devguard_poll_available_fd(int fd);
int exitos_devguard_poll_available_fd_at(int fd, const char *sysfs_root);

/* 1 = target or child partition mounted, 0 = not mounted, negative = the
 * query was not trustworthy.  Production checks treat negative as refusal. */
int exitos_devguard_mounted_at(const char *sysfs_root, const char *mountinfo_path,
                               unsigned dev_major, unsigned dev_minor);

/* Is it safe to send NVMe passthru commands to this device node?
 * Returns 1 only for a whole disk / namespace. A partition node returns 0:
 * passthru commands carry an ABSOLUTE namespace LBA and the kernel does not
 * clamp them to the partition, so an LBA that looks partition-relative lands
 * elsewhere on the disk. This exact mistake has overwritten a GPT. */
int exitos_devguard_passthru_ok(const char *dev_path);
int exitos_devguard_passthru_ok_fd(int fd);
int exitos_devguard_passthru_ok_fd_at(int fd, const char *sysfs_root);

/* Is the LBA window [lba, lba+count) wholly inside the named partition of this
 * same disk?  Returns 0 when it is, and a negative value otherwise, writing a
 * sentence into `why`.
 *
 * Why this exists.  NVMe passthru addresses the whole namespace, so it must be
 * sent to the namespace node and never to a partition node -- a partition-
 * relative LBA sent to the namespace lands somewhere else entirely, which is
 * how a GPT was overwritten.  But refusing every partitioned disk
 * then leaves no way to measure passthru at all on a disk whose partitions are
 * the operator's own.  This is the narrow answer: the operator names the
 * partition the window is supposed to be inside, and the partition table --
 * not the operator's arithmetic -- decides whether it really is.  A mistyped
 * offset is refused here rather than written.
 *
 * The window is given in the device's logical blocks; the partition table is in
 * 512-byte sectors, and the conversion is done here so the caller cannot get it
 * wrong either.  Refuses when `part_name` is not a partition, when it belongs
 * to a different disk, or when the window crosses either edge. */
/* The device's logical block size in bytes, or 0 if it cannot be read. The
 * window-containment check needs it to convert the caller's LBAs into the
 * 512-byte sectors the partition table is written in. */
/* The namespace-stable identity of a block device by NAME (no leading /dev/):
 * namespace WWID, controller identifier plus NSID, the parent's identity for a
 * partition, or a synthesised name for a loop device. */
int exitos_devguard_identity(const char *base, char *out, unsigned len);

/* Prove that an NVMe block namespace and generic character node address the
 * same controller AND namespace ID. Device names alone are not related: after
 * reprobe, /dev/nvme1n2 has been observed to pair with /dev/ng4n2. */
int exitos_devguard_nvme_pair(const char *block_path, const char *char_path);

/* Pure sysfs identity check used by the path-based gate above.  sysfs_root is
 * normally "/sys"; tests may point it at an isolated tree.  The lookup is by
 * device number, never by a caller-controlled path basename. */
int exitos_devguard_nvme_pair_at(const char *sysfs_root,
                                 unsigned block_major, unsigned block_minor,
                                 unsigned char_major, unsigned char_minor,
                                 uint32_t char_nsid);

/* 1 only when /dev/block/major:minor resolves to a whole block device (not a
 * partition).  Missing, malformed and unresolvable sysfs data fail closed. */
int exitos_devguard_whole_block_at(const char *sysfs_root,
                                   unsigned dev_major, unsigned dev_minor);

uint32_t exitos_devguard_lbs(const char *dev_path);
uint32_t exitos_devguard_lbs_fd(int fd);

int exitos_devguard_window_in_partition(const char *dev_path, const char *part_name,
                                        uint64_t lba, uint64_t count, uint32_t lbs,
                                        char *why, unsigned wlen);
int exitos_devguard_window_in_partition_fds(int disk_fd, int partition_fd,
                                            uint64_t lba, uint64_t count,
                                            uint32_t lbs,
                                            char *why, unsigned wlen);
int exitos_devguard_window_in_partition_fds_at(int disk_fd, int partition_fd,
                                               const char *sysfs_root,
                                               uint64_t lba, uint64_t count,
                                               uint32_t lbs,
                                               char *why, unsigned wlen);

/* ---------------------------------------------------------------------------
 * DISK PINNING.  Serial alone is not enough: the CONTENT of a disk can change
 * between the moment an operator certifies a region as free and the moment we
 * write to it (a reboot, another user, another tool).  A pin records what the
 * disk looked like when it was certified; verify it immediately before every
 * raw write and refuse on ANY difference.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
struct devguard_pin {
    char     serial[EXITOS_DEVGUARD_IDENTITY_MAX]; /* namespace-stable identity */
    uint64_t size_bytes;
    uint64_t sample_hash;   /* FNV-1a over deterministic sampled regions */
    uint64_t zero_lba_start;/* certified window start, in native logical blocks */
    uint64_t zero_lba_count;
    uint32_t logical_block_size; /* byte size of each native LBA above */
    uint32_t nsamples;
};
int exitos_devguard_pin_capture(const char *dev_path, uint64_t zlba, uint64_t zcnt,
                                struct devguard_pin *out);
int exitos_devguard_pin_capture_fd(int fd, uint64_t zlba, uint64_t zcnt,
                                   struct devguard_pin *out);
int exitos_devguard_pin_capture_fd_at(int fd, const char *sysfs_root,
                                      uint64_t zlba, uint64_t zcnt,
                                      struct devguard_pin *out);
int exitos_devguard_pin_save(const struct devguard_pin *p, const char *path);
int exitos_devguard_pin_load(struct devguard_pin *p, const char *path);
/* Proves both content identity and an exact write-window match.  A subset,
 * superset or shifted request is refused; there is deliberately no unbound
 * verification API. */
int exitos_devguard_pin_verify_window(const char *dev_path, const char *pin_path,
                                      uint64_t lba, uint64_t count,
                                      char *why, unsigned wlen);
int exitos_devguard_pin_verify_window_fd(int fd, const char *pin_path,
                                         uint64_t lba, uint64_t count,
                                         char *why, unsigned wlen);
int exitos_devguard_pin_verify_window_fd_at(int fd, const char *sysfs_root,
                                            const char *pin_path,
                                            uint64_t lba, uint64_t count,
                                            char *why, unsigned wlen);
/* Checked native-logical-block variant.  It validates all byte arithmetic and
 * refuses every failed or short read. */
int exitos_devguard_window_is_zero_checked(const char *dev_path, uint64_t lba,
                                           uint64_t cnt, uint32_t lbs,
                                           uint64_t *first_bad_lba);
/* Production fd form derives LBS from this fd's canonical sysfs namespace. */
int exitos_devguard_window_is_zero_fd(int fd, uint64_t lba, uint64_t cnt,
                                      uint64_t *first_bad_lba);
int exitos_devguard_window_is_zero_fd_at(int fd, const char *sysfs_root,
                                         uint64_t lba, uint64_t cnt,
                                         uint64_t *first_bad_lba);
#endif
