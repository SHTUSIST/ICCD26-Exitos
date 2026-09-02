/* iopath: deliver data to a device LBA. Distinct backends let the
 * same correctness tests run on a loop device (safe) and on real NVMe. */
#ifndef EXITOS_IOPATH_H
#define EXITOS_IOPATH_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    IOPATH_PWRITE = 0,   /* pwrite() to the block device at lba*lbs. Works on
                          * loop devices -> validates LBA math with zero risk. */
    IOPATH_NVME_IOCTL,   /* NVME_IOCTL_IO_CMD passthru. Needs a real NVMe.     */
    IOPATH_URING_CMD,    /* io_uring IORING_OP_URING_CMD. Needs Linux >= 5.19. */
    IOPATH_URING_CMD_POLL,/* same, but completions are POLLED, not interrupt-driven:
                          * the submitting thread reaps the completion from an
                          * IOPOLL ring instead of waiting for a device interrupt.
                          * Requires the nvme driver to have poll queues
                          * (poll_queues=N at probe time); a stock kernel has none,
                          * so the host setup is opt-in. Selected explicitly by the
                          * operator through EXITOS_IOPATH, which is resolved once
                          * at context creation and fixed for that context's life.
                          * Costs a core: the caller spins instead of sleeping. */
    IOPATH_URING_WRITE_POLL,/* exact O_DIRECT block fd + ordinary
                             * IORING_OP_WRITE on an IOPOLL ring.  This bypasses
                             * the filesystem but retains the block layer. */
} iopath_backend;

/* Ring setup flags, exposed so the mapping from backend to flags is testable
 * without a device. */
#define IOPATH_SETUP_IOPOLL   (1u << 0)
#define IOPATH_SETUP_SQE128   (1u << 10)
/* IORING_SETUP_CQE32. NVMe passthru returns its completion result in the second
 * half of a 32-byte completion entry, and the kernel refuses the command
 * outright with EOPNOTSUPP if the ring was not set up to carry one. */
#define IOPATH_SETUP_CQE32    (1u << 11)
unsigned iopath_setup_flags(iopath_backend b);

/* Does this device actually offer polled completion? Reads
 * /sys/block/<disk>/queue/io_poll. Returns 1 yes, 0 no or unknown. Never
 * guesses: a backend that polls a device without poll queues never completes. */
int iopath_poll_available(const char *dev_path);
/* Same proof, bound to an already-open block object.  Partition fds are
 * resolved to their parent queue; 1 is the only accepted sysfs value. */
int iopath_poll_available_fd(int fd);

struct iopath;

/* Declare that the caller serializes every operation on this handle, letting
 * the internal ring/staging mutexes be skipped. One-way; 0 or -EINVAL. */
int iopath_set_exclusive(struct iopath *p, int on);

/* Exact-fd markers around iopath-owned libc calls.  Static-library users get
 * weak no-op definitions; preload/bpftime override them with lock-free TLS so
 * their reentrancy guard can distinguish an internal fd from a signal handler
 * mutating application descriptor state. */
void exitos_internal_fd_enter(int fd);
void exitos_internal_fd_leave(int fd);

/* Control-path I/O issued by the library itself.  Frontends override these
 * weak defaults and call the already-resolved libc functions directly, so a
 * registration-time sysfs/device helper never masquerades as asynchronous
 * application reentry.  These are explicit calls, not a TLS "next open"
 * token that a signal handler could accidentally consume. */
int exitos_internal_open_call(const char *path, int flags, mode_t mode);
int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode);
int exitos_internal_close_call(int fd);
int exitos_internal_ftruncate_call(int fd, off_t len);
int exitos_internal_fallocate_call(int fd, int mode, off_t off, off_t len);
ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off);
int exitos_internal_fdatasync_call(int fd);
/* Page-cache invalidation for buffered registrations (weak default: the real
 * posix_fadvise). */
int exitos_internal_fadvise_call(int fd, off_t off, off_t len, int advice);

struct iopath *iopath_open(const char *dev_path, iopath_backend b, uint32_t lbs);
/* Retain the selected open file description for PWRITE/NVME_IOCTL and the
 * ordinary polled block-WRITE backend without changing its access mode.
 * uring_cmd requires a separate generic-character fd; that node is discovered
 * from, and sysfs-paired to, this retained block identity. */
struct iopath *iopath_open_fd(int fd, iopath_backend b, uint32_t lbs);
/* The data-path calls on one handle are safe to invoke concurrently.  Handle
 * lifetime is different: the owner must stop and join/quiesce every caller
 * before iopath_close().  Concurrent close/use is invalid; an internal submit
 * mutex cannot make freeing the handle safe while another thread is about to
 * acquire that mutex. */
void           iopath_close(struct iopath *p);
/* Values retained and verified by the opened handle.  Non-NVMe handles report
 * NSID 0; NULL handles report 0 for both accessors. */
uint32_t       iopath_lbs(const struct iopath *p);
uint32_t       iopath_nsid(const struct iopath *p);

/* Write len bytes to device LBA. len must be a multiple of lbs. */
int  iopath_write(struct iopath *p, uint64_t lba, const void *buf, size_t len);
/* Persist: NVMe FLUSH, or fdatasync on the fd-based block backends. */
int  iopath_flush(struct iopath *p);
int  iopath_read(struct iopath *p, uint64_t lba, void *buf, size_t len);

/* Build an NVMe write command without submitting it, so command encoding is
 * unit-testable on a machine that has no NVMe at all. cmd is nvme_passthru_cmd. */
int  iopath_encode_nvme_write(void *cmd, uint32_t nsid, uint64_t slba,
                              uint16_t nlb, const void *buf, size_t len);

/* Same, but sets the Force Unit Access bit (cdw12 bit 30) when fua != 0.
 * FUA makes THIS write land on persistent media without flushing the whole
 * device cache, so durability rides on the write itself instead of on a
 * separate FLUSH command. */
int  iopath_encode_nvme_write_fua(void *cmd, uint32_t nsid, uint64_t slba,
                                  uint16_t nlb, const void *buf, size_t len, int fua);

/* Turn FUA on for every subsequent NVMe write through this handle. With it on,
 * iopath_flush() has nothing left to do and returns immediately. A pwrite
 * pwrite and ordinary block-WRITE handles cannot encode native NVMe FUA:
 * enabling it returns -EOPNOTSUPP and leaves flush-based mode active. */
int  iopath_set_fua(struct iopath *p, int on);
int  iopath_get_fua(const struct iopath *p);
#endif
