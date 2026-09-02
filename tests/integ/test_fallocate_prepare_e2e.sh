#!/bin/bash
# End-to-end proof for the setup-time native-fallocate donor takeover.
#
# The fixture is a private ext4 filesystem on a loop device.  This test never
# names, opens, formats, mounts, or writes any physical block device.  Content
# correctness is established once, after the write+sync operation, by reading
# the whole file through its ordinary pathname and comparing every byte.  No
# checksum or hash is computed in or around the timed operation.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD
PROBE="$ROOT/tests/integ/fallocate_prepare_probe"
PRELOAD="$ROOT/libexitos_preload.so"
FILE_BYTES=65536

n=0
fail=0
ok()  { n=$((n + 1)); printf 'ok %d - %s\n' "$n" "$*"; }
nok() { n=$((n + 1)); fail=1; printf 'not ok %d - %s\n' "$n" "$*"; }
finish() { printf '1..%d  (%d failed)\n' "$n" "$fail"; [ "$fail" -eq 0 ]; }
skip_all() { printf '1..0 # SKIP %s\n' "$*"; exit 0; }

[ -x "$PROBE" ] || {
    nok "native-fallocate integration probe is built"
    finish
    exit 1
}
[ -r "$PRELOAD" ] || {
    nok "LD_PRELOAD frontend is built"
    finish
    exit 1
}
[ "$(id -u)" -eq 0 ] || skip_all "needs root for an owned loop/ext4 fixture"
for command_name in losetup mkfs.ext4 mount umount mountpoint findmnt stat readlink \
                    truncate mktemp awk sed; do
    command -v "$command_name" >/dev/null 2>&1 ||
        skip_all "missing required command: $command_name"
done

WORK=$(mktemp -d /tmp/exitos-fallocate-e2e.XXXXXX) ||
    skip_all "cannot create private fixture directory"
IMG="$WORK/loop.img"
MNT="$WORK/mnt"
NATIVE_LOG="$WORK/native.log"
NATIVE_INSPECT_LOG="$WORK/native-inspect.log"
PRELOAD_LOG="$WORK/preload.log"
PRELOAD_STATS="$WORK/preload.stats"
ACTIVE_LOG="$WORK/bpftime.log"
ACTIVE_STATS="$WORK/bpftime.stats"
LOOP=""

loop_is_ours()
{
    local backing expected
    [ -n "$LOOP" ] && [ -b "$LOOP" ] || return 1
    case "$LOOP" in /dev/loop*) ;; *) return 1 ;; esac
    backing=$(losetup -n -O BACK-FILE "$LOOP" 2>/dev/null | sed -n '1p') ||
        return 1
    expected=$(readlink -f -- "$IMG") || return 1
    [ "$(readlink -f -- "$backing" 2>/dev/null)" = "$expected" ]
}

mount_is_ours()
{
    local source source_id loop_id
    mountpoint -q "$MNT" 2>/dev/null || return 1
    source=$(findmnt -n -o SOURCE --target "$MNT" 2>/dev/null) || return 1
    source_id=$(stat -Lc '%t:%T' "$source" 2>/dev/null) || return 1
    loop_id=$(stat -Lc '%t:%T' "$LOOP" 2>/dev/null) || return 1
    [ "$source_id" = "$loop_id" ]
}

cleanup()
{
    if mount_is_ours; then
        umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    fi
    if loop_is_ours; then
        losetup -d "$LOOP" 2>/dev/null || true
    fi
    rm -f -- "$IMG" "$NATIVE_LOG" "$NATIVE_INSPECT_LOG" \
        "$PRELOAD_LOG" "$PRELOAD_STATS" "$ACTIVE_LOG" "$ACTIVE_STATS"
    rmdir -- "$MNT" 2>/dev/null || true
    rmdir -- "$WORK" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

mkdir "$MNT" || skip_all "cannot create private mount point"
truncate -s 128M "$IMG" || skip_all "cannot size private loop image"
LOOP=$(losetup --find --show --nooverlap "$IMG" 2>/dev/null) ||
    skip_all "cannot allocate a loop device"
loop_is_ours || skip_all "loop backing identity does not match private image"
mkfs.ext4 -q -F -b 4096 "$LOOP" || skip_all "cannot format owned loop device"
mount -o data=ordered "$LOOP" "$MNT" ||
    skip_all "cannot mount owned loop device"
mount_is_ours || skip_all "mounted source is not the owned loop device"
ok "safety gate proves the ext4 fixture is backed by owned $LOOP"

NATIVE_FILE="$MNT/native-default.dat"
if env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_PREPARE \
       -u EXITOS_DONOR_DIR -u EXITOS_DONOR_FILES -u EXITOS_FASTPATH \
       -u EXITOS_IOPATH -u EXITOS_STATS -u EXITOS_DEV \
       -u EXITOS_EXPECT_SERIAL \
       "$PROBE" native "$NATIVE_FILE" "$FILE_BYTES" \
       >"$NATIVE_LOG" 2>&1; then
    ok "native mode-0 fallocate probe completed before any data write"
else
    nok "native mode-0 fallocate probe completed before any data write"
fi
if awk -v bytes="$FILE_BYTES" '
    /^NATIVE_ALLOCATED / {
        size = blocks = -1
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^size=/) { split($i, a, "="); size = a[2] + 0 }
            if ($i ~ /^allocated=/) { split($i, a, "="); blocks = a[2] + 0 }
        }
        if (size == bytes && blocks >= bytes) good = 1
    }
    END { exit !good }
' "$NATIVE_LOG"; then
    ok "native fallocate assigned at least $FILE_BYTES physical bytes"
else
    nok "native fallocate assigned at least $FILE_BYTES physical bytes"
fi
if awk -v bytes="$FILE_BYTES" '
    /^NATIVE_FIEMAP / {
        mapped = unwritten = complete = -1
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^mapped=/) { split($i, a, "="); mapped = a[2] + 0 }
            if ($i ~ /^all_unwritten=/) { split($i, a, "="); unwritten = a[2] + 0 }
            if ($i ~ /^complete=/) { split($i, a, "="); complete = a[2] + 0 }
        }
        if (mapped == bytes && unwritten == 1 && complete == 1) good = 1
    }
    END { exit !good }
' "$NATIVE_LOG"; then
    ok "native allocation is complete but every extent is FIEMAP UNWRITTEN"
else
    nok "native allocation is complete but every extent is FIEMAP UNWRITTEN"
fi
awk '/^NATIVE_(ALLOCATED|FIEMAP) / { print "# native evidence: " $0 }' \
    "$NATIVE_LOG"

if env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_PREPARE \
       -u EXITOS_DONOR_DIR -u EXITOS_DONOR_FILES -u EXITOS_FASTPATH \
       -u EXITOS_IOPATH -u EXITOS_STATS -u EXITOS_DEV \
       -u EXITOS_EXPECT_SERIAL \
       "$PROBE" inspect-existing-native "$NATIVE_FILE" "$FILE_BYTES" \
       >"$NATIVE_INSPECT_LOG" 2>&1; then
    ok "read-only oracle accepts an existing native-fallocated file"
else
    nok "read-only oracle accepts an existing native-fallocated file"
fi
if awk -v bytes="$FILE_BYTES" '
    /^NATIVE_EXISTING_ALLOCATED / {
        size = blocks = -1
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^size=/) { split($i, a, "="); size = a[2] + 0 }
            if ($i ~ /^allocated=/) { split($i, a, "="); blocks = a[2] + 0 }
        }
        if (size == bytes && blocks >= bytes) allocated = 1
    }
    /^NATIVE_EXISTING_FIEMAP / {
        mapped = unwritten = complete = physical = unsafe = -1
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^mapped=/) { split($i, a, "="); mapped = a[2] + 0 }
            if ($i ~ /^all_unwritten=/) { split($i, a, "="); unwritten = a[2] + 0 }
            if ($i ~ /^complete=/) { split($i, a, "="); complete = a[2] + 0 }
            if ($i ~ /^physical_nonzero=/) { split($i, a, "="); physical = a[2] + 0 }
            if ($i ~ /^non_unwritten_unsafe=/) { split($i, a, "="); unsafe = a[2] + 0 }
        }
        if (mapped == bytes && unwritten == 1 && complete == 1 &&
            physical == 1 && unsafe == 0) mapped_ok = 1
    }
    END { exit !(allocated && mapped_ok) }
' "$NATIVE_INSPECT_LOG"; then
    ok "existing-file oracle proves allocated physical space is complete UNWRITTEN"
else
    nok "existing-file oracle proves allocated physical space is complete UNWRITTEN"
fi
awk '/^NATIVE_EXISTING_(ALLOCATED|FIEMAP) / {
         print "# existing native evidence: " $0
     }' "$NATIVE_INSPECT_LOG"

check_stats()
{
    local stats=$1 label=$2
    if awk '
        NR == 1 { good = good || $1 == 1 }
        NR == 2 { good = good && $1 == 1 }
        NR == 3 { good = good && $1 == 0 }
        NR == 4 { good = good && $1 == 0 }
        NR == 5 { good = good && $1 == 1 }
        NF != 1 { bad = 1 }
        END { exit !(NR == 5 && good && !bad) }
    ' "$stats" 2>/dev/null; then
        ok "$label counters are FAST_WRITE=1 FAST_SYNC=1 PASS=0 DECLINED=0 REFRESH=1"
    else
        nok "$label counters are FAST_WRITE=1 FAST_SYNC=1 PASS=0 DECLINED=0 REFRESH=1"
    fi
}

check_intercept_log()
{
    local log=$1 frontend=$2 label=$3
    local ready_count

    if awk '
        /^APP_SETUP_OPEN_RETURN / {
            setup++
            if ($0 ~ / access=wronly$/) setup_ok = 1
        }
        /^APP_SETUP_CLOSE_RETURN rc=0$/ { setup_close++ }
        /^APP_IO_OPEN_RETURN / {
            io++
            if ($0 ~ / access=rdwr direct=1$/) io_ok = 1
        }
        END {
            exit !(setup == 1 && setup_ok && setup_close == 1 &&
                   io == 1 && io_ok)
        }
    ' "$log"; then
        ok "$label used fio-shaped O_WRONLY setup then O_RDWR|O_DIRECT I/O reopen"
    else
        nok "$label used fio-shaped O_WRONLY setup then O_RDWR|O_DIRECT I/O reopen"
    fi

    ready_count=$(awk -v frontend="$frontend" '
        $1 == "PREPARE_READY" && $2 == "frontend=" frontend { n++ }
        END { print n + 0 }
    ' "$log")
    if [ "$ready_count" -eq 1 ] &&
       awk -v frontend="$frontend" -v bytes="$FILE_BYTES" '
        $1 == "PREPARE_READY" && $2 == "frontend=" frontend {
            mode = off = len = prepared = safe = unsafe = -1
            for (i = 3; i <= NF; i++) {
                if ($i ~ /^mode=/) { split($i, a, "="); mode = a[2] + 0 }
                if ($i ~ /^off=/) { split($i, a, "="); off = a[2] + 0 }
                if ($i ~ /^len=/) { split($i, a, "="); len = a[2] + 0 }
                if ($i ~ /^prepared=/) { split($i, a, "="); prepared = a[2] + 0 }
                if ($i == "fiemap=safe") safe = 1
                if ($i == "unsafe_flags=0") unsafe = 0
            }
            if (mode == 0 && off == 0 && len == bytes &&
                prepared == bytes && safe == 1 && unsafe == 0) good = 1
        }
        END { exit !good }
    ' "$log"; then
        ok "$label emitted one exact PREPARE_READY with independent-safe contract"
    else
        nok "$label emitted one exact PREPARE_READY with independent-safe contract"
    fi

    if awk -v frontend="$frontend" -v bytes="$FILE_BYTES" '
        $1 == "PREPARE_OUTCOME" && $2 == "v=1" &&
        $3 == "frontend=" frontend {
            n++
            mode = off = len = rc = ret = reterr = alias = prepared = chunks = -1
            outcome = stage = ""
            for (i = 4; i <= NF; i++) {
                if ($i ~ /^mode=/) { split($i, a, "="); mode = a[2] + 0 }
                if ($i ~ /^off=/) { split($i, a, "="); off = a[2] + 0 }
                if ($i ~ /^len=/) { split($i, a, "="); len = a[2] + 0 }
                if ($i ~ /^outcome=/) { split($i, a, "="); outcome = a[2] }
                if ($i ~ /^stage=/) { split($i, a, "="); stage = a[2] }
                if ($i ~ /^rc=/) { split($i, a, "="); rc = a[2] + 0 }
                if ($i ~ /^return_rc=/) { split($i, a, "="); ret = a[2] + 0 }
                if ($i ~ /^return_errno=/) { split($i, a, "="); reterr = a[2] + 0 }
                if ($i ~ /^rdwr_alias=/) { split($i, a, "="); alias = a[2] + 0 }
                if ($i ~ /^prepared=/) { split($i, a, "="); prepared = a[2] + 0 }
                if ($i ~ /^chunks=/) { split($i, a, "="); chunks = a[2] + 0 }
            }
            if (mode == 0 && off == 0 && len == bytes &&
                outcome == "prepared" && stage == "complete" &&
                rc == 0 && ret == 0 && reterr == 0 && alias == 1 &&
                prepared == bytes && chunks > 0) good++
        }
        END { exit !(n == 1 && good == 1) }
    ' "$log"; then
        ok "$label emitted one successful v1 outcome proving O_WRONLY alias adaptation"
    else
        nok "$label emitted one successful v1 outcome proving O_WRONLY alias adaptation"
    fi

    if awk '
        /^APP_SETUP_OPEN_RETURN / && !opened { opened = NR }
        /^APP_FALLOCATE_CALL / && !called { called = NR }
        /^PREPARE_OUTCOME / && !outcome { outcome = NR }
        /^PREPARE_READY / && !ready { ready = NR }
        /^APP_FALLOCATE_RETURN / && !returned { returned = NR }
        /^APP_SETUP_CLOSE_RETURN / && !closed { closed = NR }
        /^APP_IO_OPEN_RETURN / && !reopened { reopened = NR }
        /^APP_FIEMAP_SAFE / && !mapped { mapped = NR }
        /^APP_TIMED_BEGIN / && !timed { timed = NR }
        /^APP_PWRITE_RETURN / && !wrote { wrote = NR }
        /^APP_FDATASYNC_RETURN / && !synced { synced = NR }
        END {
            exit !(opened && called && outcome && ready && returned && closed &&
                   reopened && mapped && timed && wrote && synced &&
                   opened < called && called < outcome && outcome < ready &&
                   ready < returned &&
                   returned < closed && closed < reopened && reopened < mapped &&
                   mapped < timed && timed < wrote && wrote < synced)
        }
    ' "$log"; then
        ok "$label completed donor proof before setup close and reopened only before timed I/O"
    else
        nok "$label completed donor proof before setup close and reopened only before timed I/O"
    fi

    if awk -v bytes="$FILE_BYTES" '
        /^APP_FIEMAP_SAFE / {
            mapped = complete = unsafe = -1
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^mapped=/) { split($i, a, "="); mapped = a[2] + 0 }
                if ($i ~ /^complete=/) { split($i, a, "="); complete = a[2] + 0 }
                if ($i ~ /^unsafe_flags=/) { split($i, a, "="); unsafe = a[2] + 0 }
            }
            if (mapped == bytes && complete == 1 && unsafe == 0) good = 1
        }
        END { exit !good }
    ' "$log"; then
        ok "$label application independently observed no hole/unwritten/unsafe extent before its first write"
    else
        nok "$label application independently observed no hole/unwritten/unsafe extent before its first write"
    fi
}

run_preload_arm()
{
    local target="$MNT/preload-prepared.dat"
    local donor_dir="$MNT/donors-preload"
    mkdir "$donor_dir" || return 1
    chmod 700 "$donor_dir" || return 1

    if env -u EXITOS_DEV -u EXITOS_EXPECT_SERIAL \
           LD_PRELOAD="$PRELOAD" EXITOS_FILES="$target" \
           EXITOS_PREPARE=donor-fallocate EXITOS_DONOR_DIR="$donor_dir" \
           EXITOS_DONOR_FILES=1 EXITOS_FASTPATH=1 EXITOS_IOPATH=pwrite \
           EXITOS_STATS="$PRELOAD_STATS" \
           "$PROBE" workload "$target" "$FILE_BYTES" \
           >"$PRELOAD_LOG" 2>&1; then
        ok "LD_PRELOAD ran the fio descriptor-shaped native-fallocate workload"
    else
        nok "LD_PRELOAD ran the fio descriptor-shaped native-fallocate workload"
    fi
    check_intercept_log "$PRELOAD_LOG" ld_preload LD_PRELOAD
    check_stats "$PRELOAD_STATS" LD_PRELOAD
    awk '/^(PREPARE_READY|PREPARE_FAILED|PREPARE_OUTCOME|APP_SETUP_OPEN_RETURN|APP_SETUP_CLOSE_RETURN|APP_IO_OPEN_RETURN|APP_FIEMAP_SAFE|workload fallocate:) / {
             print "# LD_PRELOAD evidence: " $0
         }' "$PRELOAD_LOG"

    if env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_PREPARE \
           -u EXITOS_DONOR_DIR -u EXITOS_DONOR_FILES -u EXITOS_FASTPATH \
           -u EXITOS_IOPATH -u EXITOS_STATS \
           "$PROBE" verify "$target" "$FILE_BYTES" >>"$PRELOAD_LOG" 2>&1; then
        ok "LD_PRELOAD result passes one complete ordinary-path byte-for-byte readback"
        awk '/^READBACK_OK / { print "# LD_PRELOAD evidence: " $0 }' \
            "$PRELOAD_LOG"
    else
        nok "LD_PRELOAD result passes one complete ordinary-path byte-for-byte readback"
    fi
}

run_preload_arm

# Active bpftime preloads the active Exitos DSO, which loads the locally patched
# instruction transformer named by EXITOS_BPFTIME_SO.  It is deliberately
# opt-in; production make integ never acquires this external dependency.
if [ "${EXITOS_TEST_ACTIVE_BPFTIME:-0}" = 1 ]; then
    TRANSFORMER=${EXITOS_BPFTIME_TRANSFORMER:-}
    ACTIVE="$ROOT/libexitos_bpftime_active.so"
    ACTIVE_TARGET="$MNT/bpftime-prepared.dat"
    ACTIVE_DONORS="$MNT/donors-bpftime"
    if [ -z "$TRANSFORMER" ] || [ ! -r "$TRANSFORMER" ] || [ ! -r "$ACTIVE" ]; then
        nok "active bpftime opt-in requires readable transformer=$TRANSFORMER and active DSO=$ACTIVE"
    else
        mkdir "$ACTIVE_DONORS" && chmod 700 "$ACTIVE_DONORS"
        if env -u EXITOS_DEV -u EXITOS_EXPECT_SERIAL \
               LD_PRELOAD="$ACTIVE" EXITOS_BPFTIME_SO="$TRANSFORMER" \
               EXITOS_FILES="$ACTIVE_TARGET" \
               EXITOS_PREPARE=donor-fallocate EXITOS_DONOR_DIR="$ACTIVE_DONORS" \
               EXITOS_DONOR_FILES=1 EXITOS_FASTPATH=1 EXITOS_IOPATH=pwrite \
               EXITOS_STATS="$ACTIVE_STATS" \
               "$PROBE" workload "$ACTIVE_TARGET" "$FILE_BYTES" \
               >"$ACTIVE_LOG" 2>&1; then
            ok "active bpftime ran the fio descriptor-shaped native-fallocate workload"
        else
            nok "active bpftime ran the fio descriptor-shaped native-fallocate workload"
        fi
        check_intercept_log "$ACTIVE_LOG" bpftime "active bpftime"
        check_stats "$ACTIVE_STATS" "active bpftime"
        awk '/^(PREPARE_READY|PREPARE_FAILED|PREPARE_OUTCOME|APP_SETUP_OPEN_RETURN|APP_SETUP_CLOSE_RETURN|APP_IO_OPEN_RETURN|APP_FIEMAP_SAFE|workload fallocate:) / {
                 print "# active bpftime evidence: " $0
             }' "$ACTIVE_LOG"
        if env -u LD_PRELOAD -u EXITOS_BPFTIME_SO -u EXITOS_FILES \
               -u EXITOS_PREPARE -u EXITOS_DONOR_DIR -u EXITOS_DONOR_FILES \
               -u EXITOS_FASTPATH -u EXITOS_IOPATH -u EXITOS_STATS \
               "$PROBE" verify "$ACTIVE_TARGET" "$FILE_BYTES" \
               >>"$ACTIVE_LOG" 2>&1; then
            ok "active bpftime result passes one complete ordinary-path byte-for-byte readback"
            awk '/^READBACK_OK / { print "# active bpftime evidence: " $0 }' \
                "$ACTIVE_LOG"
        else
            nok "active bpftime result passes one complete ordinary-path byte-for-byte readback"
        fi
    fi
else
    printf '# SKIP active bpftime: opt in with EXITOS_TEST_ACTIVE_BPFTIME=1 and EXITOS_BPFTIME_TRANSFORMER=<patched .so>\n'
fi

finish
