/* Tier 2: confirm the policy matches what the real hardware reports. */
#include "tap.h"
#include <stdlib.h>
#include <stdio.h>
#include "exitos_durability.h"
int main(void){
    const char *dev = getenv("EXITOS_DEV");
    if(!dev){ T_SKIP("EXITOS_DEV unset; tier 2 is opt-in"); printf("1..0  (0 failed)\n"); return 0; }
    int vol=-1, fua=-1;
    T_EQ(exitos_durability_probe(dev,&vol,&fua), 0, "probe %s succeeded", dev);
    printf("# %s: volatile_write_cache=%d fua=%d -> %s\n", dev, vol, fua,
           exitos_durability_str(exitos_durability_policy_for(dev)));
    T_OK(vol==0||vol==1, "volatile cache flag is a clean boolean");
    exitos_durability d = exitos_durability_policy_for(dev);
    if(vol==0) T_EQ(d, EXITOS_DUR_NONE, "no volatile cache -> policy is DUR_NONE");
    else       T_OK(d==EXITOS_DUR_FUA||d==EXITOS_DUR_FLUSH, "cache present -> FUA or FLUSH");
    T_DONE();
}
