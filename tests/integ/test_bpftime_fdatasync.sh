#!/bin/bash
# Backend 2 is deliberately unavailable.  This integration contract proves the
# shipped DSO reports the freeze and that an unmodified pwrite+fdatasync
# workload remains entirely on the kernel path with zero takeover counters.
# It uses ordinary temporary files only: no loop/raw/NVMe device is touched.
set -u
cd "$(dirname "$0")/../.." || exit 1

n=0
fail=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fail=1; echo "not ok $n - $*"; }

base="/tmp/exitos-bpftime-frozen-$$"
probe="$base.probe"
writer="$base.writer"
ref="$base.ref"
out="$base.out"
stats="$base.stats"
P2="$PWD/libexitos_bpftime.so"
cleanup() { rm -f "$probe" "$writer" "$ref" "$out" "$stats"; }
trap cleanup EXIT HUP INT TERM

if gcc -O2 -Wall -Wextra -Werror tests/integ/bpftime_frozen_probe.c \
       -o "$probe" -ldl &&
   gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE tests/integ/writer_libc.c \
       -o "$writer"; then
    ok "built frozen-backend probe and unmodified writer"
else
    nok "built frozen-backend probe and unmodified writer"
    echo "1..$n  ($fail failed)"
    exit 1
fi

if "$probe" "$P2" >/dev/null; then
    ok "production DSO has all claims/table bytes zero and start=-EOPNOTSUPP"
else
    rc=$?
    nok "production DSO has all claims/table bytes zero and start=-EOPNOTSUPP (rc=$rc)"
fi

if "$writer" "$ref" 32 >/dev/null 2>&1; then
    ok "reference fdatasync writer completed"
else
    nok "reference fdatasync writer completed"
fi
ref_md=$(md5sum "$ref" 2>/dev/null | awk '{print $1}')

rm -f "$stats" "$out"
if env LD_PRELOAD="$P2" EXITOS_BPFTIME_SO="$P2" \
       EXITOS_FILES="$(basename "$out")" EXITOS_STATS="$stats" \
       "$writer" "$out" 32 >/dev/null 2>&1; then
    ok "frozen bpftime DSO leaves unmodified fdatasync workload on kernel path"
else
    nok "frozen bpftime DSO leaves unmodified fdatasync workload on kernel path"
fi

out_md=$(md5sum "$out" 2>/dev/null | awk '{print $1}')
if [ -n "$ref_md" ] && [ "$out_md" = "$ref_md" ]; then
    ok "frozen-backend output is byte-identical to reference"
else
    nok "frozen-backend output is byte-identical to reference"
fi

if [ -s "$stats" ]; then
    ok "frozen backend publishes an explicit stats file"
else
    nok "frozen backend publishes an explicit stats file"
fi

fast_write=$(sed -n '1p' "$stats" 2>/dev/null || echo MISSING)
fast_sync=$(sed -n '2p' "$stats" 2>/dev/null || echo MISSING)
[ "$fast_write" = 0 ] \
    && ok "frozen backend reports zero write takeovers" \
    || nok "frozen backend reports zero write takeovers (got $fast_write)"
[ "$fast_sync" = 0 ] \
    && ok "frozen backend reports zero sync takeovers" \
    || nok "frozen backend reports zero sync takeovers (got $fast_sync)"

if awk 'NF != 1 || $1 != 0 { bad=1 } END { exit NR == 0 || bad }' "$stats" 2>/dev/null; then
    ok "every frozen-backend statistic remains zero"
else
    nok "every frozen-backend statistic remains zero"
fi

echo "1..$n  ($fail failed)"
[ "$fail" -eq 0 ]
