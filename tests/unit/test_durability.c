/* Durability policy, chosen from what the device actually reports.
 *
 * The state the decision reads is the namespace's write cache: with
 * queue/write_cache = "write through", queue/fua = 0 and NVMe vwc bit0 = 0
 * there is no volatile write cache, so the device ignores the FUA bit and an
 * explicit FLUSH forces a barrier that buys nothing.
 * So on a drive with no volatile cache the correct action is NEITHER: the write
 * is already durable when it completes. FUA is only worth setting when a
 * volatile cache exists, and an explicit FLUSH is the last resort. */
#include "tap.h"
#include "exitos_durability.h"

int main(void)
{
    /* No volatile cache -> the write is durable on completion. Do nothing. */
    T_EQ(exitos_durability_policy(0, 1), EXITOS_DUR_NONE,
         "no volatile cache -> DUR_NONE (no flush, no FUA)");
    T_EQ(exitos_durability_policy(0, 0), EXITOS_DUR_NONE,
         "no volatile cache, no FUA support -> still DUR_NONE");

    /* Volatile cache present and FUA available -> ride durability on the write. */
    T_EQ(exitos_durability_policy(1, 1), EXITOS_DUR_FUA,
         "volatile cache + FUA -> DUR_FUA (cheaper than a device-wide flush)");

    /* Volatile cache but no FUA -> an explicit FLUSH is the only correct option. */
    T_EQ(exitos_durability_policy(1, 0), EXITOS_DUR_FLUSH,
         "volatile cache, no FUA -> DUR_FLUSH as last resort");

    /* Names must be stable and non-NULL for logging. */
    T_OK(exitos_durability_str(EXITOS_DUR_NONE)  != 0, "DUR_NONE has a name");
    T_OK(exitos_durability_str(EXITOS_DUR_FUA)   != 0, "DUR_FUA has a name");
    T_OK(exitos_durability_str(EXITOS_DUR_FLUSH) != 0, "DUR_FLUSH has a name");

    /* Probing a device that does not exist must fail closed: assume the worst
     * (cache present, no FUA) so we never silently skip a needed flush. */
    int vwc = -1, fua = -1;
    T_OK(exitos_durability_probe("/dev/definitely-not-a-device", &vwc, &fua) != 0,
         "probe of a missing device returns an error");
    T_EQ(exitos_durability_policy_for("/dev/definitely-not-a-device"), EXITOS_DUR_FLUSH,
         "unknown device fails CLOSED to DUR_FLUSH, never DUR_NONE");

    T_DONE();
}
