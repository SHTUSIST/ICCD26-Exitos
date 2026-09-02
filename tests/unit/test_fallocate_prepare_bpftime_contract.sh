#!/bin/bash
# Device-free wiring contract for the optional active-bpftime arm.  The active
# Exitos DSO is the loader; it in turn receives bpftime's patched transformer
# through EXITOS_BPFTIME_SO.  Reversing these two libraries makes the opt-in
# arm test a different mechanism from the public runner.
set -u

cd "$(dirname "$0")/../.." || exit 1
SCRIPT=tests/integ/test_fallocate_prepare_e2e.sh

n=0
fail=0
ok()  { n=$((n + 1)); printf 'ok %d - %s\n' "$n" "$*"; }
nok() { n=$((n + 1)); fail=$((fail + 1)); printf 'not ok %d - %s\n' "$n" "$*"; }

if grep -Fq 'LD_PRELOAD="$ACTIVE" EXITOS_BPFTIME_SO="$TRANSFORMER"' "$SCRIPT"; then
    ok "active DSO is the LD_PRELOAD loader and transformer is EXITOS_BPFTIME_SO"
else
    nok "active DSO is the LD_PRELOAD loader and transformer is EXITOS_BPFTIME_SO"
fi

if ! grep -Fq 'LD_PRELOAD="$TRANSFORMER" EXITOS_BPFTIME_SO="$ACTIVE"' "$SCRIPT"; then
    ok "reversed active-bpftime wiring is absent"
else
    nok "reversed active-bpftime wiring is absent"
fi

if grep -Fq 'EXITOS_TEST_ACTIVE_BPFTIME:-0' "$SCRIPT" &&
   grep -Fq '[ ! -r "$TRANSFORMER" ]' "$SCRIPT" &&
   grep -Fq '[ ! -r "$ACTIVE" ]' "$SCRIPT"; then
    ok "active bpftime remains explicit opt-in and missing libraries fail closed"
else
    nok "active bpftime remains explicit opt-in and missing libraries fail closed"
fi

if grep -Fq '$(FALLOCATE_PREPARE_PROBE)' Makefile &&
   sed -n '/^clean:/p' Makefile | grep -Fq '$(FALLOCATE_PREPARE_PROBE)'; then
    ok "Makefile clean removes the integration helper"
else
    nok "Makefile clean removes the integration helper"
fi

printf '1..%d  (%d failed)\n' "$n" "$fail"
[ "$fail" -eq 0 ]
