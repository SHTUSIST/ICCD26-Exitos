#!/bin/sh
set -u
cd "$(dirname "$0")/../.." || exit 1
base="/tmp/exitos-bpftime-dso-contract-$$"
helper="$base.helper"
marker="$base.marker.so"
sentinel="$base.ctor-ran"
trap 'rm -f "$helper" "$marker" "$sentinel"' EXIT HUP INT TERM
run=0
failed=0
ok(){ run=$((run+1)); echo "ok $run - $1"; }
bad(){ run=$((run+1)); failed=$((failed+1)); echo "not ok $run - $1"; }

if cc -O2 -Wall -Wextra -Werror tests/unit/bpftime_dso_contract_helper.c \
      -o "$helper" -ldl; then
    ok "compiled actual-DSO contract helper"
else
    bad "compiled actual-DSO contract helper"
    echo "1..$run  ($failed failed)"
    exit 1
fi

if cc -O2 -Wall -Wextra -Werror -fPIC -shared \
      tests/unit/bpftime_marker_so.c -o "$marker"; then
    ok "compiled an existing marker DSO with observable constructor"
else
    bad "compiled an existing marker DSO with observable constructor"
    echo "1..$run  ($failed failed)"
    exit 1
fi

rm -f "$sentinel"
if EXITOS_BPFTIME_MARKER_SENTINEL="$sentinel" LD_PRELOAD="$marker" \
      /bin/true && [ -s "$sentinel" ]; then
    ok "marker DSO constructor creates its sentinel when actually loaded"
else
    bad "marker DSO constructor creates its sentinel when actually loaded"
fi
rm -f "$sentinel"

for sym in exitos_internal_open_call exitos_internal_openat_call \
           exitos_internal_close_call exitos_internal_ftruncate_call \
           exitos_internal_fallocate_call exitos_internal_pwrite_call \
           exitos_internal_fdatasync_call; do
    if readelf -Ws libexitos_bpftime.so | awk -v want="$sym" \
       '$5 == "GLOBAL" && $7 != "UND" && $8 == want { found=1 } END { exit !found }'; then
        ok "bpftime DSO provides strong $sym"
    else
        bad "bpftime DSO provides strong $sym"
    fi
done

if "$helper" "$PWD/libexitos_bpftime.so" "$marker" "$sentinel"; then
    ok "start(existing marker) stays frozen without dlopen, ctor, or maps entry"
else
    rc=$?
    bad "start(existing marker) stays frozen without dlopen, ctor, or maps entry (rc=$rc)"
fi

# The integration script also carries a loop-device E2E mode.  Tier 0 must run
# only its early-exit frozen-DSO contract, with the mode explicit in the unit
# recipe so a root test process cannot accidentally fall through to loop setup.
if make -n unit 2>/dev/null | grep -Fq \
      'EXITOS_BPFTIME_CONTRACT_ONLY=1 bash ./tests/integ/test_backends_e2e.sh'; then
    ok "default unit target runs backend E2E contract-only mode explicitly"
else
    bad "default unit target runs backend E2E contract-only mode explicitly"
fi

echo "1..$run  ($failed failed)"
[ "$failed" -eq 0 ]
