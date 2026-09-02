#!/bin/sh
# A unit test that dlopens/LD_PRELOADs a production DSO must not be allowed to
# consume a stale file.  These dry-run probes mark one DSO source as newer and
# require the unit dependency graph to schedule the matching relink.
set -u

cd "$(dirname "$0")/../.." || exit 1
base="/tmp/exitos-unit-dso-deps-$$"
trap 'rm -f "$base.bpftime" "$base.preload"' EXIT HUP INT TERM

run=0
failed=0
ok(){ run=$((run + 1)); echo "ok $run - $1"; }
bad(){ run=$((run + 1)); failed=$((failed + 1)); echo "not ok $run - $1"; }

if make -n -W src/bpftime_hook.c unit >"$base.bpftime" 2>&1 &&
   grep -Fq -- '-o libexitos_bpftime.so' "$base.bpftime"; then
    ok "unit relinks the bpftime production DSO when its source is stale"
else
    bad "unit relinks the bpftime production DSO when its source is stale"
fi

if make -n -W src/preload.c unit >"$base.preload" 2>&1 &&
   grep -Fq -- '-o libexitos_preload.so' "$base.preload"; then
    ok "unit relinks the preload production DSO when its source is stale"
else
    bad "unit relinks the preload production DSO when its source is stale"
fi

echo "1..$run  ($failed failed)"
[ "$failed" -eq 0 ]
