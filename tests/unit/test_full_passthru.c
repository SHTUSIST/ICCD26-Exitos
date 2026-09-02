/* fdatasync may be taken over only when the kernel currently holds no unsynced
 * write on that fd.
 *
 * The hazard the existing code flagged is real: if the application also wrote
 * through the kernel on the same descriptor, only the kernel's fdatasync
 * persists that half, so answering the call ourselves would silently drop it.
 * The fix is not to refuse always, but to track the debt: a registered fd that
 * passes a write must pass fdatasync until the real call succeeds and the
 * frontend reports that completion.
 *
 * Durability policy decides the rest: on a write-through device there is
 * genuinely nothing left for fdatasync to do. */
#include "tap.h"
#include "exitos_intercept.h"
#include "exitos_durability.h"
#include "exitos_stats.h"

int main(void)
{
    T_EQ(exitos_fdatasync_needed(EXITOS_DUR_NONE),  0, "no volatile cache: nothing left to do");
    T_EQ(exitos_fdatasync_needed(EXITOS_DUR_FUA),   0, "FUA rode on the write: nothing left to do");
    T_EQ(exitos_fdatasync_needed(EXITOS_DUR_FLUSH), 1, "flush still required");

    struct exitos_ctx *c = exitos_ctx_create();
    T_OK(c != 0, "ctx created");

    /* Unregistered fd: never taken over, whatever the policy. */
    int res = -999;
    T_EQ(exitos_on_fdatasync(c, -1, &res), EXITOS_PASS, "unregistered fd passes");
    T_EQ(res, -999, "PASS leaves the result untouched");

    /* The mixed-path guard must exist and must be queryable. */
    T_EQ(exitos_fd_all_writes_took_fast_path(c, -1), 0,
         "unregistered fd reports 'not all writes were ours'");

    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC), 0, "no fast sync counted for a PASS");
    exitos_ctx_destroy(c);
    T_DONE();
}
