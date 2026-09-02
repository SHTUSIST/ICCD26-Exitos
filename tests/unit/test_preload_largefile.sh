#!/bin/sh
set -u
cd "$(dirname "$0")/../.." || exit 1

base="/tmp/exitos-preload-largefile-$$"
helper="$base.helper"
target="$base.data"
trap 'rm -f "$helper" "$target"' EXIT HUP INT TERM
run=0
failed=0
ok(){ run=$((run+1)); echo "ok $run - $1"; }
bad(){ run=$((run+1)); failed=$((failed+1)); echo "not ok $run - $1"; }

if cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -D_LARGEFILE64_SOURCE=1 \
      -D_FILE_OFFSET_BITS=64 tests/unit/largefile_truncate_helper.c -o "$helper"; then
    ok "compiled independent large-file application TU"
else
    bad "compiled independent large-file application TU"
    echo "1..$run  ($failed failed)"
    exit 1
fi

if nm -u "$helper" | grep -q ' ftruncate64@\| ftruncate64$'; then
    ok "large-file TU has a visible ftruncate64 ABI dependency"
else
    bad "large-file TU has a visible ftruncate64 ABI dependency"
fi

if readelf -Ws libexitos_preload.so | awk \
   '$7 != "UND" && ($8 == "ftruncate64" || $8 ~ /^ftruncate64@/) { found=1 } END { exit !found }'; then
    ok "preload DSO exports ftruncate64"
else
    bad "preload DSO exports ftruncate64"
fi

: > "$target"
if EXITOS_FILES="$target" LD_PRELOAD="$PWD/libexitos_preload.so" \
     "$helper" 64 "$target"; then
    ok "explicit ftruncate64 application call completes through loaded DSO"
else
    bad "explicit ftruncate64 application call completes through loaded DSO"
fi

echo "1..$run  ($failed failed)"
[ "$failed" -eq 0 ]
