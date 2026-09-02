#!/bin/sh
# The exec image-boundary guard is useful only if the loaded object exports the
# public libc ABIs.  This is a symbol-level actual-DSO check; transaction
# ordering and errors are covered by test_sync_lifecycle.c.
set -u
cd "$(dirname "$0")/../.." || exit 1

run=0
failed=0
ok(){ run=$((run+1)); echo "ok $run - $1"; }
bad(){ run=$((run+1)); failed=$((failed+1)); echo "not ok $run - $1"; }

for sym in execve execveat fexecve; do
    if readelf -Ws libexitos_preload.so | awk -v want="$sym" \
       '$7 != "UND" && ($8 == want || index($8, want "@") == 1) { found=1 }
        END { exit !found }'; then
        ok "preload DSO exports guarded $sym image-boundary ABI"
    else
        bad "preload DSO exports guarded $sym image-boundary ABI"
    fi
done

echo "1..$run  ($failed failed)"
[ "$failed" -eq 0 ]
