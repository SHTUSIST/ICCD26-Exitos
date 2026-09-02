/* donor: preallocated donor files whose extents are handed to a growing log
 * file via EXT4_IOC_MOVE_EXT, giving preallocation's effect on the fly. */
#ifndef EXITOS_DONOR_H
#define EXITOS_DONOR_H
#include <stdint.h>
#include "exitos_maco.h"

struct donor_pool;

/* Every target_fd passed to the operations below must be an O_RDWR regular
 * file on the pool's ext4 filesystem.  In particular, O_WRONLY is rejected
 * before target mutation because EXT4_IOC_MOVE_EXT requires both read and
 * write mode on its original file description. */

/* Build a pool below one already-open directory identity.  The borrowed
 * descriptor remains owned by the caller; the pool opens and retains its own
 * descriptor, so later rename/path replacement cannot redirect child creation
 * or cleanup.  The directory must be a private (0700-style), euid-owned ext4
 * directory.  NULL always leaves a concrete errno. */
struct donor_pool *donor_pool_create_at(int dirfd, int nfiles,
                                        uint64_t bytes_each);

/* Compatibility wrapper: securely opens dir with O_DIRECTORY|O_NOFOLLOW and
 * delegates to donor_pool_create_at().  NULL always leaves a concrete errno. */
struct donor_pool *donor_pool_create(const char *dir, int nfiles,
                                     uint64_t bytes_each);
void               donor_pool_destroy(struct donor_pool *p);

/* Move `bytes` of contiguous space from a donor into target fd at file_off.
 * Returns bytes actually moved, or negative errno. */
int64_t donor_extend(struct donor_pool *p, int target_fd,
                     uint64_t file_off, uint64_t bytes);

/* Outcome of donor_prepare_exact(). prepared_bytes counts bytes whose extent
 * ownership was already changed by MOVE_EXT. On a negative return those bytes
 * may not have completed zero-initialization. MUTATED is broader: it is set as
 * soon as target fallocate succeeds, even if MOVE_EXT later makes no progress,
 * because rollback is necessarily best-effort and the target must not be
 * treated as untouched. */
struct donor_prepare_report {
    uint64_t requested_bytes;
    uint64_t prepared_bytes;
    uint32_t chunks;
    uint32_t flags;
};

#define DONOR_PREPARE_MUTATED  (1u << 0)
#define DONOR_PREPARE_SYNCED   (1u << 1)
#define DONOR_PREPARE_VERIFIED (1u << 2)

/* Prepare exactly [file_off, file_off+bytes), serially consuming partial donor
 * runs as needed. Success (0) means the complete range was initialized,
 * fdatasync'd, and FIEMAP_FLAG_SYNC verified with no holes, UNWRITTEN extents,
 * or flags that make raw physical locations unsafe. Failure is a negative
 * errno; report, when non-NULL, still describes any irreversible mutation.
 * Both offset and length must be filesystem-block aligned. */
int donor_prepare_exact(struct donor_pool *p, int target_fd,
                        uint64_t file_off, uint64_t bytes,
                        struct donor_prepare_report *report);

/* Give unused space back so the pool is not permanently consumed. */
int     donor_reclaim(struct donor_pool *p, int target_fd, uint64_t from_off);

/* 4x the moving average of observed write sizes, per the paper. */
uint64_t donor_next_chunk(struct donor_pool *p, uint64_t observed_write);

/* Filesystem block size the pool works in. donor_extend() refuses a file offset
 * that is not a multiple of it, so a caller computing its own offsets needs it. */
uint64_t donor_pool_blocksize(const struct donor_pool *p);
#endif
