#!/bin/bash
# Capture the six 8kpath cells that isolate command shape at 8 KiB:
#
#   (ext4 pwrite | whole-device pwrite | io_uring NVMe passthrough) x
#   (proven scattered | proven physically contiguous userspace buffer)
#
# qdsplit owns an anonymous O_TMPFILE on the serial-selected partition.  This
# runner owns only a private directory in that mount, retains the partition
# identity used for mount checks, and treats trace setup/results as part of the
# experiment rather than best-effort diagnostics.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

die() { echo "cmdcmp: $*" >&2; exit 2; }

QDSPLIT=${EXITOS_QDSPLIT_BIN:-./attribution/qdsplit}
BPFTRACE_BIN=${EXITOS_BPFTRACE_BIN:-bpftrace}
TASKSET_BIN=${EXITOS_TASKSET_BIN:-taskset}
SETSID_BIN=${EXITOS_SETSID_BIN:-setsid}
ENV_BIN=${EXITOS_ENV_BIN:-env}
TESTDISK=${EXITOS_TESTDISK:-tools/testdisk.sh}
TRACE_PROGRAM=tools/cmdcmp.bt

CPU=${CPU:-170}
ITERS=${ITERS:-400}
READY_TIMEOUT=${READY_TIMEOUT:-30}
TRACE_TIMEOUT=${TRACE_TIMEOUT:-15}
APP_TIMEOUT=${APP_TIMEOUT:-300}
TRACE_STOP_TIMEOUT=${TRACE_STOP_TIMEOUT:-30}
MNT=${EXITOS_TESTMNT:-/mnt/exitos_fs}

normalize_uint() {
    local name=$1 min=$2 max=$3 raw=${!1} value
    [[ $raw =~ ^[0-9]+$ ]] && (( ${#raw} <= 10 )) ||
        die "$name must be an integer in [$min,$max]"
    value=$((10#$raw))
    (( value >= min && value <= max )) ||
        die "$name must be an integer in [$min,$max]"
    printf -v "$name" '%u' "$value"
}

normalize_uint CPU 0 1023
normalize_uint ITERS 1 2000
normalize_uint READY_TIMEOUT 1 120
normalize_uint TRACE_TIMEOUT 1 120
normalize_uint APP_TIMEOUT 1 86400
normalize_uint TRACE_STOP_TIMEOUT 1 120

MODE=run
case "${1:-}" in
  --plan)
    [ "$#" -eq 1 ] || die "usage: $0 [--plan]"
    MODE=plan
    ;;
  --test-resolver)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-resolver is available only to the pure unit test"
    MODE=test-resolver
    ;;
  --test-run)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-run is available only to the pure unit test"
    MODE=test-run
    ;;
  --test-owned-dir-retained)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-owned-dir-retained is available only to the pure unit test"
    MODE=test-owned-dir
    ;;
  --test-retained-whole)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-retained-whole is available only to the pure unit test"
    MODE=test-retained-whole
    ;;
  --test-owned-mount-cleanup)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-owned-mount-cleanup is available only to the pure unit test"
    MODE=test-owned-mount-cleanup
    ;;
  --test-owned-mount-acquire)
    [ "$#" -eq 1 ] && [ "${EXITOS_CMDCMP_TEST_MODE:-}" = 1 ] ||
        die "test-owned-mount-acquire is available only to the pure unit test"
    MODE=test-owned-mount-acquire
    ;;
  "") ;;
  *) die "usage: $0 [--plan]" ;;
esac

emit_cells() {
    printf '%s\n' \
        'CELL index=1 buffer=normal arm=0 name=ext4-pwrite contract=scattered' \
        'CELL index=2 buffer=normal arm=14 name=block-pwrite contract=scattered' \
        'CELL index=3 buffer=normal arm=1 name=uring-passthrough contract=scattered' \
        'CELL index=4 buffer=contiguous arm=0 name=ext4-pwrite contract=contiguous' \
        'CELL index=5 buffer=contiguous arm=14 name=block-pwrite contract=contiguous' \
        'CELL index=6 buffer=contiguous arm=1 name=uring-passthrough contract=contiguous'
}

if [ "$MODE" = plan ]; then
    printf 'PLAN cpu=%s iters=%s ready_timeout=%s trace_timeout=%s app_timeout=%s trace_stop_timeout=%s\n' \
        "$CPU" "$ITERS" "$READY_TIMEOUT" "$TRACE_TIMEOUT" \
        "$APP_TIMEOUT" "$TRACE_STOP_TIMEOUT"
    emit_cells
    exit 0
fi

resolve_testdisk() {
    local resolver_out line key value
    declare -A seen_field=()

    if ! resolver_out=$(bash "$TESTDISK"); then
        die "test-disk resolver failed"
    fi
    unset EXITOS_DEV EXITOS_EXPECT_SERIAL EXITOS_FSPART \
          EXITOS_WINDOW_IN_PART EXITOS_TESTPART EXITOS_CHARDEV || true
    while IFS= read -r line; do
        [[ $line =~ ^export[[:space:]]+(EXITOS_[A-Z_]+)=([A-Za-z0-9._/:+-]+)$ ]] ||
            die "malformed test-disk resolver record"
        key=${BASH_REMATCH[1]}
        value=${BASH_REMATCH[2]}
        case "$key" in
          EXITOS_DEV|EXITOS_EXPECT_SERIAL|EXITOS_FSPART|EXITOS_WINDOW_IN_PART|EXITOS_TESTPART|EXITOS_CHARDEV) ;;
          *) die "unexpected test-disk resolver field: $key" ;;
        esac
        [[ -z ${seen_field[$key]+x} ]] ||
            die "duplicate test-disk resolver field: $key"
        seen_field[$key]=1
        case "$key" in
          EXITOS_DEV|EXITOS_TESTPART|EXITOS_CHARDEV)
            [[ $value =~ ^/dev/[A-Za-z0-9._+-]+$ ]] ||
                die "$key is not one fixed /dev path"
            ;;
          EXITOS_FSPART|EXITOS_WINDOW_IN_PART)
            if [[ $value =~ ^[A-Za-z0-9._+-]+$ ]]; then
                value="/dev/$value"
            fi
            [[ $value =~ ^/dev/[A-Za-z0-9._+-]+$ ]] ||
                die "$key is not one fixed partition path"
            ;;
          EXITOS_EXPECT_SERIAL)
            [[ $value =~ ^[A-Za-z0-9._:+-]+$ ]] ||
                die "serial contains unsupported characters"
            ;;
        esac
        printf -v "$key" '%s' "$value"
        export "$key"
    done <<<"$resolver_out"

    for key in EXITOS_DEV EXITOS_EXPECT_SERIAL EXITOS_FSPART \
               EXITOS_WINDOW_IN_PART EXITOS_TESTPART EXITOS_CHARDEV; do
        [[ -n ${seen_field[$key]+x} ]] || die "resolver omitted $key"
    done
    [ "$EXITOS_FSPART" = "$EXITOS_WINDOW_IN_PART" ] &&
    [ "$EXITOS_FSPART" = "$EXITOS_TESTPART" ] ||
        die "resolver partition fields disagree"
}

if [ "$MODE" = test-resolver ]; then
    resolve_testdisk
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$EXITOS_DEV" "$EXITOS_EXPECT_SERIAL" "$EXITOS_FSPART" \
        "$EXITOS_WINDOW_IN_PART" "$EXITOS_TESTPART" "$EXITOS_CHARDEV"
    exit 0
fi

validate_mount_path() {
    [[ $MNT == /* && $MNT != / && $MNT != */ &&
       $MNT != *$'\n'* && $MNT != *$'\r'* ]] ||
        die "mount path must be a non-root absolute directory without a trailing slash"
    case "/${MNT#/}/" in
      */../*|*/./*) die "mount path contains a dot component" ;;
    esac
}

command_exists() {
    case "$1" in
      */*) [ -x "$1" ] ;;
      *) command -v "$1" >/dev/null 2>&1 ;;
    esac
}

# Every external helper which participates in resource acquisition or cleanup
# runs in a session outside the runner's foreground process group.  A capture
# adds an intermediary shell for command substitution; that shell truly ignores
# group INT/TERM while GNU env resets the isolated target to default handling.
# Thus a foreground-group signal reaches the runner (where it is retained as
# pending), but cannot truncate a helper result or interrupt a cleanup syscall.
critical_run() {
    "$SETSID_BIN" --wait -- "$ENV_BIN" \
        --default-signal=INT --default-signal=TERM -- "$@"
}

critical_capture() (
    trap '' INT TERM
    exec "$SETSID_BIN" --wait -- "$ENV_BIN" \
        --default-signal=INT --default-signal=TERM -- "$@"
)

signal_tainted_status() {
    local rc=$1
    [ "$PENDING_SIGNAL_RC" -ne 0 ] && { [ "$rc" -eq 130 ] || [ "$rc" -eq 143 ]; }
}

# Bash can report the parent's trapped signal as the status of a completed
# command substitution.  Keep the assignment inside a tested context; if that
# happened, repeat only the read-only helper to obtain an unambiguous status.
capture_readonly() {
    local outvar=$1 captured="" rc=0
    shift
    captured=$(critical_capture "$@") || rc=$?
    if [ "$rc" -ne 0 ] && signal_tainted_status "$rc"; then
        rc=0
        captured=$(critical_capture "$@") || rc=$?
    fi
    [ "$rc" -eq 0 ] || return "$rc"
    printf -v "$outvar" '%s' "$captured"
}

# mktemp is not retryable: it may have created the directory before the parent
# observes its pending signal.  Accept that one completed result only when its
# exact private-template shape and directory postcondition are already proven.
capture_created_dir() {
    local outvar=$1 template=$2 captured="" suffix rc=0
    captured=$(critical_capture mktemp -d "$template") || rc=$?
    if [ "$rc" -ne 0 ]; then
        signal_tainted_status "$rc" || return "$rc"
    fi
    suffix=${captured#"${template%XXXXXX}"}
    [ "$captured" != "$suffix" ] && [[ $suffix =~ ^[A-Za-z0-9]{6}$ ]] &&
        [ -d "$captured" ] && [ ! -L "$captured" ] || return 1
    printf -v "$outvar" '%s' "$captured"
}

command_exists "$SETSID_BIN" || die "setsid executable is missing"
command_exists "$ENV_BIN" || die "env executable is missing"
"$ENV_BIN" --default-signal=INT --default-signal=TERM -- true ||
    die "env lacks required signal-disposition reset support"

TMP=""
TMP_ID=""
RUN_DIR=""
RUN_DIR_ID=""
MNT_CREATED=0
MNT_BASE_ID=""
MOUNTED_HERE=0
MOUNT_PENDING=0
MOUNT_ID=""
PART_FD=""
PART_FD_PATH=""
PART_RDEV_HEX=""
PART_MAJMIN=""
PART_SYSFS_PATH=""
WHOLE_FD=""
WHOLE_FD_PATH=""
WHOLE_RDEV_HEX=""
WHOLE_MAJMIN=""
WHOLE_MAJOR=""
WHOLE_MINOR=""
WHOLE_SYSFS_PATH=""
WHOLE_TEST_ID=""
APP=""
BT=""
CLEANUP_DONE=0
CLEANUP_STATUS=0
CLEANUP_ACTIVE=0
SIGNAL_DEFER_DEPTH=0
SIGNAL_EXIT_ACTIVE=0
PENDING_SIGNAL_RC=0
PENDING_SIGNAL_NAME=""
DEVICE_SYSFS_ROOT=/sys

signal_defer_enter() {
    SIGNAL_DEFER_DEPTH=$((SIGNAL_DEFER_DEPTH + 1))
}

signal_defer_leave() {
    [ "$SIGNAL_DEFER_DEPTH" -gt 0 ] || return 1
    SIGNAL_DEFER_DEPTH=$((SIGNAL_DEFER_DEPTH - 1))
}

dispatch_pending_signal() {
    if [ "$SIGNAL_DEFER_DEPTH" -eq 0 ] && [ "$CLEANUP_ACTIVE" -eq 0 ] &&
       [ "$PENDING_SIGNAL_RC" -ne 0 ] && [ "$SIGNAL_EXIT_ACTIVE" -eq 0 ]; then
        exit_for_pending_signal
    fi
}

acquire_begin() {
    signal_defer_enter
}

acquire_commit() {
    signal_defer_leave || die "unbalanced signal-deferred acquisition"
    dispatch_pending_signal
}

same_identity() {
    local path=$1 expected=$2 got
    [ -n "$path" ] && [ -e "$path" ] && [ ! -L "$path" ] || return 1
    capture_readonly got stat -Lc '%d:%i:%u' -- "$path" || return 1
    [ "$got" = "$expected" ]
}

owned_run_dir() {
    [ -n "$RUN_DIR_ID" ] && same_identity "$RUN_DIR" "$RUN_DIR_ID"
}

owned_tmp_dir() {
    [ -n "$TMP_ID" ] && same_identity "$TMP" "$TMP_ID"
}

mounted_source_is_retained_part() {
    local majmin
    [ -n "$PART_MAJMIN" ] && critical_run mountpoint -q -- "$MNT" || return 1
    capture_readonly majmin findmnt -n -o MAJ:MIN --target "$MNT" || return 1
    [ "$majmin" = "$PART_MAJMIN" ]
}

owned_mount_is_present() {
    local current_id
    [ "$MOUNTED_HERE" = 1 ] && [[ $MOUNT_ID =~ ^[0-9]+$ ]] || return 1
    mounted_source_is_retained_part || return 1
    capture_readonly current_id findmnt -n -o ID --target "$MNT" || return 1
    [ "$current_id" = "$MOUNT_ID" ]
}

cleanup_error() {
    echo "cmdcmp: cleanup failed: $*" >&2
    CLEANUP_STATUS=1
}

cleanup_resources() {
    local ignored
    if [ "$CLEANUP_DONE" = 1 ]; then
        return "$CLEANUP_STATUS"
    fi
    if [ "$CLEANUP_ACTIVE" = 1 ]; then
        cleanup_error "recursive cleanup attempt"
        return 1
    fi
    CLEANUP_ACTIVE=1
    signal_defer_enter

    if [ -n "$APP" ]; then
        kill -TERM "$APP" 2>/dev/null || true
        if wait "$APP" 2>/dev/null; then ignored=0; else ignored=$?; fi
        APP=""
    fi
    if [ -n "$BT" ]; then
        kill -INT "$BT" 2>/dev/null || true
        kill -TERM "$BT" 2>/dev/null || true
        if wait "$BT" 2>/dev/null; then ignored=0; else ignored=$?; fi
        BT=""
    fi

    if [ -n "$RUN_DIR_ID" ]; then
        if owned_run_dir; then
            # qdsplit creates only anonymous O_TMPFILE inodes.  Any named entry
            # is evidence, so use rmdir (never recursive/pathname removal).
            critical_run rmdir -- "$RUN_DIR" ||
                cleanup_error "could not remove owned work directory"
        else
            cleanup_error "work directory identity changed before removal"
        fi
        if [ -e "$RUN_DIR" ] || [ -L "$RUN_DIR" ]; then
            cleanup_error "work directory absence/identity is unverified after removal"
        fi
    fi
    if [ -n "$TMP_ID" ]; then
        if owned_tmp_dir; then
            critical_run rm -f -- "$TMP/app.out" "$TMP/trace.out" "$TMP/trace.err" ||
                cleanup_error "could not remove owned trace-state files"
            if owned_tmp_dir; then
                critical_run rmdir -- "$TMP" ||
                    cleanup_error "could not remove owned trace-state directory"
            fi
        else
            cleanup_error "trace-state directory identity changed before removal"
        fi
        if [ -e "$TMP" ] || [ -L "$TMP" ]; then
            cleanup_error "trace-state directory absence/identity is unverified after removal"
        fi
    fi

    # A matching dev_t alone is insufficient: after pathname replacement it
    # could name some other mount of the same partition.  Only unmount the
    # exact mount ID created and retained by this process.  Signals are deferred
    # across mount-to-ID acquisition; any non-signal failure in that window is
    # reported rather than guessed at during cleanup.
    if [ "$MOUNT_PENDING" = 1 ]; then
        cleanup_error "mount completed without a retained mount identity"
    elif [ "$MOUNTED_HERE" = 1 ]; then
        if owned_mount_is_present; then
            critical_run umount -- "$MNT" || cleanup_error "could not unmount owned mount"
        elif critical_run mountpoint -q -- "$MNT"; then
            cleanup_error "refusing to unmount a mount whose identity changed"
        fi
        if owned_mount_is_present || critical_run mountpoint -q -- "$MNT"; then
            cleanup_error "no-mount postcondition failed for owned mount"
        fi
    fi
    if [ "$MNT_CREATED" = 1 ] && [ -n "$MNT_BASE_ID" ]; then
        if critical_run mountpoint -q -- "$MNT"; then
            cleanup_error "refusing to remove a directory that remains a mountpoint"
        elif same_identity "$MNT" "$MNT_BASE_ID"; then
            critical_run rmdir -- "$MNT" ||
                cleanup_error "could not remove owned mount directory"
        else
            cleanup_error "mount-directory identity changed before removal"
        fi
        if [ -e "$MNT" ] || [ -L "$MNT" ]; then
            cleanup_error "mount-directory absence/identity is unverified after removal"
        fi
    fi
    if [ -n "$PART_FD" ]; then
        if exec {PART_FD}<&-; then PART_FD=""; else
            cleanup_error "could not close retained partition descriptor"
        fi
    fi
    if [ -n "$WHOLE_FD" ]; then
        if exec {WHOLE_FD}<&-; then WHOLE_FD=""; else
            cleanup_error "could not close retained whole-device descriptor"
        fi
    fi
    # Commit DONE only after all removal, no-mount, and descriptor-close
    # postconditions have run.  Signals remain deferred until this commit.
    CLEANUP_DONE=1
    CLEANUP_ACTIVE=0
    signal_defer_leave || CLEANUP_STATUS=1
    return "$CLEANUP_STATUS"
}

cleanup_fallback() {
    local rc=$? cleanup_rc
    trap - EXIT
    set +e
    cleanup_resources
    cleanup_rc=$?
    SIGNAL_DEFER_DEPTH=0
    if [ "$PENDING_SIGNAL_RC" -ne 0 ]; then
        SIGNAL_EXIT_ACTIVE=0
        exit_for_pending_signal
    fi
    trap - INT TERM
    if [ "$rc" -eq 0 ] && [ "$cleanup_rc" -ne 0 ]; then rc=1; fi
    exit "$rc"
}

exit_for_pending_signal() {
    local rc=$PENDING_SIGNAL_RC signal=$PENDING_SIGNAL_NAME cleanup_rc
    SIGNAL_EXIT_ACTIVE=1
    trap - EXIT
    set +e
    cleanup_resources
    cleanup_rc=$?
    SIGNAL_DEFER_DEPTH=0
    trap - INT TERM
    if [ "$cleanup_rc" -eq 0 ] && [ "$CLEANUP_DONE" -eq 1 ]; then
        printf 'SIGNAL_RESULT signal=%s exit=%s cleanup=complete\n' "$signal" "$rc" >&2
    else
        printf 'SIGNAL_RESULT signal=%s exit=%s cleanup=incomplete\n' "$signal" "$rc" >&2
    fi
    exit "$rc"
}

capture_signal() {
    local rc=$1 signal=$2
    if [ "$PENDING_SIGNAL_RC" -eq 0 ]; then
        PENDING_SIGNAL_RC=$rc
        PENDING_SIGNAL_NAME=$signal
    fi
    if [ "$SIGNAL_DEFER_DEPTH" -eq 0 ] && [ "$CLEANUP_ACTIVE" -eq 0 ] &&
       [ "$SIGNAL_EXIT_ACTIVE" -eq 0 ]; then
        exit_for_pending_signal
    fi
}

finish_successful_run() {
    local cleanup_rc=0
    cleanup_resources || cleanup_rc=$?
    dispatch_pending_signal
    if [ "$cleanup_rc" -ne 0 ]; then
        trap - EXIT INT TERM
        echo 'cmdcmp: successful campaign invalidated by incomplete cleanup' >&2
        exit 1
    fi
    trap - EXIT INT TERM
    printf 'CLEANUP_RESULT status=ok owned_paths=absent owned_mount=absent\n'
    printf 'RUN_RESULT status=ok cells=6\n'
    exit 0
}
trap cleanup_fallback EXIT
trap 'capture_signal 130 INT' INT
trap 'capture_signal 143 TERM' TERM

prepare_state_dir() {
    local owner mode
    acquire_begin
    capture_created_dir TMP "${TMPDIR:-/tmp}/exitos-cmdcmp-state.XXXXXX" ||
        die "cannot create private trace-state directory"
    critical_run chmod 0700 "$TMP"
    capture_readonly owner stat -Lc '%u' -- "$TMP" || die "cannot stat trace-state directory"
    capture_readonly mode stat -Lc '%a' -- "$TMP" || die "cannot stat trace-state directory"
    [ "$owner" = "$EUID" ] && [ "$mode" = 700 ] ||
        die "trace-state directory is not private and owned"
    capture_readonly TMP_ID stat -Lc '%d:%i:%u' -- "$TMP" ||
        die "cannot retain trace-state directory identity"
    acquire_commit
}

prepare_run_dir() {
    local owner mode
    acquire_begin
    [ -d "$MNT" ] && [ ! -L "$MNT" ] ||
        die "mount directory is not a safe directory: $MNT"
    capture_created_dir RUN_DIR "$MNT/.exitos-cmdcmp.XXXXXX" ||
        die "cannot create private work directory inside $MNT"
    critical_run chmod 0700 "$RUN_DIR"
    capture_readonly owner stat -Lc '%u' -- "$RUN_DIR" || die "cannot stat work directory"
    capture_readonly mode stat -Lc '%a' -- "$RUN_DIR" || die "cannot stat work directory"
    [ "$owner" = "$EUID" ] && [ "$mode" = 700 ] ||
        die "work directory identity/mode mismatch"
    capture_readonly RUN_DIR_ID stat -Lc '%d:%i:%u' -- "$RUN_DIR" ||
        die "cannot retain work-directory identity"
    acquire_commit
}

prepare_mount_dir() {
    acquire_begin
    if [ -e "$MNT" ] || [ -L "$MNT" ]; then
        [ -d "$MNT" ] && [ ! -L "$MNT" ] ||
            die "refusing unsafe mount path: $MNT"
    else
        critical_run mkdir -m 0700 -- "$MNT" ||
            die "cannot create private mount directory"
        MNT_CREATED=1
    fi
    capture_readonly MNT_BASE_ID stat -Lc '%d:%i:%u' -- "$MNT" ||
        die "cannot retain mount-directory identity"
    acquire_commit
}

retain_partition() {
    local major_hex minor_hex current
    acquire_begin
    exec {PART_FD}<"$EXITOS_TESTPART" ||
        die "cannot retain serial-resolved partition"
    PART_FD_PATH="/proc/$$/fd/$PART_FD"
    [ -b "$PART_FD_PATH" ] || die "serial-resolved partition is not a block device"
    capture_readonly PART_RDEV_HEX stat -Lc '%t:%T' -- "$PART_FD_PATH" ||
        die "cannot read retained partition dev_t"
    capture_readonly current stat -Lc '%t:%T' -- "$EXITOS_TESTPART" ||
        die "partition pathname disappeared after retention"
    [ "$current" = "$PART_RDEV_HEX" ] ||
        die "partition pathname changed during retention"
    IFS=: read -r major_hex minor_hex <<<"$PART_RDEV_HEX"
    [[ $major_hex =~ ^[0-9a-fA-F]+$ && $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
        die "invalid retained partition dev_t"
    PART_MAJMIN="$((16#$major_hex)):$((16#$minor_hex))"
    acquire_commit
}

retain_whole_device() {
    local major_hex minor_hex current sys_dev part_number
    acquire_begin
    exec {WHOLE_FD}<"$EXITOS_DEV" ||
        die "cannot retain serial-resolved whole block device"
    WHOLE_FD_PATH="/proc/$$/fd/$WHOLE_FD"
    [ -b "$WHOLE_FD_PATH" ] ||
        die "serial-resolved whole device is not a block device"
    # stat follows the procfd and obtains st_rdev from the retained open file,
    # so controller identity no longer depends on EXITOS_DEV's basename.
    capture_readonly WHOLE_RDEV_HEX stat -Lc '%t:%T' -- "$WHOLE_FD_PATH" ||
        die "cannot fstat retained whole-device st_rdev"
    capture_readonly current stat -Lc '%t:%T' -- "$EXITOS_DEV" ||
        die "whole-device pathname disappeared during retention"
    [ "$current" = "$WHOLE_RDEV_HEX" ] ||
        die "whole-device pathname changed during retention"
    IFS=: read -r major_hex minor_hex <<<"$WHOLE_RDEV_HEX"
    [[ $major_hex =~ ^[0-9a-fA-F]+$ && $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
        die "invalid retained whole-device dev_t"
    WHOLE_MAJOR=$((16#$major_hex))
    WHOLE_MINOR=$((16#$minor_hex))
    WHOLE_MAJMIN="$WHOLE_MAJOR:$WHOLE_MINOR"

    capture_readonly WHOLE_SYSFS_PATH readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN" ||
        die "cannot resolve retained whole-device dev_t in sysfs"
    capture_readonly PART_SYSFS_PATH readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$PART_MAJMIN" ||
        die "cannot resolve retained partition dev_t in sysfs"
    [ -d "$WHOLE_SYSFS_PATH" ] && [ -d "$PART_SYSFS_PATH" ] ||
        die "retained device sysfs identity is not a directory"
    read -r sys_dev <"$WHOLE_SYSFS_PATH/dev" ||
        die "cannot read whole-device sysfs dev_t"
    [ "$sys_dev" = "$WHOLE_MAJMIN" ] ||
        die "whole-device sysfs dev_t disagrees with retained fd"
    read -r sys_dev <"$PART_SYSFS_PATH/dev" ||
        die "cannot read partition sysfs dev_t"
    [ "$sys_dev" = "$PART_MAJMIN" ] ||
        die "partition sysfs dev_t disagrees with retained fd"
    read -r part_number <"$PART_SYSFS_PATH/partition" ||
        die "retained filesystem device is not a partition"
    [[ $part_number =~ ^[1-9][0-9]*$ ]] ||
        die "partition number in sysfs is malformed"
    [ "${PART_SYSFS_PATH%/*}" = "$WHOLE_SYSFS_PATH" ] ||
        die "retained partition does not belong to retained whole device"
    acquire_commit
}

mount_test_partition() {
    local new_mount_id
    acquire_begin
    if critical_run mountpoint -q -- "$MNT"; then
        mounted_source_is_retained_part ||
            die "refusing: existing mount at $MNT is not retained partition dev_t $PART_MAJMIN"
    else
        same_identity "$MNT" "$MNT_BASE_ID" ||
            die "mount directory was replaced before mount"
        MOUNT_PENDING=1
        critical_run mount --source "$PART_FD_PATH" --target "$MNT"
        mounted_source_is_retained_part ||
            die "new mount source does not match retained partition dev_t"
        capture_readonly new_mount_id findmnt -n -o ID --target "$MNT" ||
            die "cannot retain new mount ID"
        [[ $new_mount_id =~ ^[0-9]+$ ]] || die "new mount ID is malformed"
        MOUNT_ID=$new_mount_id
        MOUNTED_HERE=1
        MOUNT_PENDING=0
    fi
    acquire_commit
}

if [ "$MODE" = test-owned-dir ]; then
    validate_mount_path
    prepare_run_dir
    printf 'WORKDIR=%s\n' "$RUN_DIR"
    printf 'WORKDIR_READY\n'
    case "${EXITOS_TEST_SELF_SIGNAL:-}" in
      INT) kill -INT "$$" ;;
      TERM) kill -TERM "$$" ;;
      "") kill -STOP "$$" ;;
      *) die "invalid pure-test self signal" ;;
    esac
    exit 0
fi

if [ "$MODE" = test-owned-mount-cleanup ]; then
    validate_mount_path
    PART_MAJMIN=${EXITOS_TEST_PART_MAJMIN:?pure test requires EXITOS_TEST_PART_MAJMIN}
    MOUNT_ID=${EXITOS_TEST_MOUNT_ID:?pure test requires EXITOS_TEST_MOUNT_ID}
    [[ $PART_MAJMIN =~ ^[0-9]+:[0-9]+$ && $MOUNT_ID =~ ^[1-9][0-9]*$ ]] ||
        die "invalid pure-test retained mount identity"
    MOUNTED_HERE=1
    if ! cleanup_resources; then
        trap - EXIT INT TERM
        exit 1
    fi
    dispatch_pending_signal
    trap - EXIT INT TERM
    printf 'MOUNT_CLEANUP_RESULT status=ok no_mount=proven\n'
    exit 0
fi

if [ "$MODE" = test-owned-mount-acquire ]; then
    validate_mount_path
    PART_MAJMIN=${EXITOS_TEST_PART_MAJMIN:?pure test requires EXITOS_TEST_PART_MAJMIN}
    PART_FD_PATH=${EXITOS_TEST_PART_FD_PATH:?pure test requires EXITOS_TEST_PART_FD_PATH}
    [[ $PART_MAJMIN =~ ^[0-9]+:[0-9]+$ && $PART_FD_PATH == /* &&
       $PART_FD_PATH != *$'\n'* ]] ||
        die "invalid pure-test retained mount source"
    [ -f "$PART_FD_PATH" ] && [ ! -L "$PART_FD_PATH" ] ||
        die "pure-test retained mount source is not a fixed regular file"
    prepare_mount_dir
    mount_test_partition
    if ! cleanup_resources; then
        trap - EXIT INT TERM
        exit 1
    fi
    dispatch_pending_signal
    trap - EXIT INT TERM
    printf 'MOUNT_ACQUIRE_RESULT status=ok no_mount=proven\n'
    exit 0
fi

configure_cell_env() {
    local buffer=$1 arm=$2
    unset EXITOS_PAIRED EXITOS_SWAP45 EXITOS_REQUIRE_CONTIG \
          EXITOS_REQUIRE_SCATTERED EXITOS_HUGEBUF || true
    export EXITOS_ONLY="$arm"
    export EXITOS_BIGSZ=8192
    case "$buffer" in
      normal) export EXITOS_REQUIRE_SCATTERED=1 ;;
      contiguous)
        export EXITOS_HUGEBUF=1
        export EXITOS_REQUIRE_CONTIG=1
        ;;
      *) die "unknown buffer contract: $buffer" ;;
    esac
}

controller_from_retained_dev_t() {
    local outvar=$1 namespace_path component candidate=""
    local -a path_components=()
    capture_readonly namespace_path readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN" || return 1
    [ "$namespace_path" = "$WHOLE_SYSFS_PATH" ] || return 1
    IFS=/ read -r -a path_components <<<"$namespace_path"
    for component in "${path_components[@]}"; do
        if [[ $component =~ ^nvme[0-9]+$ ]]; then candidate=$component; fi
    done
    [ -n "$candidate" ] || return 1
    printf -v "$outvar" '%s' "$candidate"
}

derive_nvme_controller() {
    controller_from_retained_dev_t NVME_CTRL ||
        die "cannot derive NVMe controller from retained dev_t sysfs identity"
}

verify_retained_devices() {
    local cell=$1 phase=$2 whole_now part_now whole_link part_link
    local sys_dev controller
    [ "$DEVICE_MODE" = serial-guarded ] || return 0
    [ -b "$WHOLE_FD_PATH" ] && [ -b "$PART_FD_PATH" ] || return 1
    capture_readonly whole_now stat -Lc '%t:%T' -- "$WHOLE_FD_PATH" || return 1
    capture_readonly part_now stat -Lc '%t:%T' -- "$PART_FD_PATH" || return 1
    [ "$whole_now" = "$WHOLE_RDEV_HEX" ] &&
        [ "$part_now" = "$PART_RDEV_HEX" ] || return 1
    capture_readonly whole_link readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN" || return 1
    capture_readonly part_link readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$PART_MAJMIN" || return 1
    [ "$whole_link" = "$WHOLE_SYSFS_PATH" ] &&
        [ "$part_link" = "$PART_SYSFS_PATH" ] || return 1
    read -r sys_dev <"$WHOLE_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$WHOLE_MAJMIN" ] || return 1
    read -r sys_dev <"$PART_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$PART_MAJMIN" ] || return 1
    [ "${PART_SYSFS_PATH%/*}" = "$WHOLE_SYSFS_PATH" ] || return 1
    controller_from_retained_dev_t controller || return 1
    [ "$controller" = "$NVME_CTRL" ] || return 1
    printf 'DEVICE_IDENTITY cell=%s phase=%s whole_dev_t=%s part_dev_t=%s controller=%s status=verified\n' \
        "$cell" "$phase" "$WHOLE_MAJMIN" "$PART_MAJMIN" "$NVME_CTRL"
}

run_test_retained_whole() {
    local source_path=${EXITOS_TEST_WHOLE_PATH:?pure test requires EXITOS_TEST_WHOLE_PATH}
    local current_id current_link sys_dev controller
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires EXITOS_DEVICE_SYSFS_ROOT}
    WHOLE_MAJMIN=${EXITOS_TEST_WHOLE_MAJMIN:?pure test requires EXITOS_TEST_WHOLE_MAJMIN}
    [[ $source_path == /* && $source_path != *$'\n'* &&
       $DEVICE_SYSFS_ROOT == /* && $DEVICE_SYSFS_ROOT != *$'\n'* &&
       $WHOLE_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] ||
        die "invalid pure retained-device test identity"
    [ -f "$source_path" ] && [ ! -L "$source_path" ] ||
        die "pure retained-device source is not a fixed regular file"
    acquire_begin
    exec {WHOLE_FD}<"$source_path" || die "cannot retain pure-test whole object"
    WHOLE_FD_PATH="/proc/$$/fd/$WHOLE_FD"
    capture_readonly WHOLE_TEST_ID stat -Lc '%d:%i:%u' -- "$WHOLE_FD_PATH" ||
        die "cannot retain pure-test whole identity"
    capture_readonly WHOLE_SYSFS_PATH readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN" ||
        die "cannot resolve pure-test whole identity in sysfs"
    read -r sys_dev <"$WHOLE_SYSFS_PATH/dev" ||
        die "cannot read pure-test whole sysfs dev_t"
    [ "$sys_dev" = "$WHOLE_MAJMIN" ] ||
        die "pure-test whole sysfs dev_t mismatch"
    derive_nvme_controller
    printf 'RETAINED_WHOLE_IDENTITY_COMMITTED\n'
    acquire_commit
    printf 'RETAINED_WHOLE_READY\n'
    kill -STOP "$$"
    capture_readonly current_id stat -Lc '%d:%i:%u' -- "$WHOLE_FD_PATH" ||
        die "retained pure-test whole descriptor disappeared"
    [ "$current_id" = "$WHOLE_TEST_ID" ] ||
        die "retained pure-test whole identity changed"
    capture_readonly current_link readlink -f -- \
        "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN" ||
        die "pure-test whole sysfs link disappeared"
    [ "$current_link" = "$WHOLE_SYSFS_PATH" ] ||
        die "pure-test whole sysfs identity changed"
    controller_from_retained_dev_t controller ||
        die "cannot rederive controller from retained pure-test identity"
    [ "$controller" = "$NVME_CTRL" ] ||
        die "pure-test controller identity changed"
    printf 'RETAINED_WHOLE controller=%s dev_t=%s source_path_ignored=1\n' \
        "$NVME_CTRL" "$WHOLE_MAJMIN"
    if ! cleanup_resources; then
        trap - EXIT INT TERM
        exit 1
    fi
    dispatch_pending_signal
    trap - EXIT INT TERM
    exit 0
}

if [ "$MODE" = test-retained-whole ]; then
    run_test_retained_whole
fi

record_irq_snapshot() {
    local cell=$1 irq_path irq configured effective
    local irq_dir="$IRQ_SYSROOT/class/nvme/$NVME_CTRL/device/msi_irqs"
    local -a irq_paths=()
    shopt -s nullglob
    irq_paths=("$irq_dir"/[0-9]*)
    shopt -u nullglob
    [ "${#irq_paths[@]}" -gt 0 ] ||
        die "no MSI IRQs found for controller $NVME_CTRL"
    while IFS= read -r irq_path; do
        irq=${irq_path##*/}
        [[ $irq =~ ^[0-9]+$ ]] || die "malformed MSI IRQ number"
        configured=$(<"$IRQ_PROCROOT/irq/$irq/smp_affinity_list") ||
            die "cannot read IRQ $irq configured affinity"
        effective=$(<"$IRQ_PROCROOT/irq/$irq/effective_affinity_list") ||
            die "cannot read IRQ $irq effective affinity"
        [[ $configured =~ ^[0-9,-]+$ && $effective =~ ^[0-9,-]+$ ]] ||
            die "malformed IRQ affinity for IRQ $irq"
        printf 'IRQ_AFFINITY cell=%s controller=%s irq=%s configured=%s effective=%s\n' \
            "$cell" "$NVME_CTRL" "$irq" "$configured" "$effective"
    done < <(printf '%s\n' "${irq_paths[@]}" | sort -V)
}

child_alive() { kill -0 "$1" 2>/dev/null; }

wait_for_marker() {
    local file=$1 marker=$2 pid=$3 seconds=$4 attempt max
    max=$((seconds * 20))
    for ((attempt = 0; attempt < max; attempt++)); do
        if grep -Fqx -- "$marker" "$file" 2>/dev/null; then return 0; fi
        child_alive "$pid" || return 1
        sleep 0.05
    done
    grep -Fqx -- "$marker" "$file" 2>/dev/null
}

wait_for_trace_handshake() {
    local pid=$1 target_pid=$2 seconds=$3 attempt max attached_count begin_count
    local begin_marker="TRACE_BEGIN target_pid=$target_pid"
    max=$((seconds * 20))
    for ((attempt = 0; attempt < max; attempt++)); do
        attached_count=$(grep -Fxc 'Attached 3 probes' "$TMP/trace.err" 2>/dev/null || true)
        begin_count=$(grep -Fxc "$begin_marker" "$TMP/trace.out" 2>/dev/null || true)
        [ "$attached_count" -le 1 ] && [ "$begin_count" -le 1 ] || return 1
        if grep '^TRACE_BEGIN ' "$TMP/trace.out" 2>/dev/null |
           grep -Fvx "$begin_marker" >/dev/null 2>&1; then
            return 1
        fi
        if [ "$attached_count" -eq 1 ] && [ "$begin_count" -eq 1 ]; then
            child_alive "$pid" || return 1
            return 0
        fi
        child_alive "$pid" || return 1
        sleep 0.05
    done
    attached_count=$(grep -Fxc 'Attached 3 probes' "$TMP/trace.err" 2>/dev/null || true)
    begin_count=$(grep -Fxc "$begin_marker" "$TMP/trace.out" 2>/dev/null || true)
    [ "$attached_count" -eq 1 ] && [ "$begin_count" -eq 1 ] && child_alive "$pid"
}

wait_status() {
    local pid=$1 outvar=$2 child_rc
    set +e
    wait "$pid"
    child_rc=$?
    set -e
    printf -v "$outvar" '%d' "$child_rc"
}

stop_app() {
    local ignored
    [ -n "$APP" ] || return 0
    kill -TERM "$APP" 2>/dev/null || true
    wait_status "$APP" ignored
    APP=""
}

stop_tracer() {
    local requested=0 forced=0 attempt status
    [ -n "$BT" ] || return 0
    if child_alive "$BT"; then
        requested=1
        kill -INT "$BT" 2>/dev/null || true
        for ((attempt = 0; attempt < TRACE_STOP_TIMEOUT * 20; attempt++)); do
            child_alive "$BT" || break
            sleep 0.05
        done
    fi
    if child_alive "$BT"; then
        forced=1
        kill -TERM "$BT" 2>/dev/null || true
        for ((attempt = 0; attempt < TRACE_STOP_TIMEOUT * 20; attempt++)); do
            child_alive "$BT" || break
            sleep 0.05
        done
    fi
    if child_alive "$BT"; then return 1; fi
    wait_status "$BT" status
    BT=""
    [ "$forced" -eq 0 ] || return 1
    [ "$status" -eq 0 ] || { [ "$requested" -eq 1 ] && [ "$status" -eq 130 ]; }
}

cell_failure() {
    local index=$1 reason=$2
    echo "cmdcmp: cell $index failed: $reason" >&2
    if [ -s "$TMP/app.out" ]; then
        echo "--- qdsplit stdout/stderr ---" >&2
        sed -n '1,160p' "$TMP/app.out" >&2
    fi
    if [ -s "$TMP/trace.err" ]; then
        echo "--- tracer stderr ---" >&2
        sed -n '1,160p' "$TMP/trace.err" >&2
    fi
    if [ -s "$TMP/trace.out" ]; then
        echo "--- tracer stdout ---" >&2
        sed -n '1,160p' "$TMP/trace.out" >&2
    fi
}

abort_cell() {
    stop_app || true
    stop_tracer || true
}

SHAPE_ERROR=""

validate_trace_event_shape() {
    local line=$1 expected_usercmd=$2 expected_nr_phys=$3
    local expected_format=$4 expected_psdt=$5 expected_sgl_class=$6
    local expected_sgl_type=$7 expected_sgl_length=$8 expected_descriptors=$9
    local token key value flags_hex flags_value actual_usercmd required
    local -a tokens=()
    local -A field=()

    SHAPE_ERROR=""
    read -r -a tokens <<<"$line"
    [ "${tokens[0]-}" = TRACE_EVENT ] || {
        SHAPE_ERROR="record does not start with TRACE_EVENT"
        return 1
    }
    for token in "${tokens[@]:1}"; do
        [[ $token =~ ^([a-z][a-z0-9_]*)=([^[:space:]]+)$ ]] || {
            SHAPE_ERROR="malformed trace token: $token"
            return 1
        }
        key=${BASH_REMATCH[1]}
        value=${BASH_REMATCH[2]}
        [ -z "${field[$key]+x}" ] || {
            SHAPE_ERROR="duplicate trace field: $key"
            return 1
        }
        field[$key]=$value
    done
    for required in usercmd nr_phys_segments format psdt iod_req_flags iod_nr_descriptors; do
        [ -n "${field[$required]+x}" ] || {
            SHAPE_ERROR="missing trace field: $required"
            return 1
        }
    done

    [ "${field[usercmd]}" = "$expected_usercmd" ] || {
        SHAPE_ERROR="usercmd=${field[usercmd]} expected=$expected_usercmd"
        return 1
    }
    [ "${field[nr_phys_segments]}" = "$expected_nr_phys" ] || {
        if [ "$expected_nr_phys" = 2 ] && [ "${field[nr_phys_segments]}" = 1 ]; then
            SHAPE_ERROR="nr_phys_segments=1 after scattered-PFN proof; required 2 (DMA-coalesced shape is out of contract)"
        else
            SHAPE_ERROR="nr_phys_segments=${field[nr_phys_segments]} expected=$expected_nr_phys"
        fi
        return 1
    }
    [ "${field[format]}" = "$expected_format" ] || {
        SHAPE_ERROR="format=${field[format]} expected=$expected_format"
        return 1
    }
    [ "${field[psdt]}" = "$expected_psdt" ] || {
        SHAPE_ERROR="psdt=${field[psdt]} expected=$expected_psdt"
        return 1
    }
    [ "${field[iod_nr_descriptors]}" = "$expected_descriptors" ] || {
        SHAPE_ERROR="iod_nr_descriptors=${field[iod_nr_descriptors]} expected=$expected_descriptors"
        return 1
    }

    flags_hex=${field[iod_req_flags]#0x}
    [[ ${field[iod_req_flags]} == 0x* && $flags_hex =~ ^[0-9a-fA-F]{1,8}$ ]] || {
        SHAPE_ERROR="iod_req_flags is not bounded hexadecimal: ${field[iod_req_flags]}"
        return 1
    }
    flags_value=$((16#$flags_hex))
    actual_usercmd=0
    (( (flags_value & 0x2) != 0 )) && actual_usercmd=1
    [ "$actual_usercmd" = "$expected_usercmd" ] || {
        SHAPE_ERROR="iod_req_flags=${field[iod_req_flags]} USER_CMD-bit=$actual_usercmd expected=$expected_usercmd"
        return 1
    }

    if [ "$expected_format" = PRP ]; then
        for key in sgl_class sgl_type sgl_length; do
            [ -z "${field[$key]+x}" ] || {
                SHAPE_ERROR="PRP record unexpectedly contains $key=${field[$key]}"
                return 1
            }
        done
    else
        for required in sgl_class sgl_type sgl_length; do
            [ -n "${field[$required]+x}" ] || {
                SHAPE_ERROR="missing SGL trace field: $required"
                return 1
            }
        done
        [ "${field[sgl_class]}" = "$expected_sgl_class" ] || {
            SHAPE_ERROR="sgl_class=${field[sgl_class]} expected=$expected_sgl_class"
            return 1
        }
        [ "${field[sgl_type]}" = "$expected_sgl_type" ] || {
            SHAPE_ERROR="sgl_type=${field[sgl_type]} expected=$expected_sgl_type"
            return 1
        }
        [ "${field[sgl_length]}" = "$expected_sgl_length" ] || {
            SHAPE_ERROR="sgl_length=${field[sgl_length]} expected=$expected_sgl_length"
            return 1
        }
    fi
}

run_cell() {
    local index=$1 buffer=$2 arm=$3 name=$4 contract app_rc=0 bt_rc=0
    local attempt trace_events trace_end event_count target_pid line shape_event=0
    local expected_usercmd expected_nr_phys expected_format expected_psdt
    local expected_sgl_class expected_sgl_type expected_sgl_length expected_descriptors
    contract=$buffer
    [ "$buffer" = normal ] && contract=scattered

    case "$buffer:$arm" in
      normal:0|normal:14)
        expected_usercmd=0; expected_nr_phys=2; expected_format=PRP; expected_psdt=0
        expected_sgl_class=none; expected_sgl_type=none; expected_sgl_length=none
        expected_descriptors=0
        ;;
      normal:1)
        expected_usercmd=1; expected_nr_phys=2; expected_format=SGL; expected_psdt=1
        expected_sgl_class=external_last_segment; expected_sgl_type=0x30
        expected_sgl_length=32; expected_descriptors=1
        ;;
      contiguous:0|contiguous:14)
        expected_usercmd=0; expected_nr_phys=1; expected_format=PRP; expected_psdt=0
        expected_sgl_class=none; expected_sgl_type=none; expected_sgl_length=none
        expected_descriptors=0
        ;;
      contiguous:1)
        expected_usercmd=1; expected_nr_phys=1; expected_format=SGL; expected_psdt=1
        expected_sgl_class=inline_data; expected_sgl_type=0x0
        expected_sgl_length=8192; expected_descriptors=0
        ;;
      *) die "no command-shape contract for buffer=$buffer arm=$arm" ;;
    esac

    configure_cell_env "$buffer" "$arm"
    export EXITOS_FSFILE="$RUN_DIR/cell_${index}_${buffer}_${arm}.dat"
    : >"$TMP/app.out"
    : >"$TMP/trace.out"
    : >"$TMP/trace.err"

    printf 'CELL_BEGIN index=%s buffer=%s arm=%s name=%s contract=%s\n' \
        "$index" "$buffer" "$arm" "$name" "$contract"
    printf 'SHAPE_CONTRACT index=%s usercmd=%s nr_phys_segments=%s format=%s psdt=%s sgl_class=%s sgl_type=%s sgl_length=%s iod_nr_descriptors=%s\n' \
        "$index" "$expected_usercmd" "$expected_nr_phys" "$expected_format" \
        "$expected_psdt" "$expected_sgl_class" "$expected_sgl_type" \
        "$expected_sgl_length" "$expected_descriptors"
    if ! verify_retained_devices "$index" pre; then
        cell_failure "$index" "retained device/controller identity failed before tracing"
        return 1
    fi
    record_irq_snapshot "$index"

    "$TASKSET_BIN" -c "$CPU" "$QDSPLIT" "$ITERS" \
        >"$TMP/app.out" 2>&1 & APP=$!
    target_pid=$APP
    printf 'CELL_PID index=%s qdsplit_pid=%s\n' "$index" "$APP"
    if ! wait_for_marker "$TMP/app.out" READY "$APP" "$READY_TIMEOUT" ||
       ! child_alive "$APP"; then
        abort_cell
        cell_failure "$index" "qdsplit did not remain alive after READY"
        return 1
    fi

    "$BPFTRACE_BIN" "$TRACE_PROGRAM" "$APP" "$WHOLE_MAJOR" "$WHOLE_MINOR" \
        >"$TMP/trace.out" 2>"$TMP/trace.err" & BT=$!
    # bpftrace BEGIN is explicitly pre-attach.  v0.25's status line is the
    # post-attach barrier; only their conjunction proves both the selected PID
    # and that all three programs (BEGIN, fexit, END) were attached.
    if ! wait_for_trace_handshake "$BT" "$target_pid" "$TRACE_TIMEOUT"; then
        abort_cell
        cell_failure "$index" "tracer did not reach one exact unique PID-bound attach handshake"
        return 1
    fi
    printf 'TRACE_READY target_pid=%s\n' "$target_pid"

    for ((attempt = 0; attempt < APP_TIMEOUT * 20; attempt++)); do
        if ! child_alive "$APP"; then break; fi
        if ! child_alive "$BT"; then
            abort_cell
            cell_failure "$index" "tracer exited before qdsplit"
            return 1
        fi
        sleep 0.05
    done
    if child_alive "$APP"; then
        abort_cell
        cell_failure "$index" "qdsplit exceeded APP_TIMEOUT"
        return 1
    fi
    wait_status "$APP" app_rc
    APP=""
    if ! stop_tracer; then bt_rc=1; fi
    if ! verify_retained_devices "$index" post; then
        cell_failure "$index" "retained device/controller identity failed after tracing"
        return 1
    fi

    if [ "$app_rc" -ne 0 ]; then
        cell_failure "$index" "qdsplit exited $app_rc"
        return 1
    fi
    if [ "$bt_rc" -ne 0 ]; then
        cell_failure "$index" "tracer failed or required forced termination"
        return 1
    fi
    if [ "$(grep -Fxc 'Attached 3 probes' "$TMP/trace.err" 2>/dev/null || true)" -ne 1 ] ||
       grep -Fvx 'Attached 3 probes' "$TMP/trace.err" >/dev/null 2>&1; then
        cell_failure "$index" "tracer stderr was not the sole expected post-attach marker"
        return 1
    fi
    if [ "$(grep -Fxc "TRACE_BEGIN target_pid=$target_pid" "$TMP/trace.out" 2>/dev/null || true)" -ne 1 ] ||
       grep '^TRACE_BEGIN ' "$TMP/trace.out" 2>/dev/null |
       grep -Fvx "TRACE_BEGIN target_pid=$target_pid" >/dev/null 2>&1; then
        cell_failure "$index" "TRACE_BEGIN marker was not exact and unique"
        return 1
    fi
    case "$buffer" in
      normal)
        grep -Fqx 'buffer contract: scattered proven runs=2' "$TMP/app.out" || {
            cell_failure "$index" "qdsplit omitted the exact scattered-PFN proof"
            return 1
        }
        ;;
      contiguous)
        grep -Fqx 'buffer contract: contiguous proven runs=1' "$TMP/app.out" || {
            cell_failure "$index" "qdsplit omitted the exact contiguous-PFN proof"
            return 1
        }
        ;;
    esac
    trace_events=$(grep -c '^TRACE_EVENT ' "$TMP/trace.out" 2>/dev/null || true)
    [ "$trace_events" -eq "$ITERS" ] || {
        cell_failure "$index" "trace has $trace_events successful commands, expected exactly ITERS=$ITERS"
        return 1
    }
    if grep '^TRACE_EVENT ' "$TMP/trace.out" |
       grep -Fv " pid=$target_pid dev_major=$WHOLE_MAJOR dev_first_minor=$WHOLE_MINOR " \
           >/dev/null 2>&1; then
        cell_failure "$index" "trace contains an event for another PID or block device"
        return 1
    fi
    while IFS= read -r line; do
        shape_event=$((shape_event + 1))
        if ! validate_trace_event_shape "$line" "$expected_usercmd" \
             "$expected_nr_phys" "$expected_format" "$expected_psdt" \
             "$expected_sgl_class" "$expected_sgl_type" \
             "$expected_sgl_length" "$expected_descriptors"; then
            cell_failure "$index" "trace event $shape_event violates shape contract: $SHAPE_ERROR"
            return 1
        fi
    done < <(grep '^TRACE_EVENT ' "$TMP/trace.out")
    trace_end=$(grep "^TRACE_END target_pid=$target_pid events=" "$TMP/trace.out" || true)
    [[ $trace_end =~ ^TRACE_END[[:space:]]target_pid=$target_pid[[:space:]]events=([1-9][0-9]*)$ ]] || {
        cell_failure "$index" "missing/malformed nonempty TRACE_END"
        return 1
    }
    event_count=${BASH_REMATCH[1]}
    [ "$event_count" -eq "$trace_events" ] || {
        cell_failure "$index" "TRACE_END event count disagrees with trace"
        return 1
    }

    printf 'APP_OUTPUT_BEGIN index=%s interpretation=shape-only/latency-invalid-under-tracing\n' "$index"
    sed -n '1,200p' "$TMP/app.out"
    printf 'APP_OUTPUT_END index=%s\n' "$index"
    printf 'TRACE_OUTPUT_BEGIN index=%s\n' "$index"
    sed -n '1,2400p' "$TMP/trace.out"
    printf 'TRACE_OUTPUT_END index=%s\n' "$index"
    printf 'SHAPE_RESULT index=%s status=ok matched_events=%s\n' "$index" "$shape_event"
    printf 'CELL_RESULT index=%s status=ok\n' "$index"
}

validate_mount_path
command_exists "$QDSPLIT" || die "build or provide qdsplit first"
command_exists "$BPFTRACE_BIN" || die "bpftrace executable is missing"
command_exists "$TASKSET_BIN" || die "taskset executable is missing"
[ -r "$TRACE_PROGRAM" ] || die "trace program is missing"

IRQ_SYSROOT=/sys
IRQ_PROCROOT=/proc
NVME_CTRL=""
DEVICE_MODE=serial-guarded

if [ "$MODE" = test-run ]; then
    IRQ_SYSROOT=${EXITOS_IRQ_SYSFS_ROOT:?pure test requires EXITOS_IRQ_SYSFS_ROOT}
    IRQ_PROCROOT=${EXITOS_IRQ_PROC_ROOT:?pure test requires EXITOS_IRQ_PROC_ROOT}
    NVME_CTRL=${EXITOS_TEST_NVME_CTRL:?pure test requires EXITOS_TEST_NVME_CTRL}
    WHOLE_MAJMIN=${EXITOS_TEST_WHOLE_MAJMIN:?pure test requires EXITOS_TEST_WHOLE_MAJMIN}
    [[ $NVME_CTRL =~ ^nvme-[A-Za-z0-9._+-]+$ ]] ||
        die "invalid pure-test NVMe controller label"
    IFS=: read -r WHOLE_MAJOR WHOLE_MINOR <<<"$WHOLE_MAJMIN"
    [[ $WHOLE_MAJMIN =~ ^[0-9]+:[0-9]+$ &&
       $WHOLE_MAJOR =~ ^[0-9]+$ && $WHOLE_MINOR =~ ^[0-9]+$ ]] ||
        die "invalid pure-test whole-device dev_t"
    DEVICE_MODE=pure-test
    EXITOS_DEV=/dev/test-nvme
    EXITOS_EXPECT_SERIAL=PURE-TEST
    EXITOS_FSPART=/dev/test-part
    EXITOS_WINDOW_IN_PART=/dev/test-part
    EXITOS_TESTPART=/dev/test-part
    EXITOS_CHARDEV=/dev/test-char
    export EXITOS_DEV EXITOS_EXPECT_SERIAL EXITOS_FSPART \
           EXITOS_WINDOW_IN_PART EXITOS_TESTPART EXITOS_CHARDEV
else
    [ -r "$TESTDISK" ] || die "missing test-disk resolver: $TESTDISK"
    [ -z "${EXITOS_IRQ_SYSFS_ROOT+x}" ] &&
    [ -z "${EXITOS_IRQ_PROC_ROOT+x}" ] &&
    [ -z "${EXITOS_TEST_NVME_CTRL+x}" ] &&
    [ -z "${EXITOS_DEVICE_SYSFS_ROOT+x}" ] ||
        die "IRQ root/controller overrides are pure-test-only"
    resolve_testdisk
    retain_partition
    retain_whole_device
    prepare_mount_dir
    mount_test_partition
    derive_nvme_controller
    verify_retained_devices 0 setup ||
        die "retained device/controller identity failed during setup"
fi

prepare_run_dir
prepare_state_dir

export EXITOS_SAMEFILE=1 EXITOS_LBA_START=0 EXITOS_LBA_COUNT=0
export EXITOS_PAUSE=$((TRACE_TIMEOUT + 2))
unset EXITOS_PAIRED || true

printf 'RUN_METADATA suite=8kpath cpu=%s iters=%s bytes=8192 cells=6 ready_timeout=%s trace_timeout=%s app_timeout=%s trace_stop_timeout=%s trace_program=%s\n' \
    "$CPU" "$ITERS" "$READY_TIMEOUT" "$TRACE_TIMEOUT" \
    "$APP_TIMEOUT" "$TRACE_STOP_TIMEOUT" "$TRACE_PROGRAM"
printf 'MEASUREMENT_CONTRACT shape-only latency-invalid-under-tracing\n'
if [ "$DEVICE_MODE" = pure-test ]; then
    printf 'DEVICE_METADATA mode=pure-test controller=%s mount=%s\n' \
        "$NVME_CTRL" "$MNT"
else
    printf 'DEVICE_METADATA mode=serial-guarded dev=%s whole_dev_t=%s part=%s part_dev_t=%s chardev=%s serial=%s controller=%s mount=%s mounted_here=%s\n' \
        "$EXITOS_DEV" "$WHOLE_MAJMIN" "$EXITOS_TESTPART" "$PART_MAJMIN" "$EXITOS_CHARDEV" \
        "$EXITOS_EXPECT_SERIAL" "$NVME_CTRL" "$MNT" "$MOUNTED_HERE"
fi

index=0
for buffer in normal contiguous; do
    for arm in 0 14 1; do
        index=$((index + 1))
        case "$arm" in
          0) name=ext4-pwrite ;;
          14) name=block-pwrite ;;
          1) name=uring-passthrough ;;
        esac
        run_cell "$index" "$buffer" "$arm" "$name" || exit 1
    done
done
finish_successful_run
