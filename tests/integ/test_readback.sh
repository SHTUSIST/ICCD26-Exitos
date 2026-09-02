#!/bin/bash
# Byte-for-byte oracle for the bypass path.  This test deliberately has three
# process boundaries:
#
#   1. an un-interposed process creates an O_EXCL file and establishes its
#      allocation/initialisation state;
#   2. a separate interposed process merely opens that existing file and writes;
#   3. an un-interposed verifier checks every byte through both buffered and
#      O_DIRECT reads.
#
# The fixture is always a loop device backed by a file created in this test's
# private directory.  Supplying an existing mount/device is intentionally not
# supported: this integration tier must be incapable of touching a physical
# disk even when an environment variable is stale or malicious.
set -u
SCRIPT_DIR=$(cd -- "$(dirname "$0")" && pwd -P) || exit 2
SELF="$SCRIPT_DIR/$(basename "$0")"
cd "$SCRIPT_DIR/../.." || exit 2

n=0
fail=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fail=$((fail+1)); echo "not ok $n - $*"; }
skipt() { n=$((n+1)); echo "ok $n - $* # SKIP"; }
skip_all() {
    if [ "${EXITOS_REQUIRE_READBACK:-0}" = 1 ]; then
        echo "not ok 1 - required read-back test cannot run: $*"
        echo "1..1  (1 failed)"
        exit 1
    fi
    echo "# SKIP - $*"
    echo "1..0  (0 failed)"
    exit 0
}

RB=${EXITOS_READBACK_BIN:-./attribution/readback}
PRELOAD_CHAIN=${EXITOS_READBACK_PRELOAD_CHAIN:-./libexitos_preload.so}
[ -x "$RB" ] || skip_all "readback oracle not built: $RB"
[ -x ./tests/integ/readback_pattern_contract ] || \
    skip_all "readback pattern contract not built"
[ "$(id -u)" -eq 0 ] || skip_all "needs root to create an owned loop/ext4 fixture"
for cmd in losetup mkfs.ext4 mount umount findmnt stat readlink mktemp dd; do
    command -v "$cmd" >/dev/null 2>&1 || skip_all "missing fixture command: $cmd"
done
REAL_LOSETUP=$(command -v losetup)
REAL_MKFS=$(command -v mkfs.ext4)
REAL_MOUNT=$(command -v mount)

# Injectable argument guard for the retained-fd safety contract.  It proxies
# the real owned-loop tools, but refuses any state-changing source argument
# that was reconstructed from an image/loop pathname after validation.
run_losetup() {
    local last

    last=${!#}
    if [ "${EXITOS_READBACK_REQUIRE_FD_ACTIONS:-0}" = 1 ]; then
        case " $* " in
          *" --find "*)
            [ "$last" = "/proc/$$/fd/9" ] || {
                echo "fd-action guard: loop attach did not use retained image fd" >&2
                return 97
            }
            ;;
          *" -d "*)
            echo "fd-action guard: pathname loop detach is forbidden" >&2
            return 97
            ;;
          *" -j "*)
            [ "$last" = "/proc/$$/fd/9" ] || {
                echo "fd-action guard: association proof did not use retained image fd" >&2
                return 97
            }
            ;;
        esac
    fi
    "$REAL_LOSETUP" "$@"
}

run_mkfs() {
    local last=${!#}

    if [ "${EXITOS_READBACK_REQUIRE_FD_ACTIONS:-0}" = 1 ] &&
       [ "$last" != "/proc/$$/fd/8" ]; then
        echo "fd-action guard: mkfs did not use retained loop fd" >&2
        return 97
    fi
    "$REAL_MKFS" "$@"
}

run_mount() {
    local args=("$@") source

    source=${args[${#args[@]}-2]}
    if [ "${EXITOS_READBACK_REQUIRE_FD_ACTIONS:-0}" = 1 ] &&
       [ "$source" != "/proc/$$/fd/8" ]; then
        echo "fd-action guard: mount did not use retained loop fd" >&2
        return 97
    fi
    "$REAL_MOUNT" "$@"
}

REC=${EXITOS_READBACK_RECORDS:-32}
case "$REC" in
  ''|*[!0-9]*) skip_all "EXITOS_READBACK_RECORDS must be a positive integer" ;;
esac
[ "$REC" -gt 0 ] || skip_all "EXITOS_READBACK_RECORDS must be positive"
case "${EXITOS_READBACK_INJECT_CLEANUP:-}" in
  ''|umount|detach) ;;
  *) skip_all "invalid EXITOS_READBACK_INJECT_CLEANUP value" ;;
esac
case "${EXITOS_READBACK_WINDOW_SIGNAL:-}" in
  ''|attach:INT|attach:TERM|mount:INT|mount:TERM) ;;
  *) skip_all "invalid EXITOS_READBACK_WINDOW_SIGNAL value" ;;
esac

FIXDIR=""
FIX_ID=""
CTRL=""
CTRL_ID=""
MNT=""
MNT_BASE_ID=""
WORK=""
IMG=""
IMG_CANON=""
IMG_ID=""
LOOP=""
LOOP_ID=""
LOOP_MAJMIN=""
MOUNTED=0
RAW_FD_OPEN=0
ASSOCIATION_FOUND=0
CLEANUP_FINISHED=0
CLEANUP_BLOCKED=0
CLEANUP_INJECT_USED=0
CLEAN_PROOF_MOUNT=0
CLEAN_PROOF_LOOP=0
CLEAN_PROOF_FILES=0
CLEAN_PROOF_DIRS=0
holder=""
OWNED_CTRL_FILES=()
PENDING_SIGNAL=0

plain() {
    env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_IOPATH -u EXITOS_STATS \
        -u EXITOS_VERIFY_IDENTITY "$@"
}

fixture_dir_is_ours() {
    [ -n "$FIXDIR" ] && [ -d "$FIXDIR" ] && [ -n "$FIX_ID" ] &&
    [ "$(stat -Lc '%d:%i:%u:%a' "$FIXDIR" 2>/dev/null)" = "$FIX_ID" ] &&
    [ "$(stat -Lc '%d:%i:%u:%a' "/proc/$$/fd/7" 2>/dev/null)" = "$FIX_ID" ]
}

image_is_ours() {
    fixture_dir_is_ours &&
    [ -n "$IMG" ] && [ -f "$IMG" ] && [ -n "$IMG_ID" ] &&
    [ "$(stat -Lc '%d:%i:%u:%a' "$IMG" 2>/dev/null)" = "$IMG_ID" ] &&
    [ "$(stat -Lc '%d:%i:%u:%a' "/proc/$$/fd/9" 2>/dev/null)" = "$IMG_ID" ]
}

loop_is_ours() {
    [ -n "$LOOP" ] && [ -b "$LOOP" ] &&
    [ "$(stat -Lc '%t:%T' "$LOOP" 2>/dev/null)" = "$LOOP_ID" ] &&
    image_is_ours || return 1
    local name backing
    name=${LOOP##*/}
    [ -r "/sys/class/block/$name/loop/backing_file" ] || return 1
    [ "$(plain sed -n '1p' "/sys/class/block/$name/loop/offset" 2>/dev/null)" = 0 ] \
        || return 1
    backing=$(plain sh -c 'readlink -f -- "$(cat "$1")"' sh \
        "/sys/class/block/$name/loop/backing_file" 2>/dev/null) || return 1
    [ "$backing" = "$IMG_CANON" ]
}

raw_fd_is_ours() {
    [ "$RAW_FD_OPEN" = 1 ] && [ -b "/proc/$$/fd/8" ] &&
    [ "$(stat -Lc '%t:%T' "/proc/$$/fd/8" 2>/dev/null)" = "$LOOP_ID" ] &&
    image_is_ours
}

mounted_source_is_ours() {
    raw_fd_is_ours || return 1
    [ "$(findmnt -rn -o TARGET -M "$MNT" 2>/dev/null)" = "$MNT" ] &&
    [ "$(findmnt -rn -o MAJ:MIN -M "$MNT" 2>/dev/null)" = "$LOOP_MAJMIN" ]
}

mount_is_ours() {
    [ "$MOUNTED" = 1 ] && mounted_source_is_ours
}

set_loop_identity_from_fd() {
    local loop_major_hex loop_minor_hex

    LOOP_ID=$(stat -Lc '%t:%T' "/proc/$$/fd/8" 2>/dev/null) || return 1
    loop_major_hex=${LOOP_ID%:*}
    loop_minor_hex=${LOOP_ID#*:}
    LOOP_MAJMIN="$((16#$loop_major_hex)):$((16#$loop_minor_hex))"
}

discover_retained_association() {
    local associations candidate candidate_id

    ASSOCIATION_FOUND=0
    image_is_ours || return 1
    if ! associations=$(run_losetup -j "/proc/$$/fd/9" 2>/dev/null); then
        echo "# cleanup cannot query association through retained image fd" >&2
        return 1
    fi
    [ -n "$associations" ] || return 0
    case "$associations" in
      *$'\n'*)
        echo "# cleanup refuses multiple associations for one private image" >&2
        return 1
        ;;
    esac
    candidate=${associations%%:*}
    case "$candidate" in /dev/loop*) ;; *) return 1 ;; esac
    [ -b "$candidate" ] || return 1

    if [ "$RAW_FD_OPEN" = 0 ]; then
        exec 8<>"$candidate" || return 1
        RAW_FD_OPEN=1
    fi
    candidate_id=$(stat -Lc '%t:%T' "$candidate" 2>/dev/null) || return 1
    [ "$(stat -Lc '%t:%T' "/proc/$$/fd/8" 2>/dev/null)" = "$candidate_id" ] ||
        return 1
    plain "$RB" raw-fd-check 8 9 >/dev/null 2>&1 || return 1
    LOOP=$candidate
    set_loop_identity_from_fd || return 1
    ASSOCIATION_FOUND=1
    return 0
}

cleanup_owned_fixture() {
    local bad=0 owned="" backing="" detach_ok=0 tries=0

    [ "$CLEANUP_FINISHED" = 0 ] || return 0
    if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then
        kill -TERM "$holder" 2>/dev/null || true
        wait "$holder" 2>/dev/null || true
        holder=""
    fi
    # Shell assignments are not commit records.  A signal can arrive after
    # LOOP_SET_FD or mount(2) succeeds but before LOOP/MOUNTED is assigned.
    # Rediscover both kernel states from retained identities before cleanup.
    if [ -n "$IMG" ] && [ -e "/proc/$$/fd/9" ]; then
        if ! discover_retained_association; then
            echo "# cleanup blocked: cannot prove retained image association" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
    fi
    if [ -n "$MNT" ] && findmnt -rn -M "$MNT" >/dev/null 2>&1; then
        if ! mounted_source_is_ours; then
            echo "# cleanup blocked: discovered mount identity mismatch" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        MOUNTED=1
    else
        MOUNTED=0
    fi
    # Never unmount or detach after an identity mismatch.  Leaking our private
    # fixture is safer than operating on an object whose identity changed.
    if [ "$MOUNTED" = 1 ]; then
        if ! mount_is_ours; then
            echo "# cleanup blocked: mounted fixture identity mismatch" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        if [ "${EXITOS_READBACK_INJECT_CLEANUP:-}" = umount ] &&
           [ "$CLEANUP_INJECT_USED" = 0 ]; then
            echo "# injected cleanup failure: umount" >&2
            CLEANUP_INJECT_USED=1
            bad=1
        elif umount "$MNT" 2>/dev/null; then
            MOUNTED=0
        else
            echo "# cleanup umount failed" >&2
            bad=1
        fi
        # Injection and a transient first failure are reported, but a real
        # second attempt is mandatory so the regression itself leaks nothing.
        if [ "$MOUNTED" = 1 ]; then
            if mount_is_ours && umount "$MNT" 2>/dev/null; then
                MOUNTED=0
            else
                echo "# cleanup recovery umount failed" >&2
                CLEANUP_BLOCKED=1
                return 1
            fi
        fi
    fi
    if [ -n "$MNT" ] && findmnt -rn -M "$MNT" >/dev/null 2>&1; then
        echo "# cleanup proof failed: fixture is still a mountpoint" >&2
        bad=1
    else
        CLEAN_PROOF_MOUNT=1
    fi

    # Destructive detach is issued on the already-validated block fd itself.
    # Closing fd8 before LOOP_CLR_FD and then reopening $LOOP would recreate the
    # very node-reuse race this retained descriptor exists to prevent.
    if [ "$ASSOCIATION_FOUND" = 1 ]; then
        if [ "$RAW_FD_OPEN" != 1 ] || ! raw_fd_is_ours ||
           ! plain "$RB" raw-fd-check 8 9 >/dev/null 2>&1; then
            echo "# cleanup blocked: retained raw fd identity mismatch" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        if [ "${EXITOS_READBACK_INJECT_CLEANUP:-}" = detach ] &&
           [ "$CLEANUP_INJECT_USED" = 0 ]; then
            echo "# injected cleanup failure: detach" >&2
            CLEANUP_INJECT_USED=1
            bad=1
        elif plain "$RB" raw-fd-detach 8 9 >/dev/null 2>&1; then
            detach_ok=1
        else
            echo "# cleanup retained-fd LOOP_CLR_FD failed" >&2
            bad=1
        fi
        if [ "$detach_ok" = 0 ]; then
            # Recover after an injected/transient first failure, but preserve
            # the non-zero result of the explicit cleanup.
            if raw_fd_is_ours &&
               plain "$RB" raw-fd-detach 8 9 >/dev/null 2>&1; then
                detach_ok=1
            else
                echo "# cleanup recovery LOOP_CLR_FD failed" >&2
                CLEANUP_BLOCKED=1
                return 1
            fi
        fi
        exec 8>&- 2>/dev/null || {
            echo "# cleanup could not close retained raw fd after LOOP_CLR_FD" >&2
            CLEANUP_BLOCKED=1
            return 1
        }
        RAW_FD_OPEN=0
        if ! backing=$(run_losetup -j "/proc/$$/fd/9" 2>/dev/null); then
            echo "# cleanup cannot prove post-detach association state" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        while [ -n "$backing" ] && [ "$tries" -lt 100 ]; do
            sleep 0.01
            tries=$((tries+1))
            if ! backing=$(run_losetup -j "/proc/$$/fd/9" 2>/dev/null); then
                echo "# cleanup cannot repeat post-detach association query" >&2
                CLEANUP_BLOCKED=1
                return 1
            fi
        done
        if [ -n "$backing" ]; then
            echo "# cleanup proof failed: private image remains loop-associated" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        LOOP=""
        ASSOCIATION_FOUND=0
        CLEAN_PROOF_LOOP=1
    else
        if [ "$RAW_FD_OPEN" = 1 ]; then
            exec 8>&- 2>/dev/null || return 1
            RAW_FD_OPEN=0
        fi
        CLEAN_PROOF_LOOP=1
    fi
    if [ -n "$IMG" ]; then
        if ! image_is_ours; then
            echo "# cleanup blocked: retained image identity mismatch" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        if ! backing=$(run_losetup -j "/proc/$$/fd/9" 2>/dev/null); then
            echo "# cleanup cannot perform final retained-image association proof" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        if [ -n "$backing" ]; then
            echo "# cleanup refuses to unlink a still-associated backing inode" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        exec 9>&- 2>/dev/null || {
            echo "# cleanup could not close retained image fd" >&2
            CLEANUP_BLOCKED=1
            return 1
        }
        if [ "$(stat -Lc '%d:%i:%u:%a' "$IMG" 2>/dev/null)" != "$IMG_ID" ] ||
           ! rm -f -- "$IMG"; then
            echo "# cleanup could not unlink the proven image inode" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
    fi
    if ! fixture_dir_is_ours; then
        echo "# cleanup blocked: fixture directory identity mismatch" >&2
        CLEANUP_BLOCKED=1
        return 1
    fi
    if [ -n "$CTRL" ] && [ -d "$CTRL" ] &&
       [ "$(stat -Lc '%d:%i:%u:%a' "$CTRL" 2>/dev/null)" != "$CTRL_ID" ]; then
        echo "# cleanup blocked: control directory identity mismatch" >&2
        CLEANUP_BLOCKED=1
        return 1
    fi
    for owned in "${OWNED_CTRL_FILES[@]}"; do
        case "$owned" in
          "$CTRL"/*)
            rm -f -- "$owned" 2>/dev/null || bad=1
            [ ! -e "$owned" ] && [ ! -L "$owned" ] || bad=1
            ;;
          *)
            echo "# cleanup rejected non-control known path: $owned" >&2
            CLEANUP_BLOCKED=1
            return 1
            ;;
        esac
    done
    if [ -n "$CTRL" ] && [ -d "$CTRL" ]; then
        rmdir -- "$CTRL" 2>/dev/null || bad=1
    fi
    if [ -n "$MNT" ] && [ -d "$MNT" ]; then
        if [ "$(stat -Lc '%d:%i:%u:%a' "$MNT" 2>/dev/null)" != "$MNT_BASE_ID" ]; then
            echo "# cleanup blocked: unmounted mount-directory identity mismatch" >&2
            CLEANUP_BLOCKED=1
            return 1
        fi
        rmdir -- "$MNT" 2>/dev/null || bad=1
    fi
    if [ -n "$IMG" ] && { [ -e "$IMG" ] || [ -L "$IMG" ]; }; then
        bad=1
    fi
    for owned in "${OWNED_CTRL_FILES[@]}"; do
        if [ -e "$owned" ] || [ -L "$owned" ]; then
            bad=1
        fi
    done
    [ "$bad" -ne 0 ] || CLEAN_PROOF_FILES=1
    if ! fixture_dir_is_ours || ! rmdir -- "$FIXDIR" 2>/dev/null; then
        echo "# cleanup could not remove the proven empty fixture directory" >&2
        CLEANUP_BLOCKED=1
        return 1
    fi
    exec 7>&- 2>/dev/null || bad=1
    if [ ! -e "$CTRL" ] && [ ! -e "$MNT" ] && [ ! -e "$FIXDIR" ]; then
        CLEAN_PROOF_DIRS=1
    else
        bad=1
    fi
    CLEANUP_FINISHED=1
    return "$bad"
}

defer_signal_exit() {
    PENDING_SIGNAL=0
    trap 'PENDING_SIGNAL=129' HUP
    trap 'PENDING_SIGNAL=130' INT
    trap 'PENDING_SIGNAL=143' TERM
}

finish_deferred_signals() {
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if [ "$PENDING_SIGNAL" -ne 0 ]; then
        exit "$PENDING_SIGNAL"
    fi
}

emit_fixture_meta() {
    local emitted_loop=$1 emitted_loop_id=""

    [ -z "$emitted_loop" ] ||
        emitted_loop_id=$(stat -Lc '%t:%T' "$emitted_loop" 2>/dev/null || true)
    echo "# fixture-meta path=$FIXDIR image=$IMG mount=$MNT "\
"loop=$emitted_loop image_id=$IMG_ID loop_id=$emitted_loop_id"
}

inject_window_signal() { # phase loop-path
    local phase=$1 emitted_loop=$2 requested=${EXITOS_READBACK_WINDOW_SIGNAL:-}

    case "$requested" in
      "$phase":INT)
        emit_fixture_meta "$emitted_loop"
        trap 'exit 130' INT
        kill -INT $$
        exit 130
        ;;
      "$phase":TERM)
        emit_fixture_meta "$emitted_loop"
        trap 'exit 143' TERM
        kill -TERM $$
        exit 143
        ;;
    esac
}

cleanup_on_exit() {
    local saved_rc=$? cleanup_rc=0

    trap - EXIT HUP INT TERM
    EXITOS_READBACK_INJECT_CLEANUP=""
    cleanup_owned_fixture || cleanup_rc=$?
    if [ "$saved_rc" -eq 0 ] && [ "$cleanup_rc" -ne 0 ]; then
        saved_rc=$cleanup_rc
    fi
    exit "$saved_rc"
}

trap cleanup_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

defer_signal_exit
FIXDIR=$(mktemp -d /tmp/exitos-readback.XXXXXX) || skip_all "mktemp -d failed"
[ "$(stat -Lc '%a' "$FIXDIR")" = 700 ] || skip_all "private fixture directory is not mode 0700"
FIX_ID=$(stat -Lc '%d:%i:%u:%a' "$FIXDIR") || skip_all "cannot identify fixture directory"
exec 7<"$FIXDIR" || skip_all "cannot retain fixture-directory descriptor"
fixture_dir_is_ours || skip_all "fixture-directory retained identity mismatch"
finish_deferred_signals
CTRL="$FIXDIR/control"
MNT="$FIXDIR/mnt"
mkdir -m 700 "$CTRL" "$MNT" || skip_all "cannot create private control/mount directories"
CTRL_ID=$(stat -Lc '%d:%i:%u:%a' "$CTRL") || skip_all "cannot identify control directory"
MNT_BASE_ID=$(stat -Lc '%d:%i:%u:%a' "$MNT") || skip_all "cannot identify mount directory"
defer_signal_exit
IMG=$(mktemp "$FIXDIR/loop-image.XXXXXX") || skip_all "cannot create O_EXCL loop image"
chmod 600 "$IMG" || skip_all "cannot protect loop image"
truncate -s 128M "$IMG" || skip_all "cannot size loop image"
IMG_CANON=$(readlink -f -- "$IMG") || skip_all "cannot canonicalise loop image"
IMG_ID=$(stat -Lc '%d:%i:%u:%a' "$IMG") || skip_all "cannot identify loop image"
# Retaining this descriptor means replacement of the pathname cannot make a
# later identity check accidentally bless a different backing file.
exec 9<>"$IMG" || skip_all "cannot retain loop-image descriptor"
image_is_ours || skip_all "retained image inode does not match its pathname"
finish_deferred_signals
defer_signal_exit
ATTACHED_LOOP=$(run_losetup --find --show --nooverlap "/proc/$$/fd/9") || skip_all "cannot allocate loop device"
inject_window_signal attach "$ATTACHED_LOOP"
LOOP=$ATTACHED_LOOP
case "$LOOP" in /dev/loop*) ;; *) skip_all "losetup returned a non-loop path" ;; esac
[ -b "$LOOP" ] || skip_all "losetup result is not a block device"
LOOP_ID=$(stat -Lc '%t:%T' "$LOOP") || skip_all "cannot identify loop device"
loop_major_hex=${LOOP_ID%:*}
loop_minor_hex=${LOOP_ID#*:}
LOOP_MAJMIN="$((16#$loop_major_hex)):$((16#$loop_minor_hex))"
loop_is_ours || skip_all "loop backing identity does not match the private image"
exec 8<>"$LOOP" || skip_all "cannot retain loop block descriptor"
RAW_FD_OPEN=1
raw_fd_is_ours || skip_all "retained loop descriptor dev_t mismatch"
if raw_check_out=$(plain "$RB" raw-fd-check 8 9 2>&1); then
    :
else
    skip_all "retained loop descriptor/backing check failed: $raw_check_out"
fi
finish_deferred_signals
run_mkfs -q -F -b 4096 "/proc/$$/fd/8" || skip_all "cannot format owned loop fixture"
defer_signal_exit
run_mount -o data=ordered "/proc/$$/fd/8" "$MNT" || skip_all "cannot mount owned loop fixture"
inject_window_signal mount "$LOOP"
MOUNTED=1
mount_is_ours || skip_all "mounted source is not the owned loop device"
finish_deferred_signals
chmod 700 "$MNT" || skip_all "cannot make fixture root private"
WORK="$MNT/work"
mkdir -m 700 "$WORK" || skip_all "cannot create private filesystem work directory"
[ "$(stat -Lc '%a' "$CTRL")" = 700 ] && [ "$(stat -Lc '%a' "$WORK")" = 700 ] \
    || skip_all "control/work directory permissions are not exactly 0700"

# The lifecycle children stop here.  They exist solely to prove that explicit
# cleanup failures propagate and that INT/TERM retain their conventional exit
# status while the EXIT fallback removes the private fixture.
if [ "${EXITOS_READBACK_META_CHILD:-0}" = 1 ]; then
    emit_fixture_meta "$LOOP"
    case "${EXITOS_READBACK_SELF_SIGNAL:-}" in
      INT) kill -INT $$; exit 130 ;;
      TERM) kill -TERM $$; exit 143 ;;
      '') ;;
      *) echo "invalid self signal" >&2; exit 2 ;;
    esac
    cleanup_rc=0
    cleanup_owned_fixture || cleanup_rc=$?
    [ "$cleanup_rc" -eq 0 ] || exit "$cleanup_rc"
    exit 0
fi

ok "fixture/control/work are private and retained directory identity matches ($FIX_ID)"
ok "raw oracle retained the loop fd and proved O_DIRECT/backing identity ($raw_check_out)"
if plain_pattern_out=$(plain ./tests/integ/readback_pattern_contract 2>&1); then
    ok "record pattern encodes tag, record, and word offset in every 64-bit word"
else
    nok "record pattern contract failed ($plain_pattern_out)"
fi

collision="$WORK/exitos-rb-collision.dat"
if plain "$RB" prepare "$collision" 1 init >/dev/null 2>&1 &&
   ! plain "$RB" prepare "$collision" 1 noinit >/dev/null 2>&1; then
    ok "prepare uses O_EXCL and refuses to overwrite a pre-existing fixture path"
else
    nok "prepare did not enforce O_EXCL on a pre-existing fixture path"
fi

corrupt="$WORK/exitos-rb-corrupt.dat"
mismatch_out=""
mismatch_rc=0
if plain "$RB" prepare "$corrupt" 1 init >/dev/null 2>&1 &&
   plain "$RB" write "$corrupt" 1 init >/dev/null 2>&1 &&
   printf '\377' | plain dd of="$corrupt" bs=1 seek=8315 count=1 \
                         conv=notrunc,fsync status=none; then
    mismatch_out=$(plain "$RB" verify "$corrupt" 1 init 2>&1) \
        || mismatch_rc=$?
    if [ "$mismatch_rc" -ne 0 ] &&
       printf '%s\n' "$mismatch_out" | grep -q \
           'mismatched_bytes=1 first_record_byte=123 first_file_byte=8315 expected=00 actual=ff'; then
        ok "verifier rejects a one-byte corruption and reports its exact byte/value"
    else
        nok "verifier did not report the injected byte exactly ($mismatch_out)"
    fi
else
    nok "could not construct the owned-loop one-byte negative oracle"
fi

swap_file="$WORK/exitos-rb-swap.dat"
swap_out=""
swap_rc=0
if plain "$RB" prepare "$swap_file" 2 init >/dev/null 2>&1 &&
   plain "$RB" write "$swap_file" 2 init >/dev/null 2>&1 &&
   plain "$RB" damage "$swap_file" 2 swap >/dev/null 2>&1; then
    swap_out=$(plain "$RB" verify "$swap_file" 2 init 2>&1) || swap_rc=$?
    if [ "$swap_rc" -ne 0 ] &&
       printf '%s\n' "$swap_out" | grep -q \
           'buffered mismatch: record=1 mismatched_bytes=1024 first_record_byte=3' &&
       printf '%s\n' "$swap_out" | grep -q \
           'O_DIRECT mismatch: record=2 mismatched_bytes=1024 first_record_byte=3'; then
        ok "verifier rejects a whole-record swap in both read channels"
    else
        nok "whole-record swap was not diagnosed exactly ($swap_out)"
    fi
else
    nok "could not construct the whole-record swap negative oracle"
fi

half_file="$WORK/exitos-rb-half.dat"
half_out=""
half_rc=0
if plain "$RB" prepare "$half_file" 1 init >/dev/null 2>&1 &&
   plain "$RB" damage "$half_file" 1 half >/dev/null 2>&1; then
    half_out=$(plain "$RB" verify "$half_file" 1 init 2>&1) || half_rc=$?
    if [ "$half_rc" -ne 0 ] &&
       printf '%s\n' "$half_out" | grep -q \
           'buffered mismatch: record=1 mismatched_bytes=512 first_record_byte=4102' &&
       printf '%s\n' "$half_out" | grep -q \
           'O_DIRECT mismatch: record=1 mismatched_bytes=512 first_record_byte=4102'; then
        ok "verifier rejects a write that updates only the first 4K half"
    else
        nok "first-4K-only write was not diagnosed exactly ($half_out)"
    fi
else
    nok "could not construct the first-4K-only negative oracle"
fi

guard_file="$WORK/exitos-rb-guard.dat"
guard_out=""
guard_rc=0
if plain "$RB" prepare "$guard_file" 1 init >/dev/null 2>&1 &&
   plain "$RB" write "$guard_file" 1 init >/dev/null 2>&1 &&
   plain "$RB" damage "$guard_file" 1 guard >/dev/null 2>&1; then
    guard_out=$(plain "$RB" verify "$guard_file" 1 init 2>&1) || guard_rc=$?
    if [ "$guard_rc" -ne 0 ] &&
       printf '%s\n' "$guard_out" | grep -q \
           'buffered mismatch: record=0 mismatched_bytes=1024 first_record_byte=6' &&
       printf '%s\n' "$guard_out" | grep -q \
           'O_DIRECT mismatch: record=0 mismatched_bytes=1024 first_record_byte=6'; then
        ok "verifier rejects an overwritten left guard in both read channels"
    else
        nok "guard overwrite was not diagnosed exactly ($guard_out)"
    fi
else
    nok "could not construct the guard-overwrite negative oracle"
fi

run_arm() { # arm mode expected-fast-writes
    local arm=$1 mode=$2 expected=$3
    local file out stats stats_id fw writer_rc
    file="$WORK/exitos-rb-${arm}-${mode}-${n}.dat"
    stats=$(mktemp "$CTRL/stats.${arm}.${mode}.XXXXXX") || {
        nok "$arm/$mode: cannot create private stats inode"; return;
    }
    OWNED_CTRL_FILES+=("$stats")
    stats_id=$(stat -Lc '%d:%i:%u:%a' "$stats") || {
        nok "$arm/$mode: cannot identify private stats inode"; return;
    }

    if out=$(plain "$RB" prepare "$file" "$REC" "$mode" 2>&1); then
        ok "$arm/$mode: un-interposed O_EXCL prepare completed before registration ($out)"
    else
        nok "$arm/$mode: prepare failed ($out)"
        return
    fi

    case "$arm" in
      none)
        if out=$(plain "$RB" write "$file" "$REC" "$mode" 2>&1); then
            ok "$arm/$mode: independent write process completed ($out)"
        else
            nok "$arm/$mode: write process failed ($out)"
        fi
        ;;
      preload-pwrite)
        writer_rc=0
        out=$(env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_IOPATH \
            -u EXITOS_STATS -u EXITOS_VERIFY_IDENTITY \
            LD_PRELOAD="$PRELOAD_CHAIN" \
            EXITOS_FILES=exitos-rb- EXITOS_IOPATH=pwrite \
            EXITOS_VERIFY_IDENTITY=1 EXITOS_STATS="$stats" \
            "$RB" write "$file" "$REC" "$mode" 2>&1) || writer_rc=$?
        if [ "$writer_rc" -eq 0 ]; then
            ok "$arm/$mode: independent intercepted write process completed ($out)"
        else
            nok "$arm/$mode: intercepted write process failed ($out)"
        fi
        if [ "$(stat -Lc '%d:%i:%u:%a' "$stats" 2>/dev/null)" != "$stats_id" ]; then
            nok "$arm/$mode: stats pathname no longer names the private inode"
        elif [ -s "$stats" ]; then
            fw=$(sed -n '1p' "$stats")
            if [ "$fw" -eq "$expected" ] 2>/dev/null; then
                ok "$arm/$mode: fast-write count is exact ($fw/$REC planned records)"
            else
                nok "$arm/$mode: fast-write count $fw, expected exactly $expected "\
"(counters=$(plain tr '\n' ',' <"$stats"))"
            fi
        else
            nok "$arm/$mode: private stats inode was not populated"
        fi
        ;;
      *) nok "$arm/$mode: unknown arm" ;;
    esac

    if out=$(plain "$RB" verify "$file" "$REC" "$mode" 2>&1); then
        ok "$arm/$mode: buffered and O_DIRECT verifiers match every byte; guards intact ($out)"
    else
        nok "$arm/$mode: byte/guard verification failed ($out)"
    fi
}

# Baselines prove the oracle accepts the three ordinary kernel semantics.
run_arm none init 0
run_arm none noinit 0
run_arm none append 0

# Only a fully initialised file is eligible.  The two negative arms must remain
# correct through fallback, but taking over even one record is a failure.
run_arm preload-pwrite init "$REC"
run_arm preload-pwrite noinit 0
run_arm preload-pwrite append 0

# Keep a clean MAP_SHARED mapping and buffered descriptor alive across raw
# writes.  The raw-device O_DIRECT oracle must see C; the holder reports whether
# the same cached bytes remain A or become C.  Either coherent outcome is legal
# for this characterization, but any third value is corruption.
stale_file="$WORK/exitos-rb-stale-${n}.dat"
ready="$CTRL/stale.ready"
release="$CTRL/stale.release"
result="$CTRL/stale.result"
OWNED_CTRL_FILES+=("$ready" "$release" "$result")
stale_stats=$(mktemp "$CTRL/stats.stale.XXXXXX") || stale_stats=""
[ -z "$stale_stats" ] || OWNED_CTRL_FILES+=("$stale_stats")
stale_stats_id=""
[ -z "$stale_stats" ] || stale_stats_id=$(stat -Lc '%d:%i:%u:%a' "$stale_stats")
if out=$(plain "$RB" prepare "$stale_file" "$REC" init 2>&1); then
    ok "stale-cache: initialized private file before holder/interception ($out)"
else
    nok "stale-cache: prepare failed ($out)"
fi
plain "$RB" cache-hold "$stale_file" "$REC" "$ready" "$release" "$result" &
holder=$!
tries=0
while [ ! -f "$ready" ] && kill -0 "$holder" 2>/dev/null && [ "$tries" -lt 200 ]; do
    sleep 0.05
    tries=$((tries+1))
done
if [ -f "$ready" ] && kill -0 "$holder" 2>/dev/null; then
    ok "stale-cache: holder faulted baseline A through buffered read and MAP_SHARED"
else
    nok "stale-cache: holder did not establish a live clean cache mapping"
fi
if out=$(env -u LD_PRELOAD -u EXITOS_FILES -u EXITOS_IOPATH \
    -u EXITOS_STATS -u EXITOS_VERIFY_IDENTITY \
    LD_PRELOAD="$PRELOAD_CHAIN" EXITOS_FILES=exitos-rb- \
    EXITOS_IOPATH=pwrite EXITOS_VERIFY_IDENTITY=1 EXITOS_STATS="$stale_stats" \
    "$RB" write "$stale_file" "$REC" init 2>&1); then
    ok "stale-cache: raw takeover wrote payload C ($out)"
else
    nok "stale-cache: intercepted writer failed ($out)"
fi
if out=$(plain "$RB" raw-verify "$stale_file" "$REC" init 8 9 0 2>&1); then
    ok "stale-cache: owned-loop O_DIRECT oracle sees payload C and intact guards ($out)"
else
    nok "stale-cache: raw-device oracle failed ($out)"
fi
raw_offset_out=""
raw_offset_rc=0
raw_offset_out=$(plain "$RB" raw-verify "$stale_file" "$REC" init \
    8 9 4096 2>&1) || raw_offset_rc=$?
if [ "$raw_offset_rc" -ne 0 ] &&
   printf '%s\n' "$raw_offset_out" | grep -q \
       'raw-device O_DIRECT mismatch: record='; then
    ok "raw oracle rejects a deliberately wrong +4096 physical offset"
else
    nok "wrong raw physical offset did not fail with an exact mismatch ($raw_offset_out)"
fi
if [ -z "$stale_stats_id" ] ||
   [ "$(stat -Lc '%d:%i:%u:%a' "$stale_stats" 2>/dev/null)" != "$stale_stats_id" ]; then
    nok "stale-cache: stats pathname does not name its private original inode"
elif [ -s "$stale_stats" ]; then
    fw=$(sed -n '1p' "$stale_stats")
    [ "$fw" -eq "$REC" ] 2>/dev/null \
        && ok "stale-cache: all planned writes used the takeover path ($fw/$REC)" \
        || nok "stale-cache: fast-write count $fw, expected exactly $REC "\
"(counters=$(plain tr '\n' ',' <"$stale_stats"))"
else
    nok "stale-cache: stats were not populated"
fi
( set -C; : >"$release" ) 2>/dev/null || nok "stale-cache: cannot create private release marker"
if wait "$holder"; then
    holder=""
    if [ -s "$result" ]; then
        out=$(plain sed -n '1p' "$result")
        case "$out" in
          *" unexpected=0") ok "stale-cache: live buffered/mmap observation classified as old A or new C ($out)" ;;
          *) nok "stale-cache: cache holder observed bytes other than A or C ($out)" ;;
        esac
    else
        nok "stale-cache: holder produced no result"
    fi
else
    nok "stale-cache: holder failed after release"
fi

# A dirty MAP_SHARED page can be written back by the kernel at any point, so a
# deterministic C-before-msync assertion needs a writeback freeze or a kernel
# hook this repository does not have.  The exact future experiment is recorded
# in the measurement note; claiming this as tested would be misleading.
skipt "dirty MAP_SHARED B -> raw C -> msync/fsync device ordering needs a deterministic writeback freeze"

cleanup_rc=0
cleanup_owned_fixture || cleanup_rc=$?
[ "$CLEAN_PROOF_MOUNT" = 1 ] \
    && ok "explicit cleanup proves the private mount is gone" \
    || nok "explicit cleanup could not prove the private mount is gone"
[ "$CLEAN_PROOF_LOOP" = 1 ] \
    && ok "explicit cleanup proves the private image is loop-detached" \
    || nok "explicit cleanup could not prove the private image is loop-detached"
[ "$CLEAN_PROOF_FILES" = 1 ] \
    && ok "explicit cleanup proves the image and every known control file are removed" \
    || nok "explicit cleanup could not prove all known files are removed"
[ "$CLEAN_PROOF_DIRS" = 1 ] \
    && ok "explicit cleanup proves the owned control/mount/fixture directories are removed" \
    || nok "explicit cleanup could not prove all owned directories are removed"
[ "$cleanup_rc" -eq 0 ] \
    && ok "explicit cleanup completed without a hidden failure" \
    || nok "explicit cleanup returned non-zero ($cleanup_rc)"

run_lifecycle_regression() { # label status cleanup-fault signal fd-guard window
    local label=$1 expected=$2 injected=$3 signal=$4 guard=${5:-0}
    local window=${6:-} child_out child_rc=0 meta path image mountpoint loop
    local image_id image_dev image_rest image_ino loop_id assoc_out assoc_rc=0

    child_out=$(plain env EXITOS_REQUIRE_READBACK=1 EXITOS_READBACK_RECORDS=1 \
        EXITOS_READBACK_META_CHILD=1 \
        EXITOS_READBACK_INJECT_CLEANUP="$injected" \
        EXITOS_READBACK_SELF_SIGNAL="$signal" \
        EXITOS_READBACK_REQUIRE_FD_ACTIONS="$guard" \
        EXITOS_READBACK_WINDOW_SIGNAL="$window" \
        EXITOS_READBACK_BIN="$RB" bash "$SELF" 2>&1) || child_rc=$?
    meta=$(printf '%s\n' "$child_out" | sed -n \
        's/^# fixture-meta /# fixture-meta /p' | tail -n 1)
    path=${meta#* path=}; path=${path%% image=*}
    image=${meta#* image=}; image=${image%% mount=*}
    mountpoint=${meta#* mount=}; mountpoint=${mountpoint%% loop=*}
    loop=${meta#* loop=}; loop=${loop%% image_id=*}
    image_id=${meta#* image_id=}; image_id=${image_id%% loop_id=*}
    loop_id=${meta##* loop_id=}
    image_dev=${image_id%%:*}
    image_rest=${image_id#*:}
    image_ino=${image_rest%%:*}
    assoc_out=$(plain "$RB" association-gone "$loop" "$loop_id" \
        "$image_dev" "$image_ino" 2>&1) || assoc_rc=$?
    case "$path:$image:$mountpoint:$loop:$loop_id:$image_dev:$image_ino" in
      /tmp/exitos-readback.*:/tmp/exitos-readback.*/loop-image.*:/tmp/exitos-readback.*/mnt:/dev/loop*:*:*:*)
        if [ "$child_rc" -eq "$expected" ] &&
           [ ! -e "$path" ] && [ ! -L "$path" ] &&
           [ "$assoc_rc" -eq 0 ] &&
           ! findmnt -rn -M "$mountpoint" >/dev/null 2>&1; then
            ok "$label returns $expected and leaves no mount, loop association, file, or directory"
        else
            nok "$label lifecycle regression: rc=$child_rc expected=$expected "\
"path_exists=$([ -e "$path" ] && echo 1 || echo 0) "\
"association_proof=[$assoc_out] output=$child_out"
        fi
        ;;
      *) nok "$label child did not emit a safe owned-fixture identity ($meta; $child_out)" ;;
    esac
}

run_lifecycle_regression "injected umount failure" 1 umount ""
run_lifecycle_regression "injected detach failure" 1 detach ""
run_lifecycle_regression "INT fallback cleanup" 130 "" INT
run_lifecycle_regression "TERM fallback cleanup" 143 "" TERM
run_lifecycle_regression "retained-fd action guard" 0 "" "" 1
run_lifecycle_regression "attach-commit INT window" 130 "" "" 0 attach:INT
run_lifecycle_regression "attach-commit TERM window" 143 "" "" 0 attach:TERM
run_lifecycle_regression "mount-commit INT window" 130 "" "" 0 mount:INT
run_lifecycle_regression "mount-commit TERM window" 143 "" "" 0 mount:TERM

echo "1..$n  ($fail failed)"
[ "$fail" -eq 0 ]
