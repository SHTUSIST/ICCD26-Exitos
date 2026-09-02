#!/bin/bash
# Build the 2x2 table-lock/stats-counter A/B matrix from one exact source tree.
# No runtime knob is used: each artifact has one compile-time implementation,
# so the timed hot path cannot branch on the experimental arm.
set -euo pipefail

usage()
{
    echo "usage: $0 NEW_OUTPUT_DIRECTORY" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
OUT=$1
CC_BIN=${CC:-cc}
[ -n "$OUT" ] && [ "$OUT" != / ] || usage
[ ! -e "$OUT" ] || { echo "refusing existing output: $OUT" >&2; exit 3; }
CC_BIN=$(command -v "$CC_BIN") || { echo "compiler not found: ${CC:-cc}" >&2; exit 3; }
CC_BIN=$(readlink -f -- "$CC_BIN")
mkdir -m 0700 -- "$OUT"
OUT=$(cd "$OUT" && pwd -P)

SNAPSHOT=$OUT/source-snapshot
mkdir -m 0700 -- "$SNAPSHOT" "$SNAPSHOT/src" "$SNAPSHOT/include"
for source in "$ROOT"/src/*.c "$ROOT"/include/*.h; do
    [ -f "$source" ] && [ ! -L "$source" ] || {
        echo "unsafe source object: $source" >&2
        exit 3
    }
    case $source in
        "$ROOT"/src/*) relative=src/${source##*/} ;;
        "$ROOT"/include/*) relative=include/${source##*/} ;;
        *) exit 3 ;;
    esac
    install -m 0400 -- "$source" "$SNAPSHOT/$relative"
done
chmod 0500 -- "$SNAPSHOT/src" "$SNAPSHOT/include" "$SNAPSHOT"
exec {SNAPSHOT_FD}<"$SNAPSHOT" || {
    echo "cannot retain source snapshot: $SNAPSHOT" >&2
    exit 3
}
SNAPSHOT_FD_PATH=/proc/$$/fd/$SNAPSHOT_FD

assert_snapshot_identity()
{
    local path_identity fd_identity
    path_identity=$(stat -Lc '%d:%i:%u:%a' -- "$SNAPSHOT") || return 1
    fd_identity=$(stat -Lc '%d:%i:%u:%a' -- "$SNAPSHOT_FD_PATH") || return 1
    [ "$path_identity" = "$fd_identity" ] &&
        [ "$fd_identity" = "${fd_identity%:*}:500" ]
}

assert_snapshot_identity || {
    echo "source snapshot changed while retaining it" >&2
    exit 3
}

# The manifest order must be byte order, not the caller's locale order.  glibc
# collation under a UTF-8 locale ignores the underscore, so `sort` put
# include/exitos_donor_async.h before include/exitos_donor.h while every
# verifier -- the unit test and the independent check inside
# tools/fio-scale-pilot.sh -- compares against Python's byte ordering, which
# puts them the other way round.  The record lists then differed by ordering
# alone, the snapshot binding failed, and four downstream refusal assertions
# reported the wrong refusal reason.  LC_ALL=C pins the comparison.
snapshot_records()
{
    (cd "$SNAPSHOT_FD_PATH" && sha256sum src/*.c include/*.h | LC_ALL=C sort -k2)
}

SOURCE_HASHES_BEFORE=$(snapshot_records)
[ -n "$SOURCE_HASHES_BEFORE" ] || { echo "empty source snapshot" >&2; exit 3; }
SNAPSHOT_HASH_BEFORE=$(printf '%s\n' "$SOURCE_HASHES_BEFORE" | sha256sum | awk '{print $1}')

common=()
for source in "$SNAPSHOT_FD_PATH"/src/*.c; do
    case ${source##*/} in
        preload.c|bpftime_hook.c|bpftime_loader.c) ;;
        *) common+=("$source") ;;
    esac
done
[ "${#common[@]}" -gt 0 ] || { echo "no common sources" >&2; exit 3; }

records=$OUT/.artifact-records
: >"$records"

build_one()
{
    local table_mode=$1 stats_mode=$2
    local name=preload-table_${table_mode}-stats_${stats_mode}.so
    local defs=() defs_record=none

    if [ "$table_mode" = global ]; then
        defs+=(-DEXITOS_TABLE_GLOBAL_BASELINE)
        defs_record=EXITOS_TABLE_GLOBAL_BASELINE
    fi
    if [ "$stats_mode" = global ]; then
        defs+=(-DEXITOS_STATS_GLOBAL_BASELINE)
        if [ "$defs_record" = none ]; then
            defs_record=EXITOS_STATS_GLOBAL_BASELINE
        else
            defs_record=$defs_record+EXITOS_STATS_GLOBAL_BASELINE
        fi
    fi
    "$CC_BIN" -O2 -Wall -Wextra -D_GNU_SOURCE "${defs[@]}" \
        -fPIC -shared -I"$SNAPSHOT_FD_PATH/include" "${common[@]}" \
        "$SNAPSHOT_FD_PATH/src/preload.c" -Wl,-z,now -Wl,-Bsymbolic \
        -o "$OUT/$name" -ldl -lpthread
    printf 'artifact=%s table=%s stats=%s defs=%s sha256=%s\n' \
        "$name" "$table_mode" "$stats_mode" "$defs_record" \
        "$(sha256sum "$OUT/$name" | awk '{print $1}')" >> "$records"
}

build_one global global
build_one global sharded
build_one sharded global
build_one sharded sharded

for library in "$OUT"/*.so; do
    env -u EXITOS_FILES -u EXITOS_STATS LD_PRELOAD="$library" /bin/true
    chmod 0500 -- "$library"
done

SOURCE_HASHES_AFTER=$(snapshot_records)
[ "$SOURCE_HASHES_AFTER" = "$SOURCE_HASHES_BEFORE" ] || {
    echo "source snapshot changed during build" >&2
    exit 3
}
SNAPSHOT_HASH_AFTER=$(printf '%s\n' "$SOURCE_HASHES_AFTER" | sha256sum | awk '{print $1}')
[ "$SNAPSHOT_HASH_AFTER" = "$SNAPSHOT_HASH_BEFORE" ] || exit 3
assert_snapshot_identity || {
    echo "source snapshot pathname changed during build" >&2
    exit 3
}

manifest_tmp=$OUT/.MANIFEST.txt.tmp
manifest=$OUT/MANIFEST.txt
{
    echo "format=exitos-scale-variants-v2"
    echo "purpose=same-source table-lock x stats-counter A/B"
    echo "snapshot_dir=source-snapshot"
    echo "compiler_path=$CC_BIN"
    echo "compiler_sha256=$(sha256sum "$CC_BIN" | awk '{print $1}')"
    echo "compiler_version=$($CC_BIN --version | head -n 1)"
    echo "common_flags=-O2,-Wall,-Wextra,-D_GNU_SOURCE,-fPIC,-shared,-I(source-snapshot/include),-Wl:z:now,-Wl:Bsymbolic,-ldl,-lpthread"
    echo "switch_table_global=-DEXITOS_TABLE_GLOBAL_BASELINE"
    echo "switch_stats_global=-DEXITOS_STATS_GLOBAL_BASELINE"
    echo "snapshot_hash_before=$SNAPSHOT_HASH_BEFORE"
    echo "source_hashes_begin"
    printf '%s\n' "$SOURCE_HASHES_BEFORE"
    echo "source_hashes_end"
    cat "$records"
    echo "snapshot_hash_after=$SNAPSHOT_HASH_AFTER"
} >"$manifest_tmp"
chmod 0400 -- "$manifest_tmp"
mv -- "$manifest_tmp" "$manifest"
rm -- "$records"
echo "VARIANTS_OK output=$OUT manifest=$manifest"
