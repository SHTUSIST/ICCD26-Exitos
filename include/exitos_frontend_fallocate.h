/* Shared setup-time fallocate policy used by every interception frontend. */
#ifndef EXITOS_FRONTEND_FALLOCATE_H
#define EXITOS_FRONTEND_FALLOCATE_H

#include <stdint.h>
#include <sys/types.h>

struct exitos_ctx;
struct exitos_frontend_config;
struct exitos_frontend_fallocate_runtime;

/* Frontend adapters normalize libc/raw-syscall differences to 0/-errno. */
typedef int (*exitos_frontend_native_fallocate_fn)(void *opaque, int fd,
                                                   int mode, off_t off,
                                                   off_t len);

/* These values and their name helpers form the v=1 PREPARE_OUTCOME schema.
 * Append new values if needed; do not silently repurpose an existing value. */
enum exitos_frontend_fallocate_outcome {
    EXITOS_FALLOCATE_NATIVE_SKIP = 0,
    EXITOS_FALLOCATE_ELIGIBLE_FAILED,
    EXITOS_FALLOCATE_PREPARED,
    EXITOS_FALLOCATE_CONTROL_FAILED,
};

enum exitos_frontend_fallocate_stage {
    EXITOS_FALLOCATE_STAGE_ENTRY = 0,
    EXITOS_FALLOCATE_STAGE_TXN_BEGIN,
    EXITOS_FALLOCATE_STAGE_FLUSH,
    EXITOS_FALLOCATE_STAGE_CANDIDATE,
    EXITOS_FALLOCATE_STAGE_NATIVE,
    EXITOS_FALLOCATE_STAGE_DONOR_DIR,
    EXITOS_FALLOCATE_STAGE_DONOR_POOL,
    EXITOS_FALLOCATE_STAGE_RDWR_ALIAS,
    EXITOS_FALLOCATE_STAGE_REVALIDATE,
    EXITOS_FALLOCATE_STAGE_DONOR_PREPARE,
    EXITOS_FALLOCATE_STAGE_DONOR_PROOF,
    EXITOS_FALLOCATE_STAGE_MAP_REFRESH,
    EXITOS_FALLOCATE_STAGE_RDWR_ALIAS_CLOSE,
    EXITOS_FALLOCATE_STAGE_COMPLETE,
};

struct exitos_frontend_fallocate_result {
    uint64_t prepared_bytes;
    uint32_t chunks;
    /* rc is the diagnostic cause.  It may differ from the execute() return
     * only when transaction admission fails and the native fallback runs. */
    int rc;
    enum exitos_frontend_fallocate_outcome outcome;
    enum exitos_frontend_fallocate_stage stage;
    unsigned prepared : 1;
    unsigned used_rdwr_alias : 1;
};

const char *exitos_frontend_fallocate_outcome_name(
    enum exitos_frontend_fallocate_outcome outcome);
const char *exitos_frontend_fallocate_stage_name(
    enum exitos_frontend_fallocate_stage stage);

/* config and ctx are borrowed until destroy.  Pool creation is deliberately
 * lazy: the first eligible application fallocate supplies the one static size
 * accepted by this runtime. */
int exitos_frontend_fallocate_runtime_create(
    struct exitos_frontend_fallocate_runtime **out,
    const struct exitos_frontend_config *config, struct exitos_ctx *ctx);
void exitos_frontend_fallocate_runtime_destroy(
    struct exitos_frontend_fallocate_runtime *runtime);

/* Execute the complete same-fd transaction.  Donor takeover is intentionally
 * limited to fresh files under exclusive setup: no other descriptor/process
 * may write or rebind the inode during this call.  The runtime revalidates
 * freshness after lazy pool construction, but Linux offers no lock that can
 * make an uncooperative cross-process writer obey this userspace transaction.
 * Ordinary calls retain the
 * begin/flush/invalidate/native/note ordering.  An eligible donor call never
 * falls back after it is selected: any setup/proof failure is fail closed. */
int exitos_frontend_fallocate_execute(
    struct exitos_frontend_fallocate_runtime *runtime, int fd, int mode,
    off_t off, off_t len, exitos_frontend_native_fallocate_fn native_call,
    void *native_opaque, struct exitos_frontend_fallocate_result *result);

#endif
