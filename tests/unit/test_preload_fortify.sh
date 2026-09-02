#!/bin/sh
# Device-free regression for the glibc fortify open ABI.  The helper is a
# separate optimized application translation unit, so nm must show that libc
# emitted __open_2/__open64_2/__openat_2/__openat64_2 rather than the ordinary
# variadic entry points.
set -u

cd "$(dirname "$0")/../.." || exit 1
tmp_base="/tmp/exitos-preload-fortify-$$"
helper="$tmp_base.helper"
target="$tmp_base.data"
out="$tmp_base.out"
trap 'rm -f "$helper" "$target" "$out" "$tmp_base.create"; rmdir "$tmp_base.dir" 2>/dev/null || true' EXIT HUP INT TERM

run=0
failed=0
ok()
{
    run=$((run + 1))
    echo "ok $run - $1"
}
bad()
{
    run=$((run + 1))
    failed=$((failed + 1))
    echo "not ok $run - $1"
}

cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 \
   tests/unit/fortify_open_helper.c -o "$helper" || {
    bad "compiled the optimized fortify helper"
    echo "1..$run  ($failed failed)"
    exit 1
}
ok "compiled the optimized fortify helper"

for sym in __open_2 __open64_2 __openat_2 __openat64_2; do
    if nm -u "$helper" | grep -q " $sym@\| $sym$"; then
        ok "ordinary optimized build references $sym"
    else
        bad "ordinary optimized build references $sym"
    fi
done

: > "$target"
for api in open open64 openat openat64; do
    if EXITOS_FILES="$target" EXITOS_VERBOSE=1 \
       LD_PRELOAD="$PWD/libexitos_preload.so" \
       "$helper" "$api" "$target" normal >"$out" 2>&1 &&
       grep -Fq "register fd=" "$out"; then
        ok "$api fortified ABI reaches the common registration path"
    else
        bad "$api fortified ABI reaches the common registration path"
        sed 's/^/# /' "$out"
    fi
done

# __open*_2 must retain libc fortify's fail-fast contract when a mode is
# required but absent.  Forwarding to variadic open with an invented mode (or
# reading a nonexistent vararg) would make these calls continue instead.
for kind in create tmpfile; do
    bad_path="$tmp_base.create"
    if [ "$kind" = tmpfile ]; then
        mkdir -p "$tmp_base.dir"
        bad_path="$tmp_base.dir"
    fi
    "$helper" open "$bad_path" "$kind" >/dev/null 2>&1
    base_rc=$?
    EXITOS_FILES="$bad_path" LD_PRELOAD="$PWD/libexitos_preload.so" \
        "$helper" open "$bad_path" "$kind" >/dev/null 2>&1
    preload_rc=$?
    if [ "$base_rc" -ne 0 ] && [ "$preload_rc" -eq "$base_rc" ]; then
        ok "fortified $kind-without-mode keeps libc fail-fast status ($base_rc)"
    else
        bad "fortified $kind-without-mode keeps libc fail-fast status (baseline $base_rc, preload $preload_rc)"
    fi
done

echo "1..$run  ($failed failed)"
[ "$failed" -eq 0 ]
