/* intercept: the userspace hook. Discipline borrowed from namei_ext --
 * default PASS, act only on registered fds, verify the LBA lies inside the
 * file's own registered extents, and fall back to the real syscall on any doubt. */
#ifndef EXITOS_INTERCEPT_H
#define EXITOS_INTERCEPT_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum { EXITOS_PASS = 0, EXITOS_TAKEOVER = 1 } exitos_decision;

struct exitos_ctx;

/* A frontend transaction pins one registration and serializes operations on
 * that fd from the decision through the real syscall and its completion note.
 * The fields are opaque to callers; do not copy a live token.  begin/end must
 * run on the same thread.  begin returns 0 even when fd is unregistered; in
 * that case registered() is false, but end is still mandatory because the
 * token temporarily prevents a concurrent registration from appearing between
 * a frontend's lookup and its real syscall.
 *
 * Transactions on different registered fds run concurrently and pin separate
 * fd-hashed table-lock shards, avoiding a process-wide cacheline write on each
 * operation. Registration, unregistration, and successful descriptor
 * replacement are control-plane writer operations: they freeze every shard and
 * wait for all current transactions, so callers should not put those operations
 * on the steady-state write path. */
struct exitos_fd_txn {
    struct exitos_ctx *ctx;
    void              *registration;
    int                fd;
    unsigned           held;
    unsigned           registered;
    unsigned           restore_raw_debt;
};

/* Descriptor-alias transaction.  The source and overwritten target (when
 * distinct) are pinned under their table shards (acquired in shard order) and
 * their per-fd locks are acquired in ascending-fd order, preventing
 * dup2(A,B) / dup2(B,A) deadlock.
 * The conservative alias policy is intentional: before dup/dup2/dup3 or
 * fcntl(F_DUPFD*) exposes an unregistered alias, settle raw debt on every old
 * registration and disable the registered source fast path. Thus a sync issued
 * through any alias cannot overlook later raw writes on the original fd.
 *
 * Valid sequence: begin, prepare, run the real alias syscall while the token is
 * held, commit on success or abort on failure, then end.  fork() and descriptor
 * transfer via SCM_RIGHTS are not intercepted by either frontend: callers must
 * flush/unregister before either operation.  A received descriptor is safely
 * unregistered, but cannot account for raw debt created by a registered sender. */
struct exitos_alias_txn {
    struct exitos_ctx *ctx;
    void              *source;
    void              *target;
    void              *lock_first;
    void              *lock_second;
    int                source_fd;
    int                target_fd;
    unsigned           held;
    unsigned           source_registered;
    unsigned           target_registered;
    unsigned           restore_source_raw;
    unsigned           restore_target_raw;
};

int  exitos_alias_txn_begin(struct exitos_ctx *c, int source_fd, int target_fd,
                            struct exitos_alias_txn *tx);
int  exitos_alias_txn_prepare(struct exitos_alias_txn *tx);
void exitos_alias_txn_abort(struct exitos_alias_txn *tx);
void exitos_alias_txn_commit(struct exitos_alias_txn *tx);
void exitos_alias_txn_end(struct exitos_alias_txn *tx);

int  exitos_fd_txn_begin(struct exitos_ctx *c, int fd, struct exitos_fd_txn *tx);
int  exitos_fd_txn_registered(const struct exitos_fd_txn *tx);
/* Prepare a destructive descriptor operation (close/dup replacement) without
 * losing completed raw-write debt. It pays that debt while the old target is
 * still stable. If the real syscall then fails, abort_forget restores the exact
 * logical debt state; if it succeeds, forget deactivates the registration.
 * forget also pays debt itself when a caller omitted the explicit prepare. */
int  exitos_fd_txn_prepare_forget(struct exitos_fd_txn *tx);
void exitos_fd_txn_abort_forget(struct exitos_fd_txn *tx);
int  exitos_fd_txn_forget(struct exitos_fd_txn *tx);
void exitos_fd_txn_end(struct exitos_fd_txn *tx);

exitos_decision exitos_txn_on_write(struct exitos_fd_txn *tx,
                                    const void *buf, size_t count, off_t off,
                                    ssize_t *result);
exitos_decision exitos_txn_on_fdatasync(struct exitos_fd_txn *tx, int *result);
int             exitos_txn_flush_raw_debt(struct exitos_fd_txn *tx);
exitos_decision exitos_txn_on_ftruncate(struct exitos_fd_txn *tx, off_t len,
                                        int *result);
/* Drop the complete cached extent snapshot while the fd transaction is held.
 * Used before fallocate-class operations whose mode may allocate, free, zero,
 * collapse, insert, or unshare ranges. */
void            exitos_txn_invalidate_mapping(struct exitos_fd_txn *tx);
/* Rebuild the complete Maco/bounds snapshot while `tx` already pins the
 * registration, then prove that every byte in [off, off+len) is mapped and
 * that each resulting device-LBA span is inside the file's rebuilt bounds.
 * This is the setup-time counterpart to the write-path miss refresh: callers
 * that have just initialized/exchanged extents can make the first timed write
 * use the ready snapshot without unregister/register lock escalation.
 *
 * Returns 0 on complete coverage. Stable validation failures are -ENOENT for
 * an unregistered/not-held transaction, -EINVAL for an empty range,
 * -EOVERFLOW for a wrapping range, and -ENXIO for incomplete mapping/bounds;
 * layout discovery and fstat failures retain their specific negative errno. */
int             exitos_txn_refresh_mapping_range(struct exitos_fd_txn *tx,
                                                  uint64_t off, uint64_t len);
void            exitos_txn_note_kernel_write(struct exitos_fd_txn *tx);
/* A successful metadata-only kernel operation (currently ftruncate) also
 * requires a later real fdatasync, but is not a PASS data write and therefore
 * must not affect write-path counters. */
void            exitos_txn_note_kernel_metadata(struct exitos_fd_txn *tx);
void            exitos_txn_note_kernel_sync(struct exitos_fd_txn *tx);

/* Snapshot EXITOS_IOPATH at context creation.  Later environment changes do
 * not affect registrations already sharing this context.  Invalid explicit
 * modes make this compatibility constructor return NULL with errno=EINVAL. */
struct exitos_ctx *exitos_ctx_create(void);
/* Error-returning constructor used by the shared frontend configuration.
 * iopath_mode is NULL for the default ordered fallback, otherwise it must be
 * one of the documented exact mode names. */
int exitos_ctx_create_with_options(struct exitos_ctx **out,
                                   const char *iopath_mode);
/* The owner must stop/join all callers before destroy. The function waits for
 * already-admitted transactions and makes a best-effort raw-debt flush before
 * releasing iopaths. It has no error channel: callers needing a reportable
 * durability boundary must sync or unregister every fd first. It cannot make a
 * thread that may begin using a freed ctx afterward safe. */
void               exitos_ctx_destroy(struct exitos_ctx *c);

/* Permanently disable takeover in this context.  The store is lock-free and
 * may be issued from a reentrant signal path; it does not itself flush or take
 * a pthread lock.  Descriptor-wide mutators use flush_all_and_poison instead:
 * all completed raw writes are paid before fd numbers can be recycled. */
void exitos_ctx_poison(struct exitos_ctx *c);
int  exitos_ctx_is_poisoned(const struct exitos_ctx *c);
int  exitos_ctx_flush_all_and_poison(struct exitos_ctx *c);

/* Register a file for the fast path: probes geom, loads Maco. The fd must be
 * writable O_DIRECT, without O_APPEND/O_SYNC/O_DSYNC; this preserves EBADF and
 * rejects filesystem/open-description semantics the raw path cannot emulate.
 * Unsupported modes/device stacks simply keep the normal path. */
int  exitos_register_fd(struct exitos_ctx *c, int fd);
/* Unregister settles raw FLUSH debt before removing the only iopath owner. On
 * FLUSH failure it returns negative errno and leaves the registration/debt
 * installed for retry. This matters even when another alias stays open: a
 * later sync through that unregistered alias cannot see per-registration raw
 * debt, so dropping it here would falsely report durability. */
int  exitos_unregister_fd(struct exitos_ctx *c, int fd);

/* Declare that a write on this fd went to the kernel without passing through
 * exitos_on_write. Any entry point this layer does not interpose leaves data in
 * the kernel that only the kernel's own fdatasync persists, and without this
 * call the shortcut would
 * answer that fdatasync itself and report durability that does not exist. */
void exitos_note_kernel_write(struct exitos_ctx *c, int fd);
/* The caller performed the kernel's own fdatasync/fsync on this descriptor and
 * it returned success. This pays only the filesystem/kernel debt. A raw write
 * has an independent device-FLUSH debt and is never cleared by this note. Call
 * only on success: on failure the kernel debt is still owed. */
void exitos_note_kernel_sync(struct exitos_ctx *c, int fd);

/* Persist raw writes that completed through the shortcut but have not yet been
 * forced out of a volatile device cache. Returns 0 when there is no raw debt
 * (including unregistered fds), when writes used FUA/write-through media, or
 * after a successful FLUSH. Returns a negative errno on failure and leaves the
 * debt set so the next sync can retry. Frontends must preserve that internal
 * convention: raw syscall hooks return it directly; libc wrappers return -1
 * and set errno to its negation. */
int  exitos_flush_raw_debt(struct exitos_ctx *c, int fd);

/* Turn on per-write verification that an fd still names the file it was
 * registered for. Costs one fstat per write, which is why it is off by default
 * and why the note above tells callers to unregister before rebinding an fd
 * instead. Turn it on when correctness under an uncooperative caller matters
 * more than the syscall: a rebound fd then loses its registration and its write
 * goes the ordinary way, rather than onto the previous file's blocks. */
void exitos_ctx_verify_identity(struct exitos_ctx *c, int on);
/* Strict mode (Exitos-S): per-write kernel permission probe through the real
 * pwrite channel; implies the fd identity check. Off by default. */
void exitos_ctx_strict(struct exitos_ctx *c, int on);

/* The decision function: one entry point, like namei_ext's single hook. Each
 * direct call is internally serialized and lifetime-safe. However, a caller
 * that receives EXITOS_PASS and then invokes the real syscall concurrently
 * with sync MUST use the explicit transaction API above across decision,
 * syscall, and completion note; the LD_PRELOAD and bpftime frontends do so. */
exitos_decision exitos_on_write(struct exitos_ctx *c, int fd, const void *buf,
                                size_t count, off_t off, ssize_t *result);
/* On TAKEOVER, *result is 0 on success or a negative errno when raw durability
 * failed. Raw-syscall frontends return it unchanged; libc frontends translate
 * it to -1/errno. On PASS, *result is untouched and the real fdatasync must run. */
exitos_decision exitos_on_fdatasync(struct exitos_ctx *c, int fd, int *result);
/* Rotation/truncation: rebuild or invalidate Maco so stale LBAs are never used. */
exitos_decision exitos_on_ftruncate(struct exitos_ctx *c, int fd, off_t len, int *result);
int  exitos_lba_in_bounds(struct exitos_ctx *c, int fd, uint64_t lba, size_t len);

/* 1 when no filesystem/kernel write remains unsynced on this registration. A
 * fallback changes it to 0; a frontend changes it back only after the real
 * fdatasync/fsync succeeds and it calls exitos_note_kernel_sync(). */
int  exitos_fd_all_writes_took_fast_path(struct exitos_ctx *c, int fd);

/* Does fdatasync still have real work to do under this durability policy?
 * Returns 1 only when the device has a volatile write cache that we could not
 * make the write itself bypass. On a write-through device the kernel already
 * skips the flush, so answering this correctly is what decides whether the sync
 * costs a device barrier at all. */
int  exitos_fdatasync_needed(int durability_policy);
#endif
