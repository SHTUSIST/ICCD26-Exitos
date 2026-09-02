/* Prepare space AHEAD of demand, on a background thread, so the write path
 * never pays for it.
 *
 * The paper puts this preparation off the critical path. That is not just a
 * timing convenience: donating a chunk costs an EXT4_IOC_MOVE_EXT ioctl plus one
 * sequential pass to initialise the range (the range must be initialised or the
 * fast path cannot use it, because ext4 reads an unwritten extent back as zeros
 * until a normal write converts it). Paying
 * that during an append would dwarf the append.
 *
 * So the write path must not call into this module at all except through
 * donor_async_runway(), which is a plain memory read: no ioctl, no io_uring, no
 * syscall of any kind. The background thread does every syscall.
 */
#ifndef EXITOS_DONOR_ASYNC_H
#define EXITOS_DONOR_ASYNC_H
#include <stdint.h>
#include "exitos_donor.h"

struct donor_async;

/* chunk      how much to donate per round.
 * low_water  when the runway falls below this, prepare another chunk.
 * Preparation for the first chunk starts immediately. */
struct donor_async *donor_async_start(struct donor_pool *p, int target_fd,
                                      uint64_t chunk, uint64_t low_water);
void                donor_async_stop(struct donor_async *a);

/* Bytes of prepared space at or beyond `off`. MEMORY READ ONLY — safe to call
 * on the write path. Zero means the writer has caught up with the preparer and
 * must fall back to the ordinary kernel path for this write. */
uint64_t donor_async_runway(const struct donor_async *a, uint64_t off);

/* Tell the preparer the writer has reached `off`. Non-blocking; wakes the
 * thread only when the runway is short. Safe on the write path. */
void     donor_async_advance(struct donor_async *a, uint64_t off);

/* Test/diagnostic accessors. */
uint64_t donor_async_prepared_to(const struct donor_async *a);
uint64_t donor_async_rounds(const struct donor_async *a);
/* Why preparation stopped, or 0 while it is still running. The preparer gives
 * up permanently on the first refusal from the pool or the filesystem, and a
 * runway of 0 on its own cannot tell an exhausted pool from a rejected offset. */
int donor_async_last_error(const struct donor_async *a);

/* Preparation begins at the target's end of file as it stands when this is
 * called, and never moves below the point last reported through
 * donor_async_advance(). Both matter because preparing a range replaces its
 * blocks and zeroes them: anything the caller has already written there is
 * destroyed, not preserved. */
#endif
