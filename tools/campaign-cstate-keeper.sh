#!/bin/bash
# Primary two-arm causal screen for the 8 KiB command-shape question.  The keeper starts only
# after qdsplit has initialized its anonymous owned file and printed READY, then
# runs at SCHED_IDLE on the measurement CPU.  This screen does not by itself
# prove C-state causality: the keeper also changes frequency/cache state and
# same-CPU scheduling.  Report those interventions separately.
#
# Raw writes remain confined to qdsplit's fully initialized anonymous file on
# the serial-identified test partition.  The campaign owns a randomized private
# directory only as the retained parent for O_TMPFILE; it never removes a
# predictable or pre-existing benchmark pathname.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

die() { echo "cstate campaign: $*" >&2; exit 2; }

QDSPLIT=${EXITOS_QDSPLIT_BIN:-./attribution/qdsplit}
KEEPER_BIN=${EXITOS_KEEPER_BIN:-./tools/cstate-keeper}
TESTDISK=${EXITOS_TESTDISK:-tools/testdisk.sh}
SYSROOT=${EXITOS_CPU_SYSFS_ROOT:-/sys/devices/system/cpu}
NVME_BIN=nvme

CPU=${CPU:-170}
ITERS=${ITERS:-1000}
REPS=${REPS:-5}
SETTLE=${SETTLE:-2}
MNT=${EXITOS_TESTMNT:-/mnt/exitos_fs}
OUT_WAS_SET=${OUT+x}
OUT=${OUT:-}
KEEPER_MODES_RAW=${EXITOS_KEEPER_MODES:-off,same}
declare -a KEEPER_MODES=()
KEEPER_MODES_TOKEN=

parse_keeper_modes() {
    local token
    local -A seen=()
    IFS=, read -r -a KEEPER_MODES <<<"$KEEPER_MODES_RAW"
    [ "${#KEEPER_MODES[@]}" -gt 0 ] ||
        die "EXITOS_KEEPER_MODES must select off and/or same"
    KEEPER_MODES_TOKEN=
    for token in "${KEEPER_MODES[@]}"; do
        case "$token" in off|same) ;; *) die "invalid primary keeper mode: $token" ;; esac
        [ -z "${seen[$token]+x}" ] || die "duplicate primary keeper mode: $token"
        seen[$token]=1
        KEEPER_MODES_TOKEN+="${KEEPER_MODES_TOKEN:+,}$token"
    done
}

normalize_uint() {
    local name=$1 min=$2 max=$3 raw=${!1} value
    [[ $raw =~ ^[0-9]+$ ]] && (( ${#raw} <= 10 )) ||
        die "$name must be an integer in [$min,$max]"
    value=$((10#$raw))
    (( value >= min && value <= max )) ||
        die "$name must be an integer in [$min,$max]"
    printf -v "$name" '%u' "$value"
}

# Bounds keep plan expansion, allocation, and accidental waiting finite.  The
# CPU bound matches tools/cstate-keeper's fixed cpu_set_t implementation.
normalize_uint CPU 0 1023
normalize_uint REPS 1 1000
normalize_uint ITERS 1 1000000
normalize_uint SETTLE 0 3600
parse_keeper_modes

TMP=""
RUN_DIR=""
RUN_DIR_ID=""
APP=""
APP_EXECUTION_TOKEN=""
CORE_UTIL_TOKEN=""
KEEPER=""
NOISE_SENTINEL=""
CURRENT_FILE=""
MOUNTED_HERE=0
MOUNT_PENDING=0
MOUNT_ID=""
MOUNT_MAJMIN=""
MOUNT_SOURCE=""
MOUNT_TARGET=""
MOUNT_WAS_EXISTING=0
MOUNT_HOLDER_PID=""
MOUNT_HOLDER_READ_FD=""
MOUNT_HOLDER_WRITE_FD=""
MOUNT_ROOT_PID=""
MOUNT_ROOT_FD=""
MOUNT_ROOT_FD_PATH=""
MOUNT_ROOT_MNT_ID=""
MNT_CREATED=0
MNT_BASE_ID=""
OUTPUT_OPEN=0
OUTPUT_ID=""
PART_FD=""
PART_FD_PATH=""
PART_RDEV_HEX=""
PART_MAJMIN=""
PART_OBJECT_ID=""
PART_SYSFS_PATH=""
WHOLE_FD=""
WHOLE_FD_PATH=""
WHOLE_RDEV_HEX=""
WHOLE_MAJMIN=""
WHOLE_OBJECT_ID=""
WHOLE_SYSFS_PATH=""
WHOLE_CONTROLLER_PATH=""
WHOLE_CONTROLLER_ID=""
WHOLE_PCI_PATH=""
WHOLE_PCI_ID=""
WHOLE_DISKSEQ=""
CHAR_FD=""
CHAR_FD_PATH=""
CHAR_RDEV_HEX=""
CHAR_MAJMIN=""
CHAR_OBJECT_ID=""
CHAR_SYSFS_PATH=""
CHAR_CONTROLLER_PATH=""
CTRL_FD=""
CTRL_FD_PATH=""
CTRL_DEVICE_PATH=""
CTRL_RDEV_HEX=""
CTRL_MAJMIN=""
CTRL_OBJECT_ID=""
CTRL_PATH_ID=""
CTRL_SYSFS_PATH=""
CTRL_SYSFS_ID=""
EXPECTED_NSID=""
NVME_CTRL=""
DEVICE_SYSFS_ROOT=/sys
IRQ_PROC_ROOT=/proc
APP_PROC_ROOT=/proc
DEVICE_TEST_MODE=0
PRIVATE_MOUNT_NAMESPACE=0
PRIVATE_NAMESPACE_ENTRY=0
FULL_RUN_TEST=0
CLEANUP_DONE=0
CLEANUP_STATUS=0
CELL_NOISE_DETECTED=0
CELL_NOISE_REASON=""
PRESERVE_APP_OUTPUT=0
APP_OUTPUT_PRESERVE_REPORTED=0
NOISE_SENTINEL_INTERVAL=1
NOISE_SENTINEL_TRACE=0
CORE_UTIL_THRESHOLD_PCT=15
CORE_UTIL_WINDOWS=30
CORE_UTIL_SEQUENCE_DIR=""
RESOLVED_HCTX=""
RESOLVED_QID=""
RESOLVED_IRQ=""
RESOLVED_IRQ_CONFIGURED=""
RESOLVED_IRQ_EFFECTIVE=""
RESOLVED_BINDING_TOKEN=""
IRQ_TARGET_SELECTED=0
IRQ_TARGET_TOTAL=0
IRQ_SELECTED_NON_TARGET=0
IRQ_SIBLING_NON_TARGET=0
CPUFREQ_TARGET_KHZ=3800000
CPUFREQ_PROFILE_NAME="fixed-max-$CPUFREQ_TARGET_KHZ"
CPUFREQ_PROFILE_TOKEN=""
CPUFREQ_POLICY_COUNT=0
POLL_STATE_DIR=""
POLL_STATE_ID=""
POLL_CAPTURE_FILE=""
POLL_PYTHON_BIN=""
POLL_FRAME=""
POLL_ALIAS_PAUSE=""
POLL_DEVICE_LINK_PAUSE=""
POLL_MQ_LIST_PAUSE=""
declare -a CPUFREQ_POLICY_PATH=()
declare -a CPUFREQ_POLICY_NAME=()
declare -a CPUFREQ_POLICY_ID=()
declare -a CPUFREQ_POLICY_RELATED=()
declare -A CPUFREQ_POLICY_INDEX_BY_PATH=()

current_mount_record() {
    local snapshot="" findmnt_rc=0 snapshot_re source majmin mount_id target
    local source_identity
    [ -n "$PART_MAJMIN" ] || return 2
    snapshot=$(findmnt --kernel --mountpoint "$MNT" --noheadings --pairs \
        -o ID,MAJ:MIN,SOURCE,TARGET) || findmnt_rc=$?
    if [ "$findmnt_rc" -ne 0 ]; then
        [ "$findmnt_rc" -eq 1 ] && [ -z "$snapshot" ] && return 1
        echo "cstate campaign: cannot take unique kernel mount snapshot for $MNT" >&2
        return 2
    fi
    snapshot_re='^ID="([1-9][0-9]*)" MAJ:MIN="([0-9]+:[0-9]+)" SOURCE="([^"[:space:]\\]+)" TARGET="([^"[:space:]\\]+)"$'
    if [[ $snapshot == *$'\n'* || $snapshot == *$'\r'* ||
          ! $snapshot =~ $snapshot_re ]]; then
        echo "cstate campaign: malformed or non-unique kernel mount snapshot" >&2
        return 2
    fi
    mount_id=${BASH_REMATCH[1]}
    majmin=${BASH_REMATCH[2]}
    source=${BASH_REMATCH[3]}
    target=${BASH_REMATCH[4]}
    if [ "$target" != "$MNT" ]; then
        echo "cstate campaign: mount snapshot target mismatch" >&2
        return 2
    fi
    if [ "$majmin" != "$PART_MAJMIN" ]; then
        echo "cstate campaign: mount source dev_t mismatch: expected $PART_MAJMIN, got $majmin" >&2
        return 2
    fi
    [[ $source == /* ]] || return 2
    [ ! -L "$source" ] || return 2
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$source" ] || return 2
        source_identity=$(stat -Lc '%d:%i' -- "$source") || return 2
    else
        [ -b "$source" ] || return 2
        source_identity=$(stat -Lc '%t:%T' -- "$source") || return 2
    fi
    if [ "$source_identity" != "$PART_OBJECT_ID" ]; then
        echo "cstate campaign: mount SOURCE no longer names retained p1" >&2
        return 2
    fi
    printf '%s\t%s\t%s\t%s\n' "$mount_id" "$majmin" "$source" "$target"
}

mounted_source_is_testpart() {
    current_mount_record >/dev/null
}

retained_mount_is_present() {
    local record current_id current_majmin current_source current_target
    [[ $MOUNT_ID =~ ^[1-9][0-9]*$ ]] && [ -n "$MOUNT_SOURCE" ] || return 1
    record=$(current_mount_record) || return 1
    IFS=$'\t' read -r current_id current_majmin current_source current_target <<<"$record"
    if [ "$current_id" != "$MOUNT_ID" ]; then
        echo "cstate campaign: mount ID identity changed: expected $MOUNT_ID, got $current_id" >&2
        return 1
    fi
    if [ "$current_source" != "$MOUNT_SOURCE" ]; then
        echo "cstate campaign: mount SOURCE identity changed: expected $MOUNT_SOURCE, got $current_source" >&2
        return 1
    fi
    [ "$current_majmin" = "$MOUNT_MAJMIN" ] && [ "$current_target" = "$MOUNT_TARGET" ] || return 1
    verify_mount_root_fd || return 1
}

verify_retained_mount() {
    local phase=$1
    if [ "$FULL_RUN_TEST" = 1 ] &&
       [ "${EXITOS_TEST_FAILPOINT:-}" = "mount:$phase" ]; then
        return 1
    fi
    retained_mount_is_present || return 1
    printf 'MOUNT_IDENTITY phase=%s id=%s source=%s part_dev_t=%s existing=%s status=verified\n' \
        "$phase" "$MOUNT_ID" "$MOUNT_SOURCE" "$PART_MAJMIN" \
        "$MOUNT_WAS_EXISTING"
}

read_mount_root_mnt_id() {
    local fdinfo line key value extra count=0 found=""
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        fdinfo=${EXITOS_TEST_MOUNT_FD_ID_FILE:?pure test requires mount-fd mnt_id fixture}
    else
        fdinfo="/proc/$MOUNT_ROOT_PID/fdinfo/$MOUNT_ROOT_FD"
    fi
    [ -f "$fdinfo" ] || return 1
    while IFS= read -r line; do
        IFS=$' \t' read -r key value extra <<<"$line"
        if [ "$key" = mnt_id: ]; then
            [ -z "$extra" ] && [[ $value =~ ^[1-9][0-9]*$ ]] || return 1
            count=$((count + 1))
            found=$value
        fi
    done <"$fdinfo"
    [ "$count" -eq 1 ] || return 1
    printf '%s\n' "$found"
}

verify_mount_root_fd() {
    local current
    [[ $MOUNT_ROOT_PID =~ ^[1-9][0-9]*$ && $MOUNT_ROOT_FD =~ ^[0-9]+$ &&
       $MOUNT_ROOT_MNT_ID =~ ^[1-9][0-9]*$ ]] || return 1
    current=$(read_mount_root_mnt_id) || {
        echo "cstate campaign: cannot read retained mount-root fd mnt_id" >&2
        return 1
    }
    if [ "$current" != "$MOUNT_ROOT_MNT_ID" ] || [ "$current" != "$MOUNT_ID" ]; then
        echo "cstate campaign: retained mount-root fd mnt_id changed" >&2
        return 1
    fi
}

# Bash dynamic descriptors are not a child-ownership boundary.  Every
# long-lived child closes the campaign's four retained device descriptors in
# its own subshell before entering helper code or exec.  The parent copies stay
# open until cleanup_resources() so identity checks remain authoritative.
close_inherited_device_fds() {
    if [ -n "$WHOLE_FD" ]; then exec {WHOLE_FD}<&- || return 1; fi
    if [ -n "$PART_FD" ]; then exec {PART_FD}<&- || return 1; fi
    if [ -n "$CHAR_FD" ]; then exec {CHAR_FD}<&- || return 1; fi
    if [ -n "$CTRL_FD" ]; then exec {CTRL_FD}<&- || return 1; fi
}

poll_process_starttime() {
    local outvar=$1 line rest
    local -a fields=()
    IFS= read -r line <"/proc/$$/stat" || return 1
    [[ $line == *") "* ]] || return 1
    rest=${line##*) }
    read -r -a fields <<<"$rest" || return 1
    [ "${#fields[@]}" -ge 20 ] &&
        [[ ${fields[19]} =~ ^[1-9][0-9]*$ ]] || return 1
    printf -v "$outvar" '%s' "${fields[19]}"
}

poll_parent_pause() {
    local marker=$1 starttime
    poll_process_starttime starttime || return 1
    printf '%s pid=%s starttime=%s\n' "$marker" "$$" "$starttime" >&2
    kill -STOP "$$"
}

prepare_poll_state() {
    local state_parent canonical_parent old_umask created=0
    POLL_PYTHON_BIN=$(command -v python3) || die "python3 is required for poll snapshot"
    [[ $POLL_PYTHON_BIN == /* ]] || die "poll snapshot python3 path is not absolute"
    POLL_PYTHON_BIN=$(readlink -f -- "$POLL_PYTHON_BIN") ||
        die "cannot resolve poll snapshot python3"
    [ -f "$POLL_PYTHON_BIN" ] && [ -x "$POLL_PYTHON_BIN" ] &&
        [ ! -L "$POLL_PYTHON_BIN" ] || die "poll snapshot python3 is unsafe"

    state_parent=${TMPDIR:-/tmp}
    [[ $state_parent == /* && $state_parent != *$'\n'* &&
       -d $state_parent && ! -L $state_parent ]] ||
        die "poll snapshot TMPDIR is unsafe"
    canonical_parent=$(readlink -f -- "$state_parent") ||
        die "cannot resolve poll snapshot TMPDIR"
    [ "$canonical_parent" = "$state_parent" ] ||
        die "poll snapshot TMPDIR is not canonical"
    POLL_STATE_DIR=$(mktemp -d "$state_parent/exitos-poll-snapshot.XXXXXX") ||
        die "cannot create private poll snapshot state directory"
    chmod 0700 -- "$POLL_STATE_DIR" ||
        die "cannot protect poll snapshot state directory"
    POLL_STATE_ID=$(stat -Lc '%d:%i:%u:%a' -- "$POLL_STATE_DIR") ||
        die "cannot retain poll snapshot state identity"
    [ "$POLL_STATE_ID" = "$(stat -Lc '%d:%i:%u:700' -- "$POLL_STATE_DIR")" ] ||
        die "poll snapshot state directory is not private"
    [ "${POLL_STATE_ID%:700}" = "$(stat -Lc '%d:%i:%u' -- "$POLL_STATE_DIR")" ] ||
        die "poll snapshot state identity is malformed"
    [ "$(stat -Lc '%u' -- "$POLL_STATE_DIR")" = "$(id -u)" ] ||
        die "poll snapshot state directory is not owned by this uid"
    POLL_CAPTURE_FILE="$POLL_STATE_DIR/frame"
    old_umask=$(umask)
    umask 077
    set -C
    if { : >"$POLL_CAPTURE_FILE"; } 2>/dev/null; then created=1; fi
    set +C
    umask "$old_umask"
    [ "$created" = 1 ] || die "cannot exclusively create poll snapshot state file"
}

retain_poll_test_devices() {
    local path current
    local -A seen_identity=() seen_fd=()
    for path in "$EXITOS_DEV" "$EXITOS_TESTPART" "$EXITOS_CHARDEV" \
                "$CTRL_DEVICE_PATH"; do
        [[ $path == /* && $path != *$'\n'* && -f $path && ! -L $path ]] ||
            die "invalid pure-test poll device path"
    done

    # Resolve all pathname identities before opening the first retained fd, so
    # no external utility can inherit a retained device descriptor.
    WHOLE_OBJECT_ID=$(stat -Lc '%d:%i' -- "$EXITOS_DEV") ||
        die "cannot identify pure-test poll whole device"
    PART_OBJECT_ID=$(stat -Lc '%d:%i' -- "$EXITOS_TESTPART") ||
        die "cannot identify pure-test poll partition"
    CHAR_OBJECT_ID=$(stat -Lc '%d:%i' -- "$EXITOS_CHARDEV") ||
        die "cannot identify pure-test poll generic device"
    CTRL_OBJECT_ID=$(stat -Lc '%d:%i' -- "$CTRL_DEVICE_PATH") ||
        die "cannot identify pure-test poll controller device"
    for current in "$WHOLE_OBJECT_ID" "$PART_OBJECT_ID" \
                   "$CHAR_OBJECT_ID" "$CTRL_OBJECT_ID"; do
        [[ $current =~ ^[0-9]+:[1-9][0-9]*$ ]] &&
            [ -z "${seen_identity[$current]+x}" ] ||
            die "pure-test poll device identities are not distinct"
        seen_identity[$current]=1
    done

    exec {PART_FD}<"$EXITOS_TESTPART" || die "cannot retain pure-test poll partition"
    exec {WHOLE_FD}<"$EXITOS_DEV" || die "cannot retain pure-test poll whole device"
    exec {CTRL_FD}<"$CTRL_DEVICE_PATH" || die "cannot retain pure-test poll controller"
    exec {CHAR_FD}<"$EXITOS_CHARDEV" || die "cannot retain pure-test poll generic device"
    PART_FD_PATH="/proc/$$/fd/$PART_FD"
    WHOLE_FD_PATH="/proc/$$/fd/$WHOLE_FD"
    CTRL_FD_PATH="/proc/$$/fd/$CTRL_FD"
    CHAR_FD_PATH="/proc/$$/fd/$CHAR_FD"
    [[ $EXITOS_TESTPART -ef $PART_FD_PATH &&
       $EXITOS_DEV -ef $WHOLE_FD_PATH &&
       $CTRL_DEVICE_PATH -ef $CTRL_FD_PATH &&
       $EXITOS_CHARDEV -ef $CHAR_FD_PATH ]] ||
        die "pure-test poll device changed during retention"
    for current in "$WHOLE_FD" "$PART_FD" "$CHAR_FD" "$CTRL_FD"; do
        [[ $current =~ ^[1-9][0-9]*$ ]] && [ "$current" != 9 ] &&
            [ -z "${seen_fd[$current]+x}" ] ||
            die "poll snapshot retained fd set is invalid"
        seen_fd[$current]=1
        retained_fd_is_read_only "$current" ||
            die "poll snapshot retained fd is not read-only"
    done

    WHOLE_MAJMIN=${EXITOS_TEST_WHOLE_MAJMIN:?pure test requires whole MAJ:MIN}
    PART_MAJMIN=${EXITOS_TEST_PART_MAJMIN:?pure test requires partition MAJ:MIN}
    CHAR_MAJMIN=${EXITOS_TEST_CHAR_MAJMIN:?pure test requires generic MAJ:MIN}
    CTRL_MAJMIN=${EXITOS_TEST_CTRL_MAJMIN:?pure test requires controller MAJ:MIN}
    CTRL_PATH_ID=$CTRL_OBJECT_ID
}

launch_poll_snapshot_helper() {
    local current
    local -A seen_fd=()
    for current in "$WHOLE_FD" "$PART_FD" "$CHAR_FD" "$CTRL_FD"; do
        [[ $current =~ ^[1-9][0-9]*$ ]] && [ "$current" != 9 ] &&
            [ -z "${seen_fd[$current]+x}" ] || {
                echo "poll snapshot: launcher_fd_set_invalid" >&2
                return 1
            }
        seen_fd[$current]=1
    done
    (
        trap - EXIT INT TERM
        if ! { exec 9<&"$WHOLE_FD"; } 2>/dev/null; then
            echo "poll snapshot: fd9_dup_failed" >&2
            exit 1
        fi
        close_inherited_device_fds || exit 1
        exec "$POLL_PYTHON_BIN" -I -S - \
            "$DEVICE_SYSFS_ROOT" "$POLL_PROC_ROOT" \
            "$POLL_FAKE_DISKSEQ_FILE" "$WHOLE_OBJECT_ID" \
            "$WHOLE_MAJMIN" "$PART_MAJMIN" "$CHAR_MAJMIN" \
            "$CTRL_MAJMIN" "$POLL_EXPECTED_CTRL_SYSFS" \
            "$EXITOS_EXPECT_SERIAL" "$POLL_HELPER_PAUSE" \
            "$POLL_ALIAS_PAUSE" "$POLL_DEVICE_LINK_PAUSE" \
            "$POLL_MQ_LIST_PAUSE" "$POLL_CAPTURE1_PAUSE" \
            "$POLL_PARTIAL_OUTPUT" <<'PY'
import errno
import fcntl
import hashlib
import os
import re
import signal
import stat
import struct
import sys

BLKGETDISKSEQ = 0x80081280
UINT64_MAX = (1 << 64) - 1
UINT32_MAX = (1 << 32) - 1
INT32_MAX = (1 << 31) - 1
SAFE_PATH_RE = re.compile(r"/[A-Za-z0-9_./:+-]+")
CANON_UINT_RE = re.compile(r"0|[1-9][0-9]*")
POS_UINT_RE = re.compile(r"[1-9][0-9]*")
UUID_RE = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
)
PAUSE_HOOKS = {}


class SnapshotError(Exception):
    pass


def fail(code):
    raise SnapshotError(code)


def live_fds():
    found = set()
    try:
        names = os.listdir("/proc/self/fd")
    except OSError:
        fail("fd_enumeration_failed")
    for name in names:
        if not name.isdigit():
            fail("fd_enumeration_malformed")
        fd = int(name, 10)
        try:
            os.fstat(fd)
        except OSError as exc:
            if exc.errno == errno.EBADF:
                continue
            fail("fd_enumeration_failed")
        found.add(fd)
    return found


def close_unwanted_fds():
    for _ in range(16):
        unwanted = sorted(fd for fd in live_fds() if fd not in (0, 1, 2, 9))
        if not unwanted:
            break
        for fd in unwanted:
            try:
                os.close(fd)
            except OSError as exc:
                if exc.errno != errno.EBADF:
                    fail("fd_close_failed")
    if live_fds() != {0, 1, 2, 9}:
        fail("fd_close_failed")


def process_starttime():
    try:
        with open("/proc/self/stat", "rb", buffering=0) as stream:
            raw = stream.read(4097)
    except OSError:
        fail("proc_starttime_failed")
    if len(raw) > 4096 or not raw.endswith(b"\n"):
        fail("proc_starttime_failed")
    try:
        rest = raw.rstrip(b"\n").rsplit(b") ", 1)[1].split()
    except (IndexError, ValueError):
        fail("proc_starttime_failed")
    if len(rest) < 20 or not POS_UINT_RE.fullmatch(rest[19].decode("ascii", "strict")):
        fail("proc_starttime_failed")
    return rest[19].decode("ascii")


def pause(marker):
    sys.stderr.write(f"{marker} pid={os.getpid()} starttime={process_starttime()}\n")
    sys.stderr.flush()
    os.kill(os.getpid(), signal.SIGSTOP)


def pause_once(name, marker):
    if PAUSE_HOOKS.get(name, False):
        PAUSE_HOOKS[name] = False
        pause(marker)


def parse_canonical_uint(raw, code, maximum, positive=False):
    pattern = POS_UINT_RE if positive else CANON_UINT_RE
    if not pattern.fullmatch(raw) or len(raw) > 20:
        fail(code)
    value = int(raw, 10)
    if value > maximum or (positive and value == 0):
        fail(code)
    return value


def parse_dev_t(raw, code):
    match = re.fullmatch(r"(0|[1-9][0-9]*):(0|[1-9][0-9]*)", raw)
    if not match:
        fail(code)
    major = parse_canonical_uint(match.group(1), code, UINT32_MAX)
    minor = parse_canonical_uint(match.group(2), code, UINT32_MAX)
    return f"{major}:{minor}"


def read_fd_all(fd, code, maximum=4096, use_pread=False):
    chunks = []
    total = 0
    offset = 0
    while True:
        try:
            if use_pread:
                chunk = os.pread(fd, min(4097, maximum + 1 - total), offset)
                offset += len(chunk)
            else:
                chunk = os.read(fd, min(4097, maximum + 1 - total))
        except OSError:
            fail(code)
        if not chunk:
            break
        chunks.append(chunk)
        total += len(chunk)
        if total > maximum:
            fail(code)
    return b"".join(chunks)


def decode_one_line(data, code, allow_empty=False):
    if not data.endswith(b"\n") or data.count(b"\n") != 1:
        fail(code)
    if b"\x00" in data or b"\r" in data:
        fail(code)
    try:
        value = data[:-1].decode("ascii", "strict")
    except UnicodeDecodeError:
        fail(code)
    if not allow_empty and value == "":
        fail(code)
    return value


def read_at(dir_fd, name, code, allow_empty=False, content_code=None):
    if "/" in name or name in ("", ".", ".."):
        fail(code)
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        fd = os.open(name, flags, dir_fd=dir_fd)
    except OSError:
        fail(code)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            fail(code)
        if content_code is None:
            content_code = code
        return decode_one_line(
            read_fd_all(fd, content_code), content_code, allow_empty
        )
    finally:
        os.close(fd)


def canonical_fd_path(fd, code):
    try:
        value = os.readlink(f"/proc/self/fd/{fd}")
    except OSError:
        fail(code)
    if (
        not SAFE_PATH_RE.fullmatch(value)
        or os.path.realpath(value) != value
    ):
        fail(code)
    return value


def open_dir_path(path, root, code, opened):
    if not SAFE_PATH_RE.fullmatch(path) or os.path.realpath(path) != path:
        fail(code)
    if path != root and not path.startswith(root + "/"):
        fail(code)
    try:
        before = os.lstat(path)
        if not stat.S_ISDIR(before.st_mode) or stat.S_ISLNK(before.st_mode):
            fail(code)
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError:
        fail(code)
    opened.append(fd)
    after = os.fstat(fd)
    if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
        fail(code)
    if canonical_fd_path(fd, code) != path:
        fail(code)
    return fd, after


def open_dir_at(parent_fd, parent_path, name, root, code, opened):
    if not re.fullmatch(r"[A-Za-z0-9_.:+-]+", name):
        fail(code)
    expected = parent_path + "/" + name
    try:
        fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
    except OSError:
        fail(code)
    opened.append(fd)
    info = os.fstat(fd)
    if not stat.S_ISDIR(info.st_mode) or canonical_fd_path(fd, code) != expected:
        fail(code)
    if expected != root and not expected.startswith(root + "/"):
        fail(code)
    return fd, info, expected


def resolve_alias(
    alias,
    root,
    code,
    changed_code,
    opened,
    pause_name=None,
    pause_marker=None,
):
    if not SAFE_PATH_RE.fullmatch(alias):
        fail(code)
    parent_path = os.path.dirname(alias)
    name = os.path.basename(alias)
    if not re.fullmatch(r"[A-Za-z0-9_.:+-]+", name):
        fail(code)
    parent_fd, _ = open_dir_path(parent_path, root, code, opened)
    try:
        before = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        link_value = os.readlink(name, dir_fd=parent_fd)
    except OSError:
        fail(code)
    if not stat.S_ISLNK(before.st_mode):
        fail(code)
    declared_target = os.path.realpath(os.path.join(parent_path, link_value))
    if pause_name is not None:
        pause_once(pause_name, pause_marker)
    try:
        fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC,
            dir_fd=parent_fd,
        )
    except OSError:
        fail(changed_code)
    opened.append(fd)
    try:
        info = os.fstat(fd)
        target = canonical_fd_path(fd, code)
        after = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        after_value = os.readlink(name, dir_fd=parent_fd)
    except OSError:
        fail(changed_code)
    if (
        not stat.S_ISDIR(info.st_mode)
        or not stat.S_ISLNK(after.st_mode)
        or (before.st_dev, before.st_ino, before.st_mode)
        != (after.st_dev, after.st_ino, after.st_mode)
        or link_value != after_value
    ):
        fail(changed_code)
    if target != declared_target:
        fail(changed_code)
    if target != root and not target.startswith(root + "/"):
        fail(code)
    return target, fd, info


def resolve_dir_link(
    dir_fd,
    dir_path,
    name,
    root,
    code,
    changed_code,
    opened,
    pause_name=None,
    pause_marker=None,
):
    try:
        before = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        link_value = os.readlink(name, dir_fd=dir_fd)
    except OSError:
        fail(code)
    if not stat.S_ISLNK(before.st_mode):
        fail(code)
    declared_target = os.path.realpath(os.path.join(dir_path, link_value))
    if pause_name is not None:
        pause_once(pause_name, pause_marker)
    try:
        target_fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC,
            dir_fd=dir_fd,
        )
    except OSError:
        fail(changed_code)
    opened.append(target_fd)
    try:
        target_info = os.fstat(target_fd)
        target = canonical_fd_path(target_fd, code)
        after = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        after_value = os.readlink(name, dir_fd=dir_fd)
    except OSError:
        fail(changed_code)
    if (
        not stat.S_ISDIR(target_info.st_mode)
        or not stat.S_ISLNK(after.st_mode)
        or (before.st_dev, before.st_ino, before.st_mode)
        != (after.st_dev, after.st_ino, after.st_mode)
        or link_value != after_value
    ):
        fail(changed_code)
    if target != declared_target:
        fail(changed_code)
    if target != root and not target.startswith(root + "/"):
        fail(code)
    return target, target_fd, target_info


def validate_fake_root(path, code):
    if not SAFE_PATH_RE.fullmatch(path) or path == "/":
        fail(code)
    for forbidden in ("/sys", "/proc"):
        if path == forbidden or path.startswith(forbidden + "/"):
            fail(code)
    try:
        info = os.lstat(path)
    except OSError:
        fail(code)
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        fail(code)
    if os.path.realpath(path) != path:
        fail(code)


def decode_diskseq_buffer(diskseq_buffer):
    if len(diskseq_buffer) != 8:
        fail("diskseq_abi_invalid")
    try:
        return struct.unpack("=Q", diskseq_buffer)[0]
    except struct.error:
        fail("diskseq_abi_invalid")


def read_fake_diskseq(path):
    if not SAFE_PATH_RE.fullmatch(path) or os.path.realpath(path) != path:
        fail("fake_diskseq_invalid")
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        before = os.lstat(path)
        fd = os.open(path, flags)
    except OSError:
        fail("fake_diskseq_invalid")
    try:
        after = os.fstat(fd)
        if not stat.S_ISREG(after.st_mode) or after.st_uid != os.geteuid():
            fail("fake_diskseq_invalid")
        if after.st_mode & 0o022:
            fail("fake_diskseq_invalid")
        if (before.st_dev, before.st_ino, before.st_mode, before.st_uid) != (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_uid,
        ):
            fail("fake_diskseq_invalid")
        raw = decode_one_line(
            read_fd_all(fd, "fake_diskseq_malformed", use_pread=True),
            "fake_diskseq_malformed",
        )
        value = parse_canonical_uint(raw, "fake_diskseq_malformed", UINT64_MAX)
        return decode_diskseq_buffer(struct.pack("=Q", value))
    finally:
        os.close(fd)


def read_production_diskseq():
    if os.uname().machine != "x86_64" or struct.calcsize("=Q") != 8:
        fail("diskseq_abi_invalid")
    diskseq_buffer = bytearray(8)
    try:
        ioctl_rc = fcntl.ioctl(9, BLKGETDISKSEQ, diskseq_buffer, True)
    except (OSError, TypeError, ValueError):
        fail("diskseq_ioctl_failed")
    if ioctl_rc != 0:
        fail("diskseq_ioctl_failed")
    if len(diskseq_buffer) != 8:
        fail("diskseq_abi_invalid")
    return decode_diskseq_buffer(diskseq_buffer)


def canonical_cpu_list(raw):
    if raw == "":
        return "-"
    intervals = []
    for index, item in enumerate(raw.split(",")):
        if index == 0:
            if item.startswith(" "):
                fail("mq_cpu_list_malformed")
        else:
            item = item.lstrip(" ")
        match = re.fullmatch(
            r"(0|[1-9][0-9]*)(?:-(0|[1-9][0-9]*))?",
            item,
        )
        if not match:
            fail("mq_cpu_list_malformed")
        first = parse_canonical_uint(match.group(1), "mq_cpu_list_malformed", INT32_MAX)
        last = first
        if match.group(2) is not None:
            last = parse_canonical_uint(match.group(2), "mq_cpu_list_malformed", INT32_MAX)
        if first > last:
            fail("mq_cpu_list_malformed")
        intervals.append((first, last))
    intervals.sort()
    merged = []
    for first, last in intervals:
        if merged and first <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], last))
        else:
            merged.append((first, last))
    return ",".join(
        str(first) if first == last else f"{first}-{last}" for first, last in merged
    )


def inode_token(info):
    if info.st_dev < 0 or info.st_ino <= 0:
        fail("inode_invalid")
    return f"{info.st_dev}:{info.st_ino}"


def capture(sysroot, procroot, diskseq_path, dev_ts, expected_ctrl, expected_serial):
    opened = []
    try:
        sys_fd, _ = open_dir_path(sysroot, sysroot, "fake_sys_root_invalid", opened)
        module_fd, _, module_path = open_dir_at(
            sys_fd, sysroot, "module", sysroot, "poll_queues_missing", opened
        )
        nvme_module_fd, _, nvme_module_path = open_dir_at(
            module_fd, module_path, "nvme", sysroot, "poll_queues_missing", opened
        )
        params_fd, _, _ = open_dir_at(
            nvme_module_fd,
            nvme_module_path,
            "parameters",
            sysroot,
            "poll_queues_missing",
            opened,
        )
        poll_queues_raw = read_at(params_fd, "poll_queues", "poll_queues_malformed")
        poll_queues = parse_canonical_uint(
            poll_queues_raw, "poll_queues_malformed", UINT32_MAX, positive=True
        )

        proc_fd, _ = open_dir_path(procroot, procroot, "fake_proc_root_invalid", opened)
        proc_sys_fd, _, proc_sys_path = open_dir_at(
            proc_fd, procroot, "sys", procroot, "boot_id_missing", opened
        )
        kernel_fd, _, kernel_path = open_dir_at(
            proc_sys_fd, proc_sys_path, "kernel", procroot, "boot_id_missing", opened
        )
        random_fd, _, _ = open_dir_at(
            kernel_fd, kernel_path, "random", procroot, "boot_id_missing", opened
        )
        boot_id = read_at(random_fd, "boot_id", "boot_id_malformed")
        if not UUID_RE.fullmatch(boot_id):
            fail("boot_id_malformed")

        whole_target, whole_fd, whole_info = resolve_alias(
            f"{sysroot}/dev/block/{dev_ts[0]}",
            sysroot,
            "whole_target_invalid",
            "whole_alias_changed",
            opened,
            "whole_alias",
            "POLL_SNAPSHOT_ALIAS_READY",
        )
        part_target, part_fd, part_info = resolve_alias(
            f"{sysroot}/dev/block/{dev_ts[1]}",
            sysroot,
            "part_target_invalid",
            "part_alias_changed",
            opened,
        )
        generic_target, generic_fd, generic_info = resolve_alias(
            f"{sysroot}/dev/char/{dev_ts[2]}",
            sysroot,
            "generic_target_invalid",
            "generic_alias_changed",
            opened,
        )
        controller_char_target, controller_char_fd, controller_char_info = resolve_alias(
            f"{sysroot}/dev/char/{dev_ts[3]}",
            sysroot,
            "controller_char_target_invalid",
            "controller_char_alias_changed",
            opened,
        )
        for target in (
            whole_target,
            part_target,
            generic_target,
            controller_char_target,
        ):
            if not SAFE_PATH_RE.fullmatch(target):
                fail("target_path_invalid")

        if read_at(whole_fd, "dev", "whole_dev_t_mismatch") != dev_ts[0]:
            fail("whole_dev_t_mismatch")
        if read_at(part_fd, "dev", "part_dev_t_mismatch") != dev_ts[1]:
            fail("part_dev_t_mismatch")
        if read_at(generic_fd, "dev", "generic_dev_t_mismatch") != dev_ts[2]:
            fail("generic_dev_t_mismatch")
        if read_at(controller_char_fd, "dev", "controller_dev_t_mismatch") != dev_ts[3]:
            fail("controller_dev_t_mismatch")
        if read_at(part_fd, "partition", "partition_invalid") != "1":
            fail("partition_invalid")
        if os.path.dirname(part_target) != whole_target:
            fail("partition_parent_mismatch")

        nsid_raw = read_at(whole_fd, "nsid", "nsid_malformed")
        nsid = parse_canonical_uint(nsid_raw, "nsid_malformed", UINT32_MAX, positive=True)
        diskseq_sysfs_raw = read_at(whole_fd, "diskseq", "diskseq_sysfs_malformed")
        diskseq_sysfs = parse_canonical_uint(
            diskseq_sysfs_raw, "diskseq_sysfs_malformed", UINT64_MAX
        )
        diskseq_fd = read_fake_diskseq(diskseq_path)
        if diskseq_fd != diskseq_sysfs:
            fail("diskseq_mismatch")

        controller_target, controller_fd, controller_info = resolve_dir_link(
            whole_fd,
            whole_target,
            "device",
            sysroot,
            "whole_controller_mismatch",
            "whole_controller_alias_changed",
            opened,
            "whole_device",
            "POLL_SNAPSHOT_DEVICE_LINK_READY",
        )
        generic_controller, _, generic_controller_info = resolve_dir_link(
            generic_fd,
            generic_target,
            "device",
            sysroot,
            "generic_controller_mismatch",
            "generic_controller_alias_changed",
            opened,
        )
        if controller_target != generic_controller or controller_target != controller_char_target:
            fail("controller_relationship_mismatch")
        if (
            inode_token(controller_info) != inode_token(generic_controller_info)
            or inode_token(controller_info) != inode_token(controller_char_info)
        ):
            fail("controller_relationship_mismatch")
        if expected_ctrl != controller_target:
            fail("controller_target_mismatch")
        if os.path.dirname(whole_target) != controller_target or os.path.dirname(generic_target) != controller_target:
            fail("controller_child_mismatch")
        if not re.fullmatch(r"nvme[0-9]+", os.path.basename(controller_target)):
            fail("controller_name_malformed")
        if not re.fullmatch(r"nvme[0-9]+n[1-9][0-9]*", os.path.basename(whole_target)):
            fail("namespace_name_malformed")
        if os.path.basename(part_target) != os.path.basename(whole_target) + "p1":
            fail("partition_name_mismatch")
        if not re.fullmatch(r"ng[0-9]+n[1-9][0-9]*", os.path.basename(generic_target)):
            fail("generic_name_malformed")

        serial_value = read_at(controller_fd, "serial", "serial_malformed").replace(" ", "")
        if serial_value != expected_serial:
            fail("serial_mismatch")
        pci_target, pci_fd, pci_info = resolve_dir_link(
            controller_fd,
            controller_target,
            "device",
            sysroot,
            "pci_target_invalid",
            "pci_alias_changed",
            opened,
        )
        if os.path.dirname(os.path.dirname(controller_target)) != pci_target:
            fail("pci_controller_relationship_mismatch")
        open_dir_at(pci_fd, pci_target, "msi_irqs", sysroot, "pci_msi_missing", opened)

        queue_fd, queue_info, queue_target = open_dir_at(
            whole_fd, whole_target, "queue", sysroot, "queue_invalid", opened
        )
        if read_at(queue_fd, "io_poll", "io_poll_malformed") != "1":
            fail("io_poll_malformed")
        mq_fd, _, mq_target = open_dir_at(
            whole_fd, whole_target, "mq", sysroot, "mq_missing", opened
        )
        try:
            mq_names = sorted(os.listdir(mq_fd))
        except OSError:
            fail("mq_missing")
        pause_once("mq_membership", "POLL_SNAPSHOT_MQ_LIST_READY")
        try:
            if sorted(os.listdir(mq_fd)) != mq_names:
                fail("mq_membership_unstable")
        except OSError:
            fail("mq_membership_unstable")
        hctx = []
        for name in mq_names:
            try:
                entry_info = os.stat(name, dir_fd=mq_fd, follow_symlinks=False)
            except OSError:
                fail("mq_hctx_stat_failed")
            if name.isdigit():
                if not CANON_UINT_RE.fullmatch(name):
                    fail("mq_hctx_name_malformed")
                number = parse_canonical_uint(name, "mq_hctx_name_malformed", INT32_MAX)
                if stat.S_ISLNK(entry_info.st_mode):
                    fail("mq_hctx_symlink")
                hctx_fd, hctx_info, _ = open_dir_at(
                    mq_fd, mq_target, name, sysroot, "mq_hctx_invalid", opened
                )
                if (entry_info.st_dev, entry_info.st_ino) != (
                    hctx_info.st_dev,
                    hctx_info.st_ino,
                ):
                    fail("mq_hctx_changed")
                cpu_raw = read_at(
                    hctx_fd,
                    "cpu_list",
                    "mq_cpu_list_missing",
                    allow_empty=True,
                    content_code="mq_cpu_list_malformed",
                )
                cpu_list = canonical_cpu_list(cpu_raw)
                hctx.append((number, inode_token(hctx_info), cpu_list))
            else:
                fail("mq_hctx_name_malformed")
        try:
            if sorted(os.listdir(mq_fd)) != mq_names:
                fail("mq_membership_unstable")
        except OSError:
            fail("mq_membership_unstable")
        if not hctx:
            fail("mq_empty")
        hctx.sort(key=lambda row: row[0])
        if len(hctx) > 65536:
            fail("mq_count_invalid")

        lines = [
            "format=exitos-poll-generation-v1",
            f"boot_id={boot_id}",
            f"poll_queues={poll_queues}",
            "io_poll=1",
            f"whole_dev_t={dev_ts[0]}",
            f"part_dev_t={dev_ts[1]}",
            f"generic_dev_t={dev_ts[2]}",
            f"controller_dev_t={dev_ts[3]}",
            f"diskseq_fd={diskseq_fd}",
            f"diskseq_sysfs={diskseq_sysfs}",
            f"nsid={nsid}",
            f"whole_target={whole_target}",
            f"whole_inode={inode_token(whole_info)}",
            f"part_target={part_target}",
            f"part_inode={inode_token(part_info)}",
            f"generic_target={generic_target}",
            f"generic_inode={inode_token(generic_info)}",
            f"controller_char_target={controller_char_target}",
            f"controller_char_inode={inode_token(controller_char_info)}",
            f"queue_target={queue_target}",
            f"queue_inode={inode_token(queue_info)}",
            f"controller_target={controller_target}",
            f"controller_inode={inode_token(controller_info)}",
            f"pci_target={pci_target}",
            f"pci_inode={inode_token(pci_info)}",
            f"mq_count={len(hctx)}",
        ]
        lines.extend(f"mq_hctx={number}:{identity}:{cpus}" for number, identity, cpus in hctx)
        body = ("\n".join(lines) + "\n").encode("ascii", "strict")
        if b"\x00" in body or b"\r" in body or not body.endswith(b"\n"):
            fail("frame_malformed")
        return body
    except UnicodeEncodeError:
        fail("frame_malformed")
    finally:
        for fd in reversed(opened):
            try:
                os.close(fd)
            except OSError:
                pass


def main():
    if len(sys.argv) != 17:
        fail("helper_arguments_invalid")
    (
        sysroot,
        procroot,
        diskseq_path,
        expected_whole_identity,
        whole_dev_t,
        part_dev_t,
        generic_dev_t,
        controller_dev_t,
        expected_ctrl,
        expected_serial,
        helper_pause,
        alias_pause,
        device_link_pause,
        mq_list_pause,
        capture1_pause,
        partial_output,
    ) = sys.argv[1:]

    for hook in (
        helper_pause,
        alias_pause,
        device_link_pause,
        mq_list_pause,
        capture1_pause,
        partial_output,
    ):
        if hook not in ("", "1"):
            fail("helper_arguments_invalid")
    PAUSE_HOOKS.update(
        whole_alias=alias_pause == "1",
        whole_device=device_link_pause == "1",
        mq_membership=mq_list_pause == "1",
    )

    close_unwanted_fds()
    try:
        os.set_inheritable(9, False)
        fd9_info = os.fstat(9)
        fd9_flags = fcntl.fcntl(9, fcntl.F_GETFL)
    except (OSError, ValueError):
        fail("fd9_invalid")
    if os.get_inheritable(9) or fd9_flags & os.O_ACCMODE != os.O_RDONLY:
        fail("fd9_invalid")
    if not stat.S_ISREG(fd9_info.st_mode):
        fail("fd9_type_invalid")
    if f"{fd9_info.st_dev}:{fd9_info.st_ino}" != expected_whole_identity:
        fail("whole_fd_identity_mismatch")

    if helper_pause == "1":
        pause("POLL_SNAPSHOT_HELPER_READY")
    if partial_output == "1":
        os.write(1, b"snapshot_sha256=partial\n")
        pause("POLL_SNAPSHOT_PARTIAL_READY")
        fail("injected_partial_output")

    validate_fake_root(sysroot, "fake_sys_root_invalid")
    validate_fake_root(procroot, "fake_proc_root_invalid")
    dev_ts = (
        parse_dev_t(whole_dev_t, "whole_dev_t_malformed"),
        parse_dev_t(part_dev_t, "part_dev_t_malformed"),
        parse_dev_t(generic_dev_t, "generic_dev_t_malformed"),
        parse_dev_t(controller_dev_t, "controller_dev_t_malformed"),
    )
    if len(set(dev_ts)) != 4:
        fail("device_dev_t_not_distinct")
    if not SAFE_PATH_RE.fullmatch(expected_ctrl) or os.path.realpath(expected_ctrl) != expected_ctrl:
        fail("controller_target_mismatch")
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", expected_serial):
        fail("serial_malformed")

    body1 = capture(sysroot, procroot, diskseq_path, dev_ts, expected_ctrl, expected_serial)
    if capture1_pause == "1":
        pause("POLL_SNAPSHOT_CAPTURE1_READY")
    body2 = capture(sysroot, procroot, diskseq_path, dev_ts, expected_ctrl, expected_serial)
    if body1 != body2:
        fail("snapshot_unstable")
    digest = hashlib.sha256(body1).hexdigest().encode("ascii")
    os.write(1, b"snapshot_sha256=" + digest + b"\n" + body1)


try:
    main()
except SnapshotError as exc:
    sys.stderr.write(f"poll snapshot: {exc}\n")
    sys.stderr.flush()
    raise SystemExit(1)
except (OSError, ValueError, UnicodeError, OverflowError):
    sys.stderr.write("poll snapshot: internal_error\n")
    sys.stderr.flush()
    raise SystemExit(1)
PY
    ) >"$POLL_CAPTURE_FILE"
}

validate_canonical_poll_cpu_list() {
    local raw=$1 item first last previous_last=0 have_previous=0
    local -a items=()
    [ "$raw" = - ] && return 0
    [ -n "$raw" ] || return 1
    [[ $raw != *, ]] || return 1
    IFS=, read -r -a items <<<"$raw" || return 1
    [ "${#items[@]}" -gt 0 ] || return 1
    for item in "${items[@]}"; do
        [[ $item =~ ^(0|[1-9][0-9]*)(-([1-9][0-9]*|0))?$ ]] || return 1
        first=${BASH_REMATCH[1]}
        last=${BASH_REMATCH[3]:-$first}
        [ "${#first}" -le 10 ] && [ "${#last}" -le 10 ] || return 1
        first=$((10#$first))
        last=$((10#$last))
        (( first <= 2147483647 && last <= 2147483647 && first <= last )) ||
            return 1
        if [ -n "${BASH_REMATCH[2]}" ] && (( first == last )); then
            return 1
        fi
        if (( have_previous && first <= previous_last + 1 )); then
            return 1
        fi
        previous_last=$last
        have_previous=1
    done
}

validate_poll_frame_builtin() {
    local frame=$1 body mq_count expected_count index previous=-1 number
    local record st_dev inode cpu extra
    local -a lines=()
    [[ $frame == *$'\n' && $frame != *$'\n\n' &&
       $frame != *$'\r'* ]] || return 1
    body=${frame%$'\n'}
    mapfile -t lines <<<"$body"
    [ "${#lines[@]}" -ge 28 ] || return 1
    [[ ${lines[0]} =~ ^snapshot_sha256=[0-9a-f]{64}$ ]] || return 1
    [ "${lines[1]}" = format=exitos-poll-generation-v1 ] || return 1
    [[ ${lines[2]} =~ ^boot_id=[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]] || return 1
    [[ ${lines[3]} =~ ^poll_queues=[1-9][0-9]{0,9}$ ]] || return 1
    [ "${lines[4]}" = io_poll=1 ] || return 1
    [[ ${lines[5]} =~ ^whole_dev_t=(0|[1-9][0-9]*):(0|[1-9][0-9]*)$ &&
       ${lines[6]} =~ ^part_dev_t=(0|[1-9][0-9]*):(0|[1-9][0-9]*)$ &&
       ${lines[7]} =~ ^generic_dev_t=(0|[1-9][0-9]*):(0|[1-9][0-9]*)$ &&
       ${lines[8]} =~ ^controller_dev_t=(0|[1-9][0-9]*):(0|[1-9][0-9]*)$ ]] || return 1
    [[ ${lines[9]} =~ ^diskseq_fd=(0|[1-9][0-9]{0,19})$ &&
       ${lines[10]} =~ ^diskseq_sysfs=(0|[1-9][0-9]{0,19})$ &&
       ${lines[11]} =~ ^nsid=[1-9][0-9]{0,9}$ ]] || return 1
    [[ ${lines[12]} =~ ^whole_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[13]} =~ ^whole_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[14]} =~ ^part_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[15]} =~ ^part_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[16]} =~ ^generic_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[17]} =~ ^generic_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[18]} =~ ^controller_char_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[19]} =~ ^controller_char_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[20]} =~ ^queue_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[21]} =~ ^queue_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[22]} =~ ^controller_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[23]} =~ ^controller_inode=[0-9]+:[1-9][0-9]*$ &&
       ${lines[24]} =~ ^pci_target=/[A-Za-z0-9_./:+-]+$ &&
       ${lines[25]} =~ ^pci_inode=[0-9]+:[1-9][0-9]*$ ]] || return 1
    [[ ${lines[26]} =~ ^mq_count=([1-9][0-9]{0,4})$ ]] || return 1
    mq_count=$((10#${BASH_REMATCH[1]}))
    expected_count=$((27 + mq_count))
    [ "${#lines[@]}" -eq "$expected_count" ] || return 1
    for ((index = 27; index < expected_count; index++)); do
        [[ ${lines[index]} == mq_hctx=* ]] || return 1
        record=${lines[index]#mq_hctx=}
        [[ $record != *: ]] || return 1
        IFS=: read -r number st_dev inode cpu extra <<<"$record" || return 1
        [[ $number =~ ^(0|[1-9][0-9]*)$ &&
           $st_dev =~ ^(0|[1-9][0-9]*)$ &&
           $inode =~ ^[1-9][0-9]*$ && -z $extra ]] || return 1
        [ "${#number}" -le 10 ] || return 1
        number=$((10#$number))
        (( number <= 2147483647 && number > previous )) || return 1
        validate_canonical_poll_cpu_list "$cpu" || return 1
        previous=$number
    done
}

start_mount_root_holder() {
    local helper tag reported_id current
    [ -z "$MOUNT_HOLDER_PID" ] || die "mount-root holder already started"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        helper=${EXITOS_TEST_MOUNT_FD_HELPER:?pure test requires mount-fd helper}
        [[ $helper == /* && -x $helper ]] || die "invalid pure-test mount-fd helper"
        coproc MOUNT_ROOT_HOLDER {
            close_inherited_device_fds || exit 120
            exec "$helper" "$MNT"
        }
    else
        command -v python3 >/dev/null || die "python3 is required for mount-root O_PATH retention"
        coproc MOUNT_ROOT_HOLDER {
            close_inherited_device_fds || exit 120
            exec python3 -c '
import os
import sys

path = sys.argv[1]
fd = os.open(path, os.O_PATH | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
mnt_ids = []
with open(f"/proc/self/fdinfo/{fd}", "r", encoding="ascii") as stream:
    for line in stream:
        if line.startswith("mnt_id:"):
            fields = line.split()
            if len(fields) == 2 and fields[1].isdigit() and int(fields[1]) > 0:
                mnt_ids.append(fields[1])
if len(mnt_ids) != 1:
    raise SystemExit(90)
print(f"READY\t{os.getpid()}\t{fd}\t{mnt_ids[0]}", flush=True)
request = sys.stdin.readline().rstrip("\n")
if request != "CLOSE":
    raise SystemExit(91)
os.close(fd)
print("CLOSED", flush=True)
' "$MNT"
        }
    fi
    MOUNT_HOLDER_PID=$MOUNT_ROOT_HOLDER_PID
    MOUNT_HOLDER_READ_FD=${MOUNT_ROOT_HOLDER[0]}
    MOUNT_HOLDER_WRITE_FD=${MOUNT_ROOT_HOLDER[1]}
    if ! IFS=$'\t' read -r -t 5 tag MOUNT_ROOT_PID MOUNT_ROOT_FD reported_id \
            <&"$MOUNT_HOLDER_READ_FD"; then
        die "mount-root O_PATH holder did not become ready"
    fi
    [ "$tag" = READY ] && [[ $MOUNT_ROOT_PID =~ ^[1-9][0-9]*$ ]] &&
        [[ $MOUNT_ROOT_FD =~ ^[0-9]+$ ]] && [[ $reported_id =~ ^[1-9][0-9]*$ ]] ||
        die "malformed mount-root O_PATH holder record"
    [ "$MOUNT_ROOT_PID" = "$MOUNT_HOLDER_PID" ] ||
        die "mount-root O_PATH holder is not the direct coprocess"
    MOUNT_ROOT_FD_PATH="/proc/$MOUNT_ROOT_PID/fd/$MOUNT_ROOT_FD"
    MOUNT_ROOT_MNT_ID=$reported_id
    current=$(read_mount_root_mnt_id) || die "cannot read retained mount-root fdinfo"
    [ "$current" = "$reported_id" ] && [ "$current" = "$MOUNT_ID" ] ||
        die "mount-root O_PATH mnt_id does not match acquired mount"
}

stop_mount_root_holder() {
    local response holder_rc=0
    [ -n "$MOUNT_HOLDER_PID" ] || return 0
    verify_mount_root_fd || return 1
    printf 'CLOSE\n' >&"$MOUNT_HOLDER_WRITE_FD" || return 1
    IFS= read -r -t 5 response <&"$MOUNT_HOLDER_READ_FD" || return 1
    [ "$response" = CLOSED ] || return 1
    wait "$MOUNT_HOLDER_PID" || holder_rc=$?
    [ "$holder_rc" -eq 0 ] || return 1
    exec {MOUNT_HOLDER_READ_FD}<&-
    exec {MOUNT_HOLDER_WRITE_FD}>&-
    MOUNT_HOLDER_PID=""
    MOUNT_ROOT_PID=""
    MOUNT_ROOT_FD=""
    MOUNT_ROOT_FD_PATH=""
    MOUNT_ROOT_MNT_ID=""
}

owned_run_dir() {
    local got
    [ -n "$RUN_DIR" ] && [ -d "$RUN_DIR" ] && [ ! -L "$RUN_DIR" ] || return 1
    got=$(stat -Lc '%d:%i:%u' -- "$RUN_DIR") || return 1
    [ "$got" = "$RUN_DIR_ID" ]
}

noise_sentinel_scan() {
    local status_file=$1 error_file=$2 stat_file="$IRQ_PROC_ROOT/stat"
    : "$status_file"
    [ -r "$stat_file" ] || {
        printf 'kind=sentinel-error detail=proc-stat-unreadable\n' >>"$error_file"
        return 1
    }
}

noise_sentinel_loop() {
    local status_file=$1 error_file=$2 ready_file=$3 interval=$4
    trap - EXIT
    trap 'exit 0' INT TERM
    while :; do
        noise_sentinel_scan "$status_file" "$error_file" || true
        if [ ! -e "$ready_file" ]; then
            : >"$ready_file" || exit 1
        fi
        /bin/sleep "$interval"
    done
}

start_cell_noise_sentinel() {
    local attempt status_file="$TMP/noise.events"
    local error_file="$TMP/noise.errors" ready_file="$TMP/noise.ready"
    [ -z "$NOISE_SENTINEL" ] || die "cell noise sentinel already running"
    : >"$status_file"
    : >"$error_file"
    rm -f -- "$ready_file"
    CELL_NOISE_DETECTED=0
    CELL_NOISE_REASON=""
    (
        close_inherited_device_fds || exit 120
        noise_sentinel_loop "$status_file" "$error_file" "$ready_file" \
            "$NOISE_SENTINEL_INTERVAL"
    ) &
    NOISE_SENTINEL=$!
    for ((attempt = 0; attempt < 200; attempt++)); do
        [ -e "$ready_file" ] && break
        kill -0 "$NOISE_SENTINEL" 2>/dev/null || break
        /bin/sleep 0.01
    done
    if [ ! -e "$ready_file" ]; then
        kill -TERM "$NOISE_SENTINEL" 2>/dev/null || true
        wait "$NOISE_SENTINEL" 2>/dev/null || true
        NOISE_SENTINEL=""
        die "cell noise sentinel did not become ready"
    fi
    if [ "$NOISE_SENTINEL_TRACE" = 1 ]; then
        printf 'TEST_SENTINEL_START %s\n' "$NOISE_SENTINEL"
    fi
}

stop_cell_noise_sentinel() {
    local sentinel_rc=0 status_file="$TMP/noise.events"
    local error_file="$TMP/noise.errors" sentinel_pid
    [ -n "$NOISE_SENTINEL" ] || return 0
    sentinel_pid=$NOISE_SENTINEL
    noise_sentinel_scan "$status_file" "$error_file" || true
    kill -TERM "$sentinel_pid" 2>/dev/null || true
    wait "$sentinel_pid" || sentinel_rc=$?
    NOISE_SENTINEL=""
    if [ "$NOISE_SENTINEL_TRACE" = 1 ]; then
        printf 'TEST_SENTINEL_STOP %s\n' "$sentinel_pid"
    fi
    if [ "$sentinel_rc" -ne 0 ]; then
        printf 'kind=sentinel-error detail=watcher-exit-%s\n' \
            "$sentinel_rc" >>"$error_file"
    fi
    if [ -s "$error_file" ]; then
        CELL_NOISE_DETECTED=1
        IFS= read -r CELL_NOISE_REASON <"$error_file" ||
            CELL_NOISE_REASON='kind=sentinel-error detail=unreadable-error-record'
    elif [ -s "$status_file" ]; then
        CELL_NOISE_DETECTED=1
        IFS= read -r CELL_NOISE_REASON <"$status_file" ||
            CELL_NOISE_REASON='kind=sentinel-error detail=unreadable-noise-record'
    fi
}

launch_qdsplit() {
    local output=$1
    [ -z "$APP" ] || die "qdsplit child already running"
    (
        close_inherited_device_fds || exit 120
        exec nice -n -20 taskset -c "$CPU" "$QDSPLIT" "$ITERS"
    ) >"$output" 2>&1 &
    APP=$!
}

launch_keeper() {
    local keeper_cpu=$1 output=$2
    [ -z "$KEEPER" ] || die "keeper child already running"
    (
        close_inherited_device_fds || exit 120
        exec "$KEEPER_BIN" "$keeper_cpu"
    ) >"$output" 2>&1 &
    KEEPER=$!
}

read_cpu_ticks() {
    local source=$1 cpu=$2 total_var=$3 idle_var=$4
    local -a fields=()
    local line found=0 i value total=0 idle=0
    [ -r "$source" ] || return 1
    while IFS= read -r line; do
        read -r -a fields <<<"$line"
        [ "${fields[0]:-}" = "cpu$cpu" ] || continue
        found=$((found + 1))
        [ "${#fields[@]}" -ge 9 ] || return 1
        total=0
        for ((i = 1; i <= 8; i++)); do
            value=${fields[$i]}
            [[ $value =~ ^(0|[1-9][0-9]*)$ ]] && [ "${#value}" -le 18 ] ||
                return 1
            total=$((total + 10#$value))
        done
        idle=$((10#${fields[4]} + 10#${fields[5]}))
    done <"$source"
    [ "$found" -eq 1 ] || return 1
    printf -v "$total_var" '%s' "$total"
    printf -v "$idle_var" '%s' "$idle"
}

core_util_source() {
    local index=$1 outvar=$2 path
    if [ -n "$CORE_UTIL_SEQUENCE_DIR" ]; then
        path="$CORE_UTIL_SEQUENCE_DIR/stat.$index"
    else
        path="$IRQ_PROC_ROOT/stat"
    fi
    [ -r "$path" ] || return 1
    printf -v "$outvar" '%s' "$path"
}

verify_target_core_utilization() {
    local phase=$1 source window
    local selected_total selected_idle sibling_total sibling_idle
    local next_selected_total next_selected_idle next_sibling_total next_sibling_idle
    local selected_delta selected_idle_delta selected_busy sibling_delta
    local sibling_idle_delta sibling_busy selected_bp sibling_bp
    local selected_max_bp=0 sibling_max_bp=0
    local threshold_exceeded=0 threshold_exceeded_token=no

    core_util_source 0 source &&
    read_cpu_ticks "$source" "$CPU" selected_total selected_idle &&
    read_cpu_ticks "$source" "$SIBLING" sibling_total sibling_idle || {
        echo "cstate campaign: target-core utilization snapshot is unreadable during $phase" >&2
        return 1
    }
    for ((window = 1; window <= CORE_UTIL_WINDOWS; window++)); do
        if [ -z "$CORE_UTIL_SEQUENCE_DIR" ]; then /bin/sleep 1; fi
        core_util_source "$window" source &&
        read_cpu_ticks "$source" "$CPU" next_selected_total next_selected_idle &&
        read_cpu_ticks "$source" "$SIBLING" next_sibling_total next_sibling_idle || {
            echo "cstate campaign: target-core utilization snapshot is unreadable during $phase" >&2
            return 1
        }
        selected_delta=$((next_selected_total - selected_total))
        selected_idle_delta=$((next_selected_idle - selected_idle))
        sibling_delta=$((next_sibling_total - sibling_total))
        sibling_idle_delta=$((next_sibling_idle - sibling_idle))
        (( selected_delta > 0 && selected_idle_delta >= 0 &&
           selected_idle_delta <= selected_delta && sibling_delta > 0 &&
           sibling_idle_delta >= 0 && sibling_idle_delta <= sibling_delta )) || {
            echo "cstate campaign: target-core utilization counters are nonmonotonic during $phase" >&2
            return 1
        }
        selected_busy=$((selected_delta - selected_idle_delta))
        sibling_busy=$((sibling_delta - sibling_idle_delta))
        selected_bp=$(((selected_busy * 10000 + selected_delta - 1) / selected_delta))
        sibling_bp=$(((sibling_busy * 10000 + sibling_delta - 1) / sibling_delta))
        (( selected_bp > selected_max_bp )) && selected_max_bp=$selected_bp
        (( sibling_bp > sibling_max_bp )) && sibling_max_bp=$sibling_bp
        if (( selected_busy * 100 >= selected_delta * CORE_UTIL_THRESHOLD_PCT ||
              sibling_busy * 100 >= sibling_delta * CORE_UTIL_THRESHOLD_PCT )); then
            threshold_exceeded=1
        fi
        selected_total=$next_selected_total
        selected_idle=$next_selected_idle
        sibling_total=$next_sibling_total
        sibling_idle=$next_sibling_idle
    done
    if [ "$threshold_exceeded" -eq 1 ]; then
        threshold_exceeded_token=yes
        printf 'cstate campaign: target-core utilization threshold exceeded (record-only) during %s: selected_cpu=%s selected_max_bp=%s sibling_cpu=%s sibling_max_bp=%s threshold_lt_pct=%s\n' \
            "$phase" "$CPU" "$selected_max_bp" "$SIBLING" "$sibling_max_bp" \
            "$CORE_UTIL_THRESHOLD_PCT" >&2
    fi
    CORE_UTIL_TOKEN="selected_cpu=$CPU@selected_max_bp=$selected_max_bp@sibling_cpu=$SIBLING@sibling_max_bp=$sibling_max_bp@windows=$CORE_UTIL_WINDOWS@threshold_lt_pct=$CORE_UTIL_THRESHOLD_PCT@threshold_exceeded=$threshold_exceeded_token"
    printf 'CORE_UTIL_READY phase=%s selected_cpu=%s selected_max_bp=%s sibling_cpu=%s sibling_max_bp=%s windows=%s threshold_lt_pct=%s threshold_exceeded=%s status=recorded\n' \
        "$phase" "$CPU" "$selected_max_bp" "$SIBLING" "$sibling_max_bp" \
        "$CORE_UTIL_WINDOWS" "$CORE_UTIL_THRESHOLD_PCT" "$threshold_exceeded_token"
}

same_identity() {
    local path=$1 expected=$2 got
    [ -n "$path" ] && [ -e "$path" ] && [ ! -L "$path" ] || return 1
    got=$(stat -Lc '%d:%i:%u' -- "$path") || return 1
    [ "$got" = "$expected" ]
}

validate_mount_path() {
    [[ $MNT == /* && $MNT != / && $MNT != */ &&
       $MNT != *[[:space:]\\\"]* ]] ||
        die "mount path must be a simple non-root absolute directory without a trailing slash"
    case "/${MNT#/}/" in
      */../*|*/./*) die "mount path contains a dot component" ;;
    esac
}

prepare_mount_dir() {
    validate_mount_path
    [ -d "$MNT" ] && [ ! -L "$MNT" ] ||
        die "mount directory must already exist as a real directory: $MNT"
    MNT_BASE_ID=$(stat -Lc '%d:%i:%u' -- "$MNT") ||
        die "cannot retain mount-directory identity"
}

cleanup_error() {
    echo "cstate campaign: cleanup failed: $*" >&2
    CLEANUP_STATUS=1
}

cleanup_resources() {
    [ "$CLEANUP_DONE" = 0 ] || return "$CLEANUP_STATUS"
    [ -n "$KEEPER" ] && kill -TERM "$KEEPER" 2>/dev/null || true
    [ -n "$APP" ] && kill -TERM "$APP" 2>/dev/null || true
    [ -n "$KEEPER" ] && wait "$KEEPER" 2>/dev/null || true
    [ -n "$APP" ] && wait "$APP" 2>/dev/null || true
    if [ -n "$NOISE_SENTINEL" ]; then
        stop_cell_noise_sentinel ||
            cleanup_error "cannot reap cell noise sentinel"
    fi
    if owned_run_dir; then
        # qdsplit uses O_TMPFILE, so a successful or killed child leaves no
        # named file.  Refuse recursive/pathname cleanup; an unexpected entry
        # deliberately makes rmdir fail and preserves evidence.
        rmdir -- "$RUN_DIR" 2>/dev/null || true
    fi
    if [ -n "$TMP" ] && [ -d "$TMP" ] && [ ! -L "$TMP" ]; then
        if [ -s "$TMP/noise.events" ] || [ -s "$TMP/noise.errors" ]; then
            PRESERVE_APP_OUTPUT=1
        fi
        if [ "$PRESERVE_APP_OUTPUT" = 1 ]; then
            if [ "$APP_OUTPUT_PRESERVE_REPORTED" = 0 ]; then
                echo "cstate campaign: noisy cell app output preserved: $TMP/app.out" >&2
                APP_OUTPUT_PRESERVE_REPORTED=1
            fi
        else
            rm -f -- "$TMP/app.out"
        fi
        rm -f -- "$TMP/keeper.out" "$TMP/schedule" \
            "$TMP/noise.events" "$TMP/noise.errors" "$TMP/noise.ready"
        rmdir -- "$TMP" 2>/dev/null || true
    fi
    if [ "$OUTPUT_OPEN" = 1 ]; then exec 3>&-; fi
    if [ "$MOUNT_PENDING" = 1 ]; then
        cleanup_error "mount completed without a retained mount identity"
    elif [ -n "$MOUNT_ID" ]; then
        stop_mount_root_holder ||
            cleanup_error "cannot close verified mount-root O_PATH holder"
    fi
    if [ -n "$CHAR_FD" ]; then exec {CHAR_FD}<&- || cleanup_error "cannot close char fd"; CHAR_FD=""; fi
    if [ -n "$CTRL_FD" ]; then exec {CTRL_FD}<&- || cleanup_error "cannot close controller fd"; CTRL_FD=""; fi
    if [ -n "$WHOLE_FD" ]; then exec {WHOLE_FD}<&- || cleanup_error "cannot close whole fd"; WHOLE_FD=""; fi
    if [ -n "$PART_FD" ]; then exec {PART_FD}<&- || cleanup_error "cannot close partition fd"; PART_FD=""; fi
    if [ -n "$POLL_STATE_DIR" ]; then
        if [ -d "$POLL_STATE_DIR" ] && [ ! -L "$POLL_STATE_DIR" ] &&
           [ "$(stat -Lc '%d:%i:%u:%a' -- "$POLL_STATE_DIR" 2>/dev/null)" = "$POLL_STATE_ID" ]; then
            rm -f -- "$POLL_CAPTURE_FILE" || cleanup_error "cannot remove poll snapshot state file"
            rmdir -- "$POLL_STATE_DIR" 2>/dev/null ||
                cleanup_error "cannot remove poll snapshot state directory"
        else
            cleanup_error "poll snapshot state directory identity changed"
        fi
        POLL_STATE_DIR=""
        POLL_CAPTURE_FILE=""
    fi
    CLEANUP_DONE=1
    return "$CLEANUP_STATUS"
}

cleanup_fallback() {
    local rc=$? cleanup_rc=0
    trap - EXIT
    set +e
    cleanup_resources
    cleanup_rc=$?
    if [ "$rc" -eq 0 ] && [ "$cleanup_rc" -ne 0 ]; then rc=1; fi
    exit "$rc"
}
trap cleanup_fallback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

prepare_output() {
    local result_root old_umask attempt opened=0
    open_output_fd() {
        old_umask=$(umask)
        umask 077
        set -C
        if { exec 3>"$OUT"; } 2>/dev/null; then opened=1; else opened=0; fi
        set +C
        umask "$old_umask"
        [ "$opened" = 1 ]
    }
    if [ "$OUT_WAS_SET" = x ]; then
        [ -n "$OUT" ] || die "explicit OUT must not be empty"
        open_output_fd || die "cannot exclusively create OUT: $OUT"
    else
        result_root=${TMPDIR:-/tmp}
        [ -d "$result_root" ] || die "TMPDIR is not a directory: $result_root"
        # Predictability is harmless here: noclobber makes every attempt an
        # atomic O_EXCL-style create and fd 3 stays open for the whole run.
        # An attacker can at most occupy candidates and make us choose another.
        for ((attempt = 0; attempt < 128; attempt++)); do
            OUT="$result_root/exitos-cstate-keeper.$$.$RANDOM.$attempt.tsv"
            if open_output_fd; then break; fi
        done
        [ "$opened" = 1 ] || die "cannot exclusively create result file"
    fi
    [ -f "/proc/$$/fd/3" ] || die "OUT descriptor is not a regular file"
    OUTPUT_ID=$(stat -Lc '%d:%i:%u' -- "/proc/$$/fd/3") ||
        die "cannot retain OUT descriptor identity"
    OUTPUT_OPEN=1
}

prepare_run_dir() {
    local owner mode
    [ -d "$MNT" ] && [ ! -L "$MNT" ] || die "mount directory is not safe: $MNT"
    RUN_DIR=$(mktemp -d "$MNT/.exitos-cstate.XXXXXX") ||
        die "cannot create owned work directory in $MNT"
    chmod 0700 "$RUN_DIR"
    owner=$(stat -Lc '%u' -- "$RUN_DIR") || die "cannot stat owned work directory"
    mode=$(stat -Lc '%a' -- "$RUN_DIR") || die "cannot stat owned work directory"
    [ "$owner" = "$(id -u)" ] && [ "$mode" = 700 ] ||
        die "owned work directory identity/mode mismatch"
    RUN_DIR_ID=$(stat -Lc '%d:%i:%u' -- "$RUN_DIR") ||
        die "cannot retain owned work directory identity"
}

configure_buffer_env() {
    local buffer=$1
    unset EXITOS_ONLY
    case "$buffer" in
      normal)
        unset EXITOS_HUGEBUF EXITOS_REQUIRE_CONTIG || true
        export EXITOS_REQUIRE_SCATTERED=1
        ;;
      contiguous)
        unset EXITOS_REQUIRE_SCATTERED || true
        export EXITOS_HUGEBUF=1 EXITOS_REQUIRE_CONTIG=1
        ;;
      *) die "unknown buffer contract: $buffer" ;;
    esac
}

buffer_layout_from_output() {
    local input=$1 line candidates=0 count=0 bytes="" runs=""
    [ -r "$input" ] || {
        echo "cstate campaign: cannot read qdsplit output: $input" >&2
        return 1
    }
    while IFS= read -r line; do
        if [[ $line == 'buffer pages:'* ]]; then
            ((candidates += 1))
            if [[ $line =~ ^buffer[[:space:]]pages:[[:space:]][0-9]+([[:space:]][0-9]+)*[[:space:]]\.\.\.[[:space:]]submitted[[:space:]]([1-9][0-9]*)-byte[[:space:]]command[[:space:]]span[[:space:]]has[[:space:]]([1-9][0-9]*)[[:space:]]physical[[:space:]]runs?$ ]]; then
                ((count += 1))
                bytes=${BASH_REMATCH[2]}
                runs=${BASH_REMATCH[3]}
            fi
        fi
    done <"$input"
    if [ "$candidates" -ne 1 ] || [ "$count" -ne 1 ]; then
        echo "cstate campaign: expected one exact submitted-span PFN proof; found $candidates candidate lines, $count exact" >&2
        return 1
    fi
    printf 'bytes=%s,runs=%s\n' "$bytes" "$runs"
}

validate_buffer_layout() {
    local buffer=$1 layout=$2
    case "$buffer" in
      normal)
        [[ $layout =~ ^bytes=8192,runs=([2-9]|[1-9][0-9]+)$ ]]
        ;;
      contiguous)
        [ "$layout" = 'bytes=8192,runs=1' ]
        ;;
      *) return 1 ;;
    esac
}

strict_opt_median_from_output() {
    local input=$1 arm=$2 outvar=$3 line candidate_count=0 exact_count=0 value=""
    local -a fields=()
    [ -r "$input" ] || return 1
    while IFS= read -r line; do
        read -r -a fields <<<"$line"
        [ "${fields[0]-}" = "$arm" ] || continue
        candidate_count=$((candidate_count + 1))
        [ "${#fields[@]}" -eq 5 ] || continue
        [ "${fields[1]}" = "$ITERS" ] || continue
        if [[ ${fields[2]} =~ ^(0|[1-9][0-9]*)(\.[0-9]+)?$ &&
              ${fields[3]} =~ ^(0|[1-9][0-9]*)(\.[0-9]+)?$ &&
              ${fields[4]} =~ ^(0|[1-9][0-9]*)(\.[0-9]+)?$ &&
              ${#fields[2]} -le 32 && ${#fields[3]} -le 32 &&
              ${#fields[4]} -le 32 ]]; then
            exact_count=$((exact_count + 1))
            value=${fields[2]}
        fi
    done <"$input"
    if [ "$candidate_count" -ne 1 ] || [ "$exact_count" -ne 1 ]; then
        echo "cstate campaign: expected one exact $arm timing row; found $candidate_count candidate lines, $exact_count exact" >&2
        return 1
    fi
    printf -v "$outvar" '%s' "$value"
}

verify_opt_post_run_record() {
    local input=$1 line candidate_count=0 exact_count=0
    local expected="POST_RUN_VERIFY file=exact raw=exact selected=$((ITERS * 5)) guards=zero"
    [ -r "$input" ] || return 1
    while IFS= read -r line; do
        [[ $line == POST_RUN_VERIFY* ]] || continue
        candidate_count=$((candidate_count + 1))
        [ "$line" = "$expected" ] && exact_count=$((exact_count + 1))
    done <"$input"
    if [ "$candidate_count" -ne 1 ] || [ "$exact_count" -ne 1 ]; then
        echo "cstate campaign: expected one exact opt POST_RUN_VERIFY record; found $candidate_count candidate lines, $exact_count exact" >&2
        return 1
    fi
}

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
        [[ -z ${seen_field[$key]+x} ]] || die "duplicate test-disk resolver field: $key"
        seen_field[$key]=1
        case "$key" in
          EXITOS_DEV|EXITOS_TESTPART|EXITOS_CHARDEV)
            [[ $value =~ ^/dev/[A-Za-z0-9._+-]+$ ]] ||
                die "$key is not a fixed /dev path"
            ;;
          EXITOS_FSPART|EXITOS_WINDOW_IN_PART)
            if [[ $value =~ ^[A-Za-z0-9._+-]+$ ]]; then value="/dev/$value"; fi
            [[ $value =~ ^/dev/[A-Za-z0-9._+-]+$ ]] ||
                die "$key is not a fixed partition path"
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

retained_fd_is_read_only() {
    local fd=$1 line key value extra flags="" count=0 octal
    [[ $fd =~ ^[0-9]+$ ]] || return 1
    while IFS= read -r line; do
        read -r key value extra <<<"$line"
        [ "$key" = flags: ] || continue
        [ -z "${extra:-}" ] || return 1
        flags=$value
        count=$((count + 1))
    done <"/proc/$$/fdinfo/$fd" || return 1
    [ "$count" -eq 1 ] && [[ $flags =~ ^0[0-7]+$ ]] || return 1
    octal=${flags#0}
    [ -n "$octal" ] || octal=0
    (( (8#$octal & 3) == 0 ))
}

retain_partition() {
    local current major_hex minor_hex
    [ ! -L "$EXITOS_TESTPART" ] || die "resolved partition path is a symlink"
    exec {PART_FD}<"$EXITOS_TESTPART" ||
        die "cannot retain serial-resolved partition: $EXITOS_TESTPART"
    PART_FD_PATH="/proc/$$/fd/$PART_FD"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$PART_FD_PATH" ] || die "pure-test partition is not a regular file"
        PART_OBJECT_ID=$(stat -Lc '%d:%i' -- "$PART_FD_PATH") ||
            die "cannot retain pure-test partition identity"
        current=$(stat -Lc '%d:%i' -- "$EXITOS_TESTPART") ||
            die "partition pathname disappeared after retention"
        PART_MAJMIN=${EXITOS_TEST_PART_MAJMIN:?pure test requires partition MAJ:MIN}
        [[ $PART_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] ||
            die "invalid pure-test partition MAJ:MIN"
    else
        [ -b "$PART_FD_PATH" ] || die "serial-resolved partition is not a block device"
        PART_RDEV_HEX=$(stat -Lc '%t:%T' -- "$PART_FD_PATH") ||
            die "cannot read retained partition dev_t"
        PART_OBJECT_ID=$PART_RDEV_HEX
        current=$(stat -Lc '%t:%T' -- "$EXITOS_TESTPART") ||
            die "partition pathname disappeared after retention"
        IFS=: read -r major_hex minor_hex <<<"$PART_RDEV_HEX"
        [[ $major_hex =~ ^[0-9a-fA-F]+$ && $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
            die "invalid retained partition dev_t"
        PART_MAJMIN="$((16#$major_hex)):$((16#$minor_hex))"
    fi
    [ "$current" = "$PART_OBJECT_ID" ] ||
        die "partition pathname changed during retention"
}

controller_from_namespace_path() {
    local outvar=$1 namespace_path=$2 component candidate=""
    local -a components=()
    IFS=/ read -r -a components <<<"$namespace_path"
    for component in "${components[@]}"; do
        if [[ $component =~ ^nvme[0-9]+$ ]]; then candidate=$component; fi
    done
    [ -n "$candidate" ] || return 1
    printf -v "$outvar" '%s' "$candidate"
}

retained_char_nsid() {
    local outvar=$1 raw parsed_nsid
    raw=$("$NVME_BIN" get-ns-id "$CHAR_FD_PATH" 2>/dev/null) || return 1
    [[ $raw != *$'\n'* && $raw != *$'\r'* &&
       $raw =~ ^.+:[[:space:]]namespace-id:([1-9][0-9]*)$ ]] || return 1
    parsed_nsid=${BASH_REMATCH[1]}
    (( ${#parsed_nsid} <= 10 && 10#$parsed_nsid <= 4294967295 )) || return 1
    printf -v "$outvar" '%u' "$((10#$parsed_nsid))"
}

retain_whole_device() {
    local current major_hex minor_hex sys_dev serial nsid controller diskseq
    [ ! -L "$EXITOS_DEV" ] || die "resolved whole-device path is a symlink"
    exec {WHOLE_FD}<"$EXITOS_DEV" ||
        die "cannot retain serial-resolved whole device: $EXITOS_DEV"
    WHOLE_FD_PATH="/proc/$$/fd/$WHOLE_FD"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$WHOLE_FD_PATH" ] || die "pure-test whole device is not a regular file"
        WHOLE_OBJECT_ID=$(stat -Lc '%d:%i' -- "$WHOLE_FD_PATH") ||
            die "cannot retain pure-test whole identity"
        current=$(stat -Lc '%d:%i' -- "$EXITOS_DEV") ||
            die "whole-device pathname disappeared after retention"
        WHOLE_MAJMIN=${EXITOS_TEST_WHOLE_MAJMIN:?pure test requires whole MAJ:MIN}
        [[ $WHOLE_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] ||
            die "invalid pure-test whole MAJ:MIN"
    else
        [ -b "$WHOLE_FD_PATH" ] || die "serial-resolved whole device is not a block device"
        WHOLE_RDEV_HEX=$(stat -Lc '%t:%T' -- "$WHOLE_FD_PATH") ||
            die "cannot read retained whole-device dev_t"
        WHOLE_OBJECT_ID=$WHOLE_RDEV_HEX
        current=$(stat -Lc '%t:%T' -- "$EXITOS_DEV") ||
            die "whole-device pathname disappeared after retention"
        IFS=: read -r major_hex minor_hex <<<"$WHOLE_RDEV_HEX"
        [[ $major_hex =~ ^[0-9a-fA-F]+$ && $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
            die "invalid retained whole-device dev_t"
        WHOLE_MAJMIN="$((16#$major_hex)):$((16#$minor_hex))"
    fi
    [ "$current" = "$WHOLE_OBJECT_ID" ] ||
        die "whole-device pathname changed during retention"

    WHOLE_SYSFS_PATH=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN") ||
        die "cannot resolve retained whole-device dev_t in sysfs"
    PART_SYSFS_PATH=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/block/$PART_MAJMIN") ||
        die "cannot resolve retained partition dev_t in sysfs"
    [ -d "$WHOLE_SYSFS_PATH" ] && [ -d "$PART_SYSFS_PATH" ] ||
        die "retained block-device sysfs identity is not a directory"
    read -r sys_dev <"$WHOLE_SYSFS_PATH/dev" || die "cannot read whole sysfs dev_t"
    [ "$sys_dev" = "$WHOLE_MAJMIN" ] || die "whole sysfs dev_t mismatch"
    read -r sys_dev <"$PART_SYSFS_PATH/dev" || die "cannot read partition sysfs dev_t"
    [ "$sys_dev" = "$PART_MAJMIN" ] || die "partition sysfs dev_t mismatch"
    read -r nsid <"$WHOLE_SYSFS_PATH/nsid" || die "cannot read namespace NSID"
    [[ $nsid =~ ^[1-9][0-9]*$ ]] || die "namespace NSID is malformed"
    EXPECTED_NSID=$nsid
    read -r diskseq <"$WHOLE_SYSFS_PATH/diskseq" ||
        die "cannot read namespace diskseq"
    [[ $diskseq =~ ^(0|[1-9][0-9]*)$ && ${#diskseq} -le 20 ]] ||
        die "namespace diskseq is malformed"
    WHOLE_DISKSEQ=$diskseq
    WHOLE_CONTROLLER_PATH=$(readlink -f -- "$WHOLE_SYSFS_PATH/device") ||
        die "cannot resolve retained namespace controller"
    [ -d "$WHOLE_CONTROLLER_PATH" ] ||
        die "retained namespace controller is not a sysfs directory"
    WHOLE_CONTROLLER_ID=$(stat -Lc '%d:%i' -- "$WHOLE_CONTROLLER_PATH") ||
        die "cannot retain namespace controller identity"
    WHOLE_PCI_PATH=$(readlink -f -- "$WHOLE_CONTROLLER_PATH/device") ||
        die "cannot resolve retained controller PCI function"
    [ -d "$WHOLE_PCI_PATH" ] ||
        die "retained controller PCI function is not a sysfs directory"
    WHOLE_PCI_ID=$(stat -Lc '%d:%i' -- "$WHOLE_PCI_PATH") ||
        die "cannot retain controller PCI identity"
    [ -d "$WHOLE_PCI_PATH/msi_irqs" ] ||
        die "retained controller has no MSI IRQ directory"
    serial=$(tr -d ' ' <"$WHOLE_CONTROLLER_PATH/serial") ||
        die "cannot read whole-device serial from sysfs"
    [ "$serial" = "$EXITOS_EXPECT_SERIAL" ] ||
        die "whole-device sysfs serial mismatch"
    read -r sys_dev <"$PART_SYSFS_PATH/partition" ||
        die "retained filesystem device is not a partition"
    [ "$sys_dev" = 1 ] || die "retained filesystem partition is not p1"
    [ "${PART_SYSFS_PATH%/*}" = "$WHOLE_SYSFS_PATH" ] ||
        die "retained p1 is not a direct child of retained whole device"
    controller_from_namespace_path controller "$WHOLE_SYSFS_PATH" ||
        die "cannot derive NVMe controller from retained namespace"
    NVME_CTRL=$controller
}

retain_controller_device() {
    local current current_path_id major_hex minor_hex sys_dev expected_sysfs
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        CTRL_DEVICE_PATH=${EXITOS_TEST_CTRLDEV:?pure test requires controller device path}
        [[ $CTRL_DEVICE_PATH == /* && $CTRL_DEVICE_PATH != *$'\n'* ]] ||
            die "invalid pure-test controller device path"
    else
        [ -z "${EXITOS_TEST_CTRLDEV+x}" ] &&
        [ -z "${EXITOS_TEST_CTRL_MAJMIN+x}" ] &&
        [ -z "${EXITOS_TEST_CTRL_SYSFS_PATH+x}" ] ||
            die "controller identity overrides are pure-test-only"
        [[ $NVME_CTRL =~ ^nvme[0-9]+$ ]] ||
            die "retained controller name is malformed"
        CTRL_DEVICE_PATH="/dev/$NVME_CTRL"
    fi
    [ ! -L "$CTRL_DEVICE_PATH" ] ||
        die "resolved controller-device path is a symlink"
    exec {CTRL_FD}<"$CTRL_DEVICE_PATH" ||
        die "cannot retain derived controller device: $CTRL_DEVICE_PATH"
    CTRL_FD_PATH="/proc/$$/fd/$CTRL_FD"
    CTRL_PATH_ID=$(stat -Lc '%d:%i' -- "$CTRL_FD_PATH") ||
        die "cannot retain controller pathname identity"
    current_path_id=$(stat -Lc '%d:%i' -- "$CTRL_DEVICE_PATH") ||
        die "controller pathname disappeared after retention"
    [ "$current_path_id" = "$CTRL_PATH_ID" ] ||
        die "controller pathname changed during retention"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$CTRL_FD_PATH" ] ||
            die "pure-test controller device is not a regular file"
        CTRL_OBJECT_ID=$CTRL_PATH_ID
        current=$current_path_id
        CTRL_MAJMIN=${EXITOS_TEST_CTRL_MAJMIN:?pure test requires controller MAJ:MIN}
        [[ $CTRL_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] ||
            die "invalid pure-test controller MAJ:MIN"
    else
        [ -c "$CTRL_FD_PATH" ] ||
            die "derived controller device is not a character device"
        CTRL_RDEV_HEX=$(stat -Lc '%t:%T' -- "$CTRL_FD_PATH") ||
            die "cannot read retained controller dev_t"
        CTRL_OBJECT_ID=$CTRL_RDEV_HEX
        current=$(stat -Lc '%t:%T' -- "$CTRL_DEVICE_PATH") ||
            die "controller pathname disappeared after retention"
        IFS=: read -r major_hex minor_hex <<<"$CTRL_RDEV_HEX"
        [[ $major_hex =~ ^[0-9a-fA-F]+$ &&
           $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
            die "invalid retained controller dev_t"
        CTRL_MAJMIN="$((16#$major_hex)):$((16#$minor_hex))"
    fi
    [ "$current" = "$CTRL_OBJECT_ID" ] ||
        die "controller pathname changed during retention"
    retained_fd_is_read_only "$CTRL_FD" ||
        die "retained controller fd is not read-only"

    CTRL_SYSFS_PATH=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/char/$CTRL_MAJMIN") ||
        die "cannot resolve retained controller dev_t in sysfs"
    [ -d "$CTRL_SYSFS_PATH" ] ||
        die "retained controller sysfs identity is not a directory"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        expected_sysfs=${EXITOS_TEST_CTRL_SYSFS_PATH:?pure test requires controller sysfs path}
        [[ $expected_sysfs == /* && $expected_sysfs != *$'\n'* &&
           -d $expected_sysfs && ! -L $expected_sysfs ]] ||
            die "invalid pure-test controller sysfs path"
        expected_sysfs=$(readlink -f -- "$expected_sysfs") ||
            die "cannot resolve pure-test controller sysfs path"
        [ "$CTRL_SYSFS_PATH" = "$expected_sysfs" ] ||
            die "controller dev_t resolved to the wrong sysfs target"
    fi
    read -r sys_dev <"$CTRL_SYSFS_PATH/dev" ||
        die "cannot read controller sysfs dev_t"
    [ "$sys_dev" = "$CTRL_MAJMIN" ] ||
        die "controller sysfs dev_t mismatch"
    [ "$CTRL_SYSFS_PATH" = "$WHOLE_CONTROLLER_PATH" ] ||
        die "controller char device belongs to a different controller"
    CTRL_SYSFS_ID=$(stat -Lc '%d:%i' -- "$CTRL_SYSFS_PATH") ||
        die "cannot retain controller sysfs identity"
    [ "$CTRL_SYSFS_ID" = "$WHOLE_CONTROLLER_ID" ] ||
        die "controller char sysfs inode differs from retained namespace controller"
}

retain_char_device() {
    local current major_hex minor_hex sys_dev controller char_nsid
    [ ! -L "$EXITOS_CHARDEV" ] || die "resolved character-device path is a symlink"
    exec {CHAR_FD}<"$EXITOS_CHARDEV" ||
        die "cannot retain serial-resolved character device: $EXITOS_CHARDEV"
    CHAR_FD_PATH="/proc/$$/fd/$CHAR_FD"
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$CHAR_FD_PATH" ] || die "pure-test character device is not a regular file"
        CHAR_OBJECT_ID=$(stat -Lc '%d:%i' -- "$CHAR_FD_PATH") ||
            die "cannot retain pure-test character identity"
        current=$(stat -Lc '%d:%i' -- "$EXITOS_CHARDEV") ||
            die "character-device pathname disappeared after retention"
        CHAR_MAJMIN=${EXITOS_TEST_CHAR_MAJMIN:?pure test requires char MAJ:MIN}
        [[ $CHAR_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] ||
            die "invalid pure-test char MAJ:MIN"
    else
        [ -c "$CHAR_FD_PATH" ] || die "serial-resolved character device is not a character device"
        CHAR_RDEV_HEX=$(stat -Lc '%t:%T' -- "$CHAR_FD_PATH") ||
            die "cannot read retained character-device dev_t"
        CHAR_OBJECT_ID=$CHAR_RDEV_HEX
        current=$(stat -Lc '%t:%T' -- "$EXITOS_CHARDEV") ||
            die "character-device pathname disappeared after retention"
        IFS=: read -r major_hex minor_hex <<<"$CHAR_RDEV_HEX"
        [[ $major_hex =~ ^[0-9a-fA-F]+$ && $minor_hex =~ ^[0-9a-fA-F]+$ ]] ||
            die "invalid retained character-device dev_t"
        CHAR_MAJMIN="$((16#$major_hex)):$((16#$minor_hex))"
    fi
    [ "$current" = "$CHAR_OBJECT_ID" ] ||
        die "character-device pathname changed during retention"

    CHAR_SYSFS_PATH=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/char/$CHAR_MAJMIN") ||
        die "cannot resolve retained character-device dev_t in sysfs"
    [ -d "$CHAR_SYSFS_PATH" ] || die "retained character sysfs identity is not a directory"
    read -r sys_dev <"$CHAR_SYSFS_PATH/dev" || die "cannot read character sysfs dev_t"
    [ "$sys_dev" = "$CHAR_MAJMIN" ] || die "character sysfs dev_t mismatch"
    CHAR_CONTROLLER_PATH=$(readlink -f -- "$CHAR_SYSFS_PATH/device") ||
        die "cannot resolve retained character controller"
    [ -d "$CHAR_CONTROLLER_PATH" ] ||
        die "retained character controller is not a sysfs directory"
    [ "$CHAR_CONTROLLER_PATH" = "$WHOLE_CONTROLLER_PATH" ] ||
        die "retained character device belongs to a different controller"
    controller_from_namespace_path controller "$CHAR_CONTROLLER_PATH" ||
        die "cannot derive controller from retained character device"
    [ "$controller" = "$NVME_CTRL" ] ||
        die "retained character device belongs to a different controller"
    retained_char_nsid char_nsid ||
        die "cannot query NSID from retained character fd"
    [ "$char_nsid" = "$EXPECTED_NSID" ] ||
        die "retained character fd addresses NSID $char_nsid, expected $EXPECTED_NSID"
}

mount_test_partition() {
    local record current_id current_majmin current_source current_target record_rc=0
    if [ "$DEVICE_TEST_MODE" != 1 ] && [ "$PRIVATE_MOUNT_NAMESPACE" != 1 ]; then
        die "refusing mount acquisition outside the owned private namespace"
    fi
    same_identity "$MNT" "$MNT_BASE_ID" ||
        die "mount directory identity changed before mount acquisition"
    record=$(current_mount_record) || record_rc=$?
    if [ "$record_rc" -eq 0 ]; then
        IFS=$'\t' read -r current_id current_majmin current_source current_target <<<"$record"
        MOUNT_ID=$current_id
        MOUNT_MAJMIN=$current_majmin
        MOUNT_SOURCE=$current_source
        MOUNT_TARGET=$current_target
        MOUNT_WAS_EXISTING=1
    elif [ "$record_rc" -eq 1 ]; then
        MOUNT_PENDING=1
        mount --source "$PART_FD_PATH" --target "$MNT"
        record_rc=0
        record=$(current_mount_record) || record_rc=$?
        [ "$record_rc" -eq 0 ] ||
            die "new mount does not match retained p1 dev_t/source identity"
        IFS=$'\t' read -r current_id current_majmin current_source current_target <<<"$record"
        MOUNT_ID=$current_id
        MOUNT_MAJMIN=$current_majmin
        MOUNT_SOURCE=$current_source
        MOUNT_TARGET=$current_target
        MOUNTED_HERE=1
    else
        die "existing mount snapshot is unsafe or does not match retained p1"
    fi
    start_mount_root_holder
    if [ "$MOUNTED_HERE" = 1 ]; then
        MOUNT_PENDING=0
    fi
}

verify_retained_devices() {
    local phase=$1 whole_now part_now char_now ctrl_now
    local ctrl_path_now
    local whole_link part_link char_link ctrl_link
    local sys_dev serial nsid diskseq whole_controller char_controller controller
    local char_nsid controller_id pci_path pci_id ctrl_id
    if [ "$FULL_RUN_TEST" = 1 ] &&
       [ "${EXITOS_TEST_FAILPOINT:-}" = "device:$phase" ]; then
        return 1
    fi
    if [ "$DEVICE_TEST_MODE" = 1 ]; then
        [ -f "$WHOLE_FD_PATH" ] && [ -f "$PART_FD_PATH" ] &&
        [ -f "$CHAR_FD_PATH" ] && [ -f "$CTRL_FD_PATH" ] || return 1
        whole_now=$(stat -Lc '%d:%i' -- "$WHOLE_FD_PATH") || return 1
        part_now=$(stat -Lc '%d:%i' -- "$PART_FD_PATH") || return 1
        char_now=$(stat -Lc '%d:%i' -- "$CHAR_FD_PATH") || return 1
        ctrl_now=$(stat -Lc '%d:%i' -- "$CTRL_FD_PATH") || return 1
        ctrl_path_now=$ctrl_now
        [ ! -L "$EXITOS_DEV" ] && [ ! -L "$EXITOS_TESTPART" ] &&
        [ ! -L "$EXITOS_CHARDEV" ] && [ ! -L "$CTRL_DEVICE_PATH" ] || return 1
        [ "$(stat -Lc '%d:%i' -- "$EXITOS_DEV")" = "$WHOLE_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%d:%i' -- "$EXITOS_TESTPART")" = "$PART_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%d:%i' -- "$EXITOS_CHARDEV")" = "$CHAR_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%d:%i' -- "$CTRL_DEVICE_PATH")" = "$CTRL_OBJECT_ID" ] || return 1
    else
        [ -b "$WHOLE_FD_PATH" ] && [ -b "$PART_FD_PATH" ] &&
        [ -c "$CHAR_FD_PATH" ] && [ -c "$CTRL_FD_PATH" ] || return 1
        whole_now=$(stat -Lc '%t:%T' -- "$WHOLE_FD_PATH") || return 1
        part_now=$(stat -Lc '%t:%T' -- "$PART_FD_PATH") || return 1
        char_now=$(stat -Lc '%t:%T' -- "$CHAR_FD_PATH") || return 1
        ctrl_now=$(stat -Lc '%t:%T' -- "$CTRL_FD_PATH") || return 1
        ctrl_path_now=$(stat -Lc '%d:%i' -- "$CTRL_FD_PATH") || return 1
        [ ! -L "$EXITOS_DEV" ] && [ ! -L "$EXITOS_TESTPART" ] &&
        [ ! -L "$EXITOS_CHARDEV" ] && [ ! -L "$CTRL_DEVICE_PATH" ] || return 1
        [ "$(stat -Lc '%t:%T' -- "$EXITOS_DEV")" = "$WHOLE_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%t:%T' -- "$EXITOS_TESTPART")" = "$PART_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%t:%T' -- "$EXITOS_CHARDEV")" = "$CHAR_OBJECT_ID" ] || return 1
        [ "$(stat -Lc '%t:%T' -- "$CTRL_DEVICE_PATH")" = "$CTRL_OBJECT_ID" ] || return 1
    fi
    [ "$whole_now" = "$WHOLE_OBJECT_ID" ] && [ "$part_now" = "$PART_OBJECT_ID" ] &&
    [ "$char_now" = "$CHAR_OBJECT_ID" ] && [ "$ctrl_now" = "$CTRL_OBJECT_ID" ] || return 1
    [ "$ctrl_path_now" = "$CTRL_PATH_ID" ] &&
        [ "$(stat -Lc '%d:%i' -- "$CTRL_DEVICE_PATH")" = "$CTRL_PATH_ID" ] || return 1
    retained_fd_is_read_only "$WHOLE_FD" &&
    retained_fd_is_read_only "$PART_FD" &&
    retained_fd_is_read_only "$CHAR_FD" &&
    retained_fd_is_read_only "$CTRL_FD" || return 1
    whole_link=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/block/$WHOLE_MAJMIN") || return 1
    part_link=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/block/$PART_MAJMIN") || return 1
    char_link=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/char/$CHAR_MAJMIN") || return 1
    ctrl_link=$(readlink -f -- "$DEVICE_SYSFS_ROOT/dev/char/$CTRL_MAJMIN") || return 1
    [ "$whole_link" = "$WHOLE_SYSFS_PATH" ] && [ "$part_link" = "$PART_SYSFS_PATH" ] &&
    [ "$char_link" = "$CHAR_SYSFS_PATH" ] && [ "$ctrl_link" = "$CTRL_SYSFS_PATH" ] || return 1
    read -r sys_dev <"$WHOLE_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$WHOLE_MAJMIN" ] || return 1
    read -r sys_dev <"$PART_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$PART_MAJMIN" ] || return 1
    read -r sys_dev <"$CHAR_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$CHAR_MAJMIN" ] || return 1
    read -r sys_dev <"$CTRL_SYSFS_PATH/dev" || return 1
    [ "$sys_dev" = "$CTRL_MAJMIN" ] || return 1
    read -r sys_dev <"$PART_SYSFS_PATH/partition" || return 1
    [ "$sys_dev" = 1 ] && [ "${PART_SYSFS_PATH%/*}" = "$WHOLE_SYSFS_PATH" ] || return 1
    read -r nsid <"$WHOLE_SYSFS_PATH/nsid" || return 1
    [ "$nsid" = "$EXPECTED_NSID" ] || return 1
    read -r diskseq <"$WHOLE_SYSFS_PATH/diskseq" || {
        echo "cstate campaign: cannot reread namespace diskseq during $phase" >&2
        return 1
    }
    if [ "$diskseq" != "$WHOLE_DISKSEQ" ]; then
        echo "cstate campaign: namespace diskseq changed during $phase: expected $WHOLE_DISKSEQ, got $diskseq" >&2
        return 1
    fi
    whole_controller=$(readlink -f -- "$WHOLE_SYSFS_PATH/device") || return 1
    char_controller=$(readlink -f -- "$CHAR_SYSFS_PATH/device") || return 1
    [ "$whole_controller" = "$WHOLE_CONTROLLER_PATH" ] &&
        [ "$char_controller" = "$CHAR_CONTROLLER_PATH" ] &&
        [ "$char_controller" = "$whole_controller" ] || return 1
    [ "$ctrl_link" = "$whole_controller" ] &&
        [ "$CTRL_SYSFS_PATH" = "$WHOLE_CONTROLLER_PATH" ] || return 1
    controller_id=$(stat -Lc '%d:%i' -- "$whole_controller") || return 1
    [ "$controller_id" = "$WHOLE_CONTROLLER_ID" ] || return 1
    ctrl_id=$(stat -Lc '%d:%i' -- "$ctrl_link") || return 1
    [ "$ctrl_id" = "$CTRL_SYSFS_ID" ] &&
        [ "$ctrl_id" = "$WHOLE_CONTROLLER_ID" ] || return 1
    pci_path=$(readlink -f -- "$whole_controller/device") || return 1
    [ "$pci_path" = "$WHOLE_PCI_PATH" ] || return 1
    pci_id=$(stat -Lc '%d:%i' -- "$pci_path") || return 1
    [ "$pci_id" = "$WHOLE_PCI_ID" ] && [ -d "$pci_path/msi_irqs" ] || return 1
    serial=$(tr -d ' ' <"$WHOLE_CONTROLLER_PATH/serial") || return 1
    [ "$serial" = "$EXITOS_EXPECT_SERIAL" ] || return 1
    controller_from_namespace_path controller "$char_controller" || return 1
    [ "$controller" = "$NVME_CTRL" ] || return 1
    retained_char_nsid char_nsid || return 1
    [ "$char_nsid" = "$EXPECTED_NSID" ] || return 1
    printf 'DEVICE_IDENTITY phase=%s whole_dev_t=%s part_dev_t=%s char_dev_t=%s ctrl_dev_t=%s nsid=%s controller=%s serial=%s status=verified\n' \
        "$phase" "$WHOLE_MAJMIN" "$PART_MAJMIN" "$CHAR_MAJMIN" \
        "$CTRL_MAJMIN" "$EXPECTED_NSID" "$NVME_CTRL" "$EXITOS_EXPECT_SERIAL"
}

cpu_list_contains_cpu() {
    local list=$1 wanted=$2 item first last first_value last_value
    local -a items=()
    [[ $list =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] || return 2
    IFS=, read -r -a items <<<"$list"
    for item in "${items[@]}"; do
        if [[ $item == *-* ]]; then
            first=${item%-*}
            last=${item#*-}
        else
            first=$item
            last=$item
        fi
        (( ${#first} <= 10 && ${#last} <= 10 )) || return 2
        first_value=$((10#$first))
        last_value=$((10#$last))
        (( first_value <= last_value && last_value <= 2147483647 )) || return 2
        if (( wanted >= first_value && wanted <= last_value )); then
            return 0
        fi
    done
    return 1
}

resolve_irq_binding() {
    local hctx_path cpu_list contains_rc hctx qid action interrupts
    local configured effective irq_label irq_value last_index
    local -a fields=()
    local -a matching_hctx=()
    local -a matching_irq=()

    for hctx_path in "$WHOLE_SYSFS_PATH"/mq/[0-9]*; do
        [ -d "$hctx_path" ] || continue
        hctx=${hctx_path##*/}
        [[ $hctx =~ ^[0-9]+$ && ${#hctx} -le 10 ]] || {
            echo "cstate campaign: malformed blk-mq hctx name: $hctx" >&2
            return 1
        }
        read -r cpu_list <"$hctx_path/cpu_list" || {
            echo "cstate campaign: cannot read blk-mq hctx $hctx cpu_list" >&2
            return 1
        }
        contains_rc=0
        cpu_list_contains_cpu "$cpu_list" "$CPU" || contains_rc=$?
        case "$contains_rc" in
          0) matching_hctx+=("$hctx") ;;
          1) ;;
          *)
            echo "cstate campaign: malformed blk-mq hctx $hctx cpu_list" >&2
            return 1
            ;;
        esac
    done
    if [ "${#matching_hctx[@]}" -ne 1 ]; then
        echo "cstate campaign: expected exactly one hctx for CPU $CPU, found ${#matching_hctx[@]}" >&2
        return 1
    fi
    hctx=${matching_hctx[0]}
    (( 10#$hctx < 2147483647 )) || {
        echo "cstate campaign: blk-mq hctx is out of range" >&2
        return 1
    }
    qid=$((10#$hctx + 1))
    action="${NVME_CTRL}q${qid}"
    interrupts="$IRQ_PROC_ROOT/interrupts"
    [ -r "$interrupts" ] || {
        echo "cstate campaign: cannot read IRQ counters" >&2
        return 1
    }
    while IFS= read -r irq_line; do
        read -r -a fields <<<"$irq_line"
        [ "${#fields[@]}" -gt 1 ] || continue
        irq_label=${fields[0]}
        [[ $irq_label =~ ^([0-9]+):$ ]] || continue
        last_index=$((${#fields[@]} - 1))
        [ "${fields[$last_index]}" = "$action" ] || continue
        irq_value=${BASH_REMATCH[1]}
        [[ ${#irq_value} -le 10 ]] || {
            echo "cstate campaign: IRQ number for $action is out of range" >&2
            return 1
        }
        (( 10#$irq_value <= 2147483647 )) || {
            echo "cstate campaign: IRQ number for $action is out of range" >&2
            return 1
        }
        matching_irq+=("$((10#$irq_value))")
    done <"$interrupts"
    if [ "${#matching_irq[@]}" -ne 1 ]; then
        echo "cstate campaign: expected exactly one IRQ action $action, found ${#matching_irq[@]}" >&2
        return 1
    fi
    RESOLVED_IRQ=${matching_irq[0]}
    [ -e "$WHOLE_PCI_PATH/msi_irqs/$RESOLVED_IRQ" ] || {
        echo "cstate campaign: IRQ $RESOLVED_IRQ is not owned by retained controller PCI function" >&2
        return 1
    }
    read -r configured <"$IRQ_PROC_ROOT/irq/$RESOLVED_IRQ/smp_affinity_list" || {
        echo "cstate campaign: cannot read IRQ affinity for IRQ $RESOLVED_IRQ" >&2
        return 1
    }
    read -r effective <"$IRQ_PROC_ROOT/irq/$RESOLVED_IRQ/effective_affinity_list" || {
        echo "cstate campaign: cannot read effective IRQ affinity for IRQ $RESOLVED_IRQ" >&2
        return 1
    }
    if [ "$configured" != "$CPU" ] || [ "$effective" != "$CPU" ]; then
        echo "cstate campaign: IRQ affinity mismatch for IRQ $RESOLVED_IRQ: configured=$configured effective=$effective expected=$CPU" >&2
        return 1
    fi
    RESOLVED_HCTX=$((10#$hctx))
    RESOLVED_QID=$qid
    RESOLVED_IRQ_CONFIGURED=$configured
    RESOLVED_IRQ_EFFECTIVE=$effective
    RESOLVED_BINDING_TOKEN="$WHOLE_MAJMIN|$WHOLE_DISKSEQ|$WHOLE_CONTROLLER_ID|$WHOLE_PCI_ID|$RESOLVED_HCTX|$RESOLVED_QID|$RESOLVED_IRQ|$configured|$effective"
}

safe_counter_add() {
    local accumulator_name=$1 raw=$2 value
    local -n accumulator=$accumulator_name
    [[ $raw =~ ^[0-9]+$ && ${#raw} -le 18 ]] || return 1
    value=$((10#$raw))
    (( accumulator <= 9000000000000000000 - value )) || return 1
    accumulator=$((accumulator + value))
}

capture_irq_counters() {
    local interrupts="$IRQ_PROC_ROOT/interrupts" header line label irq_value
    local cpu_index selected_index=-1 sibling_index=-1 target_rows=0
    local selected_total=0 sibling_total=0 target_selected=0 target_sibling=0
    local target_total=0 field value is_numeric_irq is_target
    local -a cpus=()
    local -a fields=()

    IFS= read -r header <"$interrupts" || {
        echo "cstate campaign: cannot snapshot IRQ counter header" >&2
        return 1
    }
    read -r -a cpus <<<"$header"
    [ "${#cpus[@]}" -gt 0 ] || {
        echo "cstate campaign: empty IRQ counter header" >&2
        return 1
    }
    for cpu_index in "${!cpus[@]}"; do
        [ "${cpus[$cpu_index]}" = "CPU$CPU" ] && selected_index=$cpu_index
        [ "${cpus[$cpu_index]}" = "CPU$SIBLING" ] && sibling_index=$cpu_index
    done
    if [ "$selected_index" -lt 0 ] || [ "$sibling_index" -lt 0 ]; then
        echo "cstate campaign: selected CPU or SMT sibling is absent from IRQ counter header" >&2
        return 1
    fi

    while IFS= read -r line; do
        read -r -a fields <<<"$line"
        [ "${#fields[@]}" -gt 0 ] || continue
        label=${fields[0]}
        [[ $label == *: ]] || continue
        is_numeric_irq=0
        irq_value=-
        if [[ $label =~ ^([0-9]+):$ ]]; then
            is_numeric_irq=1
            irq_value=${BASH_REMATCH[1]}
        fi
        if [ "${#fields[@]}" -le "${#cpus[@]}" ]; then
            if [ "$is_numeric_irq" -eq 1 ]; then
                echo "cstate campaign: truncated numeric IRQ counter row" >&2
                return 1
            fi
            continue
        fi
        is_target=0
        [ "$irq_value" != "$RESOLVED_IRQ" ] || is_target=1
        for cpu_index in "${!cpus[@]}"; do
            field=$((cpu_index + 1))
            value=${fields[$field]}
            [[ $value =~ ^[0-9]+$ && ${#value} -le 18 ]] || {
                echo "cstate campaign: malformed numeric IRQ counter" >&2
                return 1
            }
            if [ "$cpu_index" -eq "$selected_index" ]; then
                safe_counter_add selected_total "$value" || {
                    echo "cstate campaign: selected-CPU IRQ counter overflow" >&2
                    return 1
                }
            fi
            if [ "$cpu_index" -eq "$sibling_index" ]; then
                safe_counter_add sibling_total "$value" || {
                    echo "cstate campaign: sibling-CPU IRQ counter overflow" >&2
                    return 1
                }
            fi
            if [ "$is_target" -eq 1 ]; then
                safe_counter_add target_total "$value" || {
                    echo "cstate campaign: target IRQ counter overflow" >&2
                    return 1
                }
                if [ "$cpu_index" -eq "$selected_index" ]; then
                    target_selected=$((10#$value))
                elif [ "$cpu_index" -eq "$sibling_index" ]; then
                    target_sibling=$((10#$value))
                fi
            fi
        done
        if [ "$is_target" -eq 1 ]; then
            target_rows=$((target_rows + 1))
        fi
    done < <(sed -n '2,$p' "$interrupts")
    if [ "$target_rows" -ne 1 ]; then
        echo "cstate campaign: expected exactly one target IRQ counter row, found $target_rows" >&2
        return 1
    fi
    (( selected_total >= target_selected && sibling_total >= target_sibling )) || return 1
    IRQ_TARGET_SELECTED=$target_selected
    IRQ_TARGET_TOTAL=$target_total
    IRQ_SELECTED_NON_TARGET=$((selected_total - target_selected))
    IRQ_SIBLING_NON_TARGET=$((sibling_total - target_sibling))
}

normalize_cpufreq_cpu_list() {
    local outvar=$1 raw=$2 token normalized=""
    local -a tokens=()
    raw=${raw//$'\t'/ }
    raw=${raw//,/ }
    read -r -a tokens <<<"$raw"
    [ "${#tokens[@]}" -gt 0 ] || return 1
    for token in "${tokens[@]}"; do
        [[ $token =~ ^[0-9]+(-[0-9]+)?$ ]] || return 1
        normalized+="${normalized:+,}$token"
    done
    printf -v "$outvar" '%s' "$normalized"
}

read_cpufreq_field() {
    local outvar=$1 path=$2 value
    [ -r "$path" ] || return 1
    IFS= read -r value <"$path" || return 1
    [ -n "$value" ] || return 1
    printf -v "$outvar" '%s' "$value"
}

retain_cpufreq_profile() {
    local represented_cpu policy policy_dir policy_name policy_id related_raw related
    local governor epp min_khz max_khz index token="$CPUFREQ_PROFILE_NAME"
    local -a represented_cpus=("$CPU" "$SIBLING")

    CPUFREQ_POLICY_PATH=()
    CPUFREQ_POLICY_NAME=()
    CPUFREQ_POLICY_ID=()
    CPUFREQ_POLICY_RELATED=()
    CPUFREQ_POLICY_INDEX_BY_PATH=()
    CPUFREQ_POLICY_COUNT=0
    for represented_cpu in "${represented_cpus[@]}"; do
        policy=$(readlink -f -- "$SYSROOT/cpu$represented_cpu/cpufreq") ||
            die "required $CPUFREQ_PROFILE_NAME cpufreq profile is missing for CPU $represented_cpu"
        policy_dir=${policy%/*}
        policy_name=${policy##*/}
        [ "$policy_dir" = "$SYSROOT/cpufreq" ] &&
        [[ $policy_name =~ ^policy[0-9]+$ ]] && [ -d "$policy" ] ||
            die "required $CPUFREQ_PROFILE_NAME cpufreq policy for CPU $represented_cpu is noncanonical"
        policy_id=$(stat -Lc '%d:%i' -- "$policy") ||
            die "cannot retain cpufreq policy identity for CPU $represented_cpu"
        read_cpufreq_field related_raw "$policy/related_cpus" &&
        normalize_cpufreq_cpu_list related "$related_raw" &&
        cpu_list_contains_cpu "$related" "$represented_cpu" ||
            die "required $CPUFREQ_PROFILE_NAME cpufreq policy omits CPU $represented_cpu"
        if [ -n "${CPUFREQ_POLICY_INDEX_BY_PATH[$policy]+x}" ]; then
            index=${CPUFREQ_POLICY_INDEX_BY_PATH[$policy]}
            [ "${CPUFREQ_POLICY_ID[$index]}" = "$policy_id" ] &&
            [ "${CPUFREQ_POLICY_RELATED[$index]}" = "$related" ] ||
                die "cpufreq policy identity changed while retaining target core"
            continue
        fi
        read_cpufreq_field governor "$policy/scaling_governor" || governor=unreadable
        read_cpufreq_field epp "$policy/energy_performance_preference" || epp=unreadable
        read_cpufreq_field min_khz "$policy/scaling_min_freq" || min_khz=unreadable
        read_cpufreq_field max_khz "$policy/scaling_max_freq" || max_khz=unreadable
        if [ "$governor" != performance ] || [ "$epp" != performance ] ||
           [ "$min_khz" != "$CPUFREQ_TARGET_KHZ" ] ||
           [ "$max_khz" != "$CPUFREQ_TARGET_KHZ" ]; then
            die "required $CPUFREQ_PROFILE_NAME cpufreq profile mismatch on $policy_name: governor=$governor epp=$epp min=$min_khz max=$max_khz"
        fi
        index=$CPUFREQ_POLICY_COUNT
        CPUFREQ_POLICY_INDEX_BY_PATH[$policy]=$index
        CPUFREQ_POLICY_PATH[$index]=$policy
        CPUFREQ_POLICY_NAME[$index]=$policy_name
        CPUFREQ_POLICY_ID[$index]=$policy_id
        CPUFREQ_POLICY_RELATED[$index]=$related
        CPUFREQ_POLICY_COUNT=$((CPUFREQ_POLICY_COUNT + 1))
        token+=";$policy_name@$policy_id@related=$related@governor=performance@epp=performance@min=$CPUFREQ_TARGET_KHZ@max=$CPUFREQ_TARGET_KHZ"
    done
    [ "$CPUFREQ_POLICY_COUNT" -gt 0 ] ||
        die "required $CPUFREQ_PROFILE_NAME cpufreq profile has no policy"
    CPUFREQ_PROFILE_TOKEN=$token
}

verify_cpufreq_profile() {
    local phase=$1 represented_cpu policy policy_id related_raw related index
    local governor epp min_khz max_khz
    local -a represented_cpus=("$CPU" "$SIBLING")
    local -A verified=()

    for represented_cpu in "${represented_cpus[@]}"; do
        policy=$(readlink -f -- "$SYSROOT/cpu$represented_cpu/cpufreq") || {
            echo "cstate campaign: cpufreq profile lost CPU $represented_cpu during $phase" >&2
            return 1
        }
        if [ -z "${CPUFREQ_POLICY_INDEX_BY_PATH[$policy]+x}" ]; then
            echo "cstate campaign: cpufreq profile policy mapping changed during $phase" >&2
            return 1
        fi
        index=${CPUFREQ_POLICY_INDEX_BY_PATH[$policy]}
        policy_id=$(stat -Lc '%d:%i' -- "$policy") || return 1
        [ "$policy_id" = "${CPUFREQ_POLICY_ID[$index]}" ] || {
            echo "cstate campaign: cpufreq profile policy identity changed during $phase" >&2
            return 1
        }
        read_cpufreq_field related_raw "$policy/related_cpus" &&
        normalize_cpufreq_cpu_list related "$related_raw" || return 1
        [ "$related" = "${CPUFREQ_POLICY_RELATED[$index]}" ] &&
        cpu_list_contains_cpu "$related" "$represented_cpu" || {
            echo "cstate campaign: cpufreq profile policy scope changed during $phase" >&2
            return 1
        }
        verified[$index]=1
    done
    [ "${#verified[@]}" -eq "$CPUFREQ_POLICY_COUNT" ] || {
        echo "cstate campaign: cpufreq profile unique policy set changed during $phase" >&2
        return 1
    }
    for ((index = 0; index < CPUFREQ_POLICY_COUNT; index++)); do
        policy=${CPUFREQ_POLICY_PATH[$index]}
        read_cpufreq_field governor "$policy/scaling_governor" || governor=unreadable
        read_cpufreq_field epp "$policy/energy_performance_preference" || epp=unreadable
        read_cpufreq_field min_khz "$policy/scaling_min_freq" || min_khz=unreadable
        read_cpufreq_field max_khz "$policy/scaling_max_freq" || max_khz=unreadable
        if [ "$governor" != performance ] || [ "$epp" != performance ] ||
           [ "$min_khz" != "$CPUFREQ_TARGET_KHZ" ] ||
           [ "$max_khz" != "$CPUFREQ_TARGET_KHZ" ]; then
            echo "cstate campaign: cpufreq profile mismatch during $phase on ${CPUFREQ_POLICY_NAME[$index]}: governor=$governor epp=$epp min=$min_khz max=$max_khz" >&2
            return 1
        fi
    done
}

verify_app_execution_contract() {
    local pid=$1 phase=$2 line cpu_list="" stat_line stat_rest
    local app_nice app_policy app_ppid
    local -a stat_fields=()

    [[ $pid =~ ^[1-9][0-9]*$ ]] || return 1
    while IFS= read -r line; do
        case "$line" in
          Cpus_allowed_list:*)
            cpu_list=${line#*:}
            cpu_list=${cpu_list//[[:space:]]/}
            ;;
        esac
    done <"$APP_PROC_ROOT/$pid/status" || return 1
    [ "$cpu_list" = "$CPU" ] || {
        echo "cstate campaign: qdsplit execution contract affinity mismatch during $phase: expected=$CPU actual=${cpu_list:-unreadable}" >&2
        return 1
    }

    IFS= read -r stat_line <"$APP_PROC_ROOT/$pid/stat" || return 1
    [[ $stat_line == *') '* ]] || return 1
    stat_rest=${stat_line##*) }
    read -r -a stat_fields <<<"$stat_rest"
    [ "${#stat_fields[@]}" -ge 39 ] || return 1
    app_ppid=${stat_fields[1]}
    app_nice=${stat_fields[16]}
    app_policy=${stat_fields[38]}
    [ "$app_ppid" = "$BASHPID" ] || {
        echo "cstate campaign: qdsplit execution contract child identity mismatch during $phase" >&2
        return 1
    }
    [ "$app_policy" = 0 ] || {
        echo "cstate campaign: qdsplit execution contract policy mismatch during $phase: expected=SCHED_OTHER actual=$app_policy" >&2
        return 1
    }
    [ "$app_nice" = -20 ] || {
        echo "cstate campaign: qdsplit execution contract nice mismatch during $phase: expected=-20 actual=$app_nice" >&2
        return 1
    }
    APP_EXECUTION_TOKEN="selected_cpu=$CPU@policy=SCHED_OTHER@nice=-20"
    printf 'APP_EXECUTION phase=%s pid=%s selected_cpu=%s policy=SCHED_OTHER nice=-20 status=verified\n' \
        "$phase" "$pid" "$CPU"
}

MODE=run
SCREEN=primary
ARCHIVE_RESULT=""
case "${1:-}" in
  --plan)
    [ "$#" -eq 1 ] || die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]"
    MODE=plan
    ;;
  --opt-plan)
    [ "$#" -eq 1 ] || die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]"
    MODE=plan
    SCREEN=opt
    ;;
  --opt-run)
    [ "$#" -eq 1 ] || die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]"
    MODE=run
    SCREEN=opt
    ;;
  --blockpoll-plan)
    [ "$#" -eq 1 ] || die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]"
    MODE=plan
    SCREEN=blockpoll
    ;;
  --archive-plan)
    [ "$#" -eq 2 ] || die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]"
    MODE=archive-plan
    SCREEN=blockpoll
    ARCHIVE_RESULT=$2
    ;;
  --test-cpufreq-profile)
    [ "$#" -eq 2 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-cpufreq-profile is available only to the pure unit test"
    MODE=test-cpufreq-profile
    TEST_PROFILE_SIBLING=$2
    ;;
  --test-irq-counters)
    [ "$#" -eq 3 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-irq-counters is available only to the pure unit test"
    MODE=test-irq-counters
    TEST_COUNTER_IRQ=$2
    TEST_COUNTER_SIBLING=$3
    ;;
  --test-cell-env)
    [ "$#" -eq 2 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-cell-env is available only to the pure unit test"
    MODE=test-cell
    TEST_BUFFER=$2
    ;;
  --test-resolver)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-resolver is available only to the pure unit test"
    MODE=test-resolver
    ;;
  --test-layout)
    [ "$#" -eq 2 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-layout is available only to the pure unit test"
    MODE=test-layout
    TEST_LAYOUT_OUTPUT=$2
    ;;
  --test-layout-contract)
    [ "$#" -eq 3 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-layout-contract is available only to the pure unit test"
    MODE=test-layout-contract
    TEST_LAYOUT_OUTPUT=$2
    TEST_LAYOUT_BUFFER=$3
    ;;
  --test-prepare-paths)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-prepare-paths is available only to the pure unit test"
    MODE=test-paths
    ;;
  --test-output-retained)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-output-retained is available only to the pure unit test"
    MODE=test-output-retained
    ;;
  --test-retained-devices)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-retained-devices is available only to the pure unit test"
    MODE=test-retained-devices
    ;;
  --test-poll-snapshot)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-poll-snapshot is available only to the pure unit test"
    MODE=test-poll-snapshot
    ;;
  --test-child-fd-hygiene)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-child-fd-hygiene is available only to the pure unit test"
    MODE=test-child-fd-hygiene
    ;;
  --test-owned-mount-acquire)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-owned-mount-acquire is available only to the pure unit test"
    MODE=test-owned-mount-acquire
    ;;
  --test-owned-mount-cleanup)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-owned-mount-cleanup is available only to the pure unit test"
    MODE=test-owned-mount-cleanup
    ;;
  --test-full-run)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-full-run is available only to the pure unit test"
    MODE=run
    FULL_RUN_TEST=1
    DEVICE_TEST_MODE=1
    PRIVATE_MOUNT_NAMESPACE=1
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires sysfs root}
    IRQ_PROC_ROOT=${EXITOS_IRQ_PROC_ROOT:?pure test requires proc IRQ root}
    APP_PROC_ROOT=${EXITOS_TEST_APP_PROC_ROOT:?pure test requires app proc root}
    CORE_UTIL_SEQUENCE_DIR=${EXITOS_TEST_CPU_UTIL_SEQUENCE_DIR:?pure test requires CPU-util sequence}
    [ "${EXITOS_TEST_CPU_UTIL_WINDOWS:-}" = 2 ] ||
        die "pure test requires two CPU-util windows"
    CORE_UTIL_WINDOWS=2
    NVME_BIN=${EXITOS_TEST_NVME_BIN:?pure test requires nvme NSID helper}
    ;;
  --test-opt-full-run)
    [ "$#" -eq 1 ] && [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" = 1 ] ||
        die "test-opt-full-run is available only to the pure unit test"
    MODE=run
    SCREEN=opt
    FULL_RUN_TEST=1
    DEVICE_TEST_MODE=1
    PRIVATE_MOUNT_NAMESPACE=1
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires sysfs root}
    IRQ_PROC_ROOT=${EXITOS_IRQ_PROC_ROOT:?pure test requires proc IRQ root}
    APP_PROC_ROOT=${EXITOS_TEST_APP_PROC_ROOT:?pure test requires app proc root}
    CORE_UTIL_SEQUENCE_DIR=${EXITOS_TEST_CPU_UTIL_SEQUENCE_DIR:?pure test requires CPU-util sequence}
    [ "${EXITOS_TEST_CPU_UTIL_WINDOWS:-}" = 2 ] ||
        die "pure test requires two CPU-util windows"
    CORE_UTIL_WINDOWS=2
    NVME_BIN=${EXITOS_TEST_NVME_BIN:?pure test requires nvme NSID helper}
    ;;
  --private-mount-namespace-run)
    [ "$#" -eq 1 ] || die "invalid private mount-namespace entry"
    MODE=run
    PRIVATE_NAMESPACE_ENTRY=1
    ;;
  --private-mount-namespace-opt-run)
    [ "$#" -eq 1 ] || die "invalid private mount-namespace entry"
    MODE=run
    SCREEN=opt
    PRIVATE_NAMESPACE_ENTRY=1
    ;;
  "") ;;
  *) die "usage: $0 [--plan|--opt-plan|--opt-run|--blockpoll-plan|--archive-plan RESULT_DIR]" ;;
esac

if { [ -n "${EXITOS_TEST_ARCHIVE_AFTER_PARENT_RETAIN_HOOK+x}" ] ||
     [ -n "${EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK+x}" ] ||
     [ -n "${EXITOS_TEST_ARCHIVE_FAIL_AFTER_SEAL_RENAME+x}" ] ||
     [ -n "${EXITOS_TEST_BLOCKPOLL_PLAN_PRODUCER_FAIL+x}" ] ||
     [ -n "${EXITOS_TEST_ARCHIVE_HASH_FAIL_AFTER_PLAN+x}" ]; } &&
   [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" != 1 ]; then
    die "archive test hook is pure-test-only"
fi
if { [ -n "${EXITOS_TEST_CHILD_FD_AUDIT_DIR+x}" ] ||
     [ -n "${EXITOS_TEST_FD_AUDITOR+x}" ] ||
     [ -n "${EXITOS_TEST_FD_OBJECT_IDS+x}" ]; } &&
   [ "${EXITOS_CAMPAIGN_TEST_MODE:-}" != 1 ]; then
    die "child-fd audit hook is pure-test-only"
fi
if { [ -n "${EXITOS_TEST_CTRLDEV+x}" ] ||
     [ -n "${EXITOS_TEST_CTRL_MAJMIN+x}" ] ||
     [ -n "${EXITOS_TEST_CTRL_SYSFS_PATH+x}" ]; } &&
   [ "$MODE" = run ] && [ "$FULL_RUN_TEST" != 1 ]; then
    die "controller identity overrides are pure-test-only"
fi
if { [ -n "${EXITOS_TEST_POLL_PROC_ROOT+x}" ] ||
     [ -n "${EXITOS_TEST_BLKGETDISKSEQ_FILE+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_BEFORE+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_AFTER+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_HELPER_PAUSE+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1+x}" ] ||
     [ -n "${EXITOS_TEST_POLL_PARTIAL_OUTPUT+x}" ]; } &&
   [ "$MODE" = run ] && [ "$FULL_RUN_TEST" != 1 ]; then
    die "poll snapshot overrides are pure-test-only"
fi

if { [ "$MODE" = run ] || [ "$MODE" = plan ] || [ "$MODE" = archive-plan ]; } &&
   [ "$FULL_RUN_TEST" != 1 ]; then
    [ "$REPS" -eq 5 ] ||
        die "$SCREEN production campaign requires REPS=5"
    [ "$KEEPER_MODES_RAW" = off,same ] && [ "$KEEPER_MODES_TOKEN" = off,same ] ||
        die "$SCREEN production campaign requires exact keeper modes off,same"
fi
EXPECTED_CELLS=$((REPS * 2 * ${#KEEPER_MODES[@]}))
if [ "$FULL_RUN_TEST" = 1 ]; then
    [[ $APP_PROC_ROOT == /* && $APP_PROC_ROOT != *$'\n'* &&
       $APP_PROC_ROOT != /proc && $APP_PROC_ROOT != /proc/* &&
       -d $APP_PROC_ROOT && ! -L $APP_PROC_ROOT ]] ||
        die "invalid pure-test app proc root"
    [[ $CORE_UTIL_SEQUENCE_DIR == /* &&
       $CORE_UTIL_SEQUENCE_DIR != *$'\n'* &&
       $CORE_UTIL_SEQUENCE_DIR != /proc &&
       $CORE_UTIL_SEQUENCE_DIR != /proc/* &&
       -d $CORE_UTIL_SEQUENCE_DIR && ! -L $CORE_UTIL_SEQUENCE_DIR ]] ||
        die "invalid pure-test CPU-util sequence directory"
    case "${EXITOS_TEST_SENTINEL_INTERVAL:-}" in
      "") ;;
      0.01) NOISE_SENTINEL_INTERVAL=0.01 ;;
      *) die "invalid pure-test sentinel interval" ;;
    esac
    case "${EXITOS_TEST_SENTINEL_TRACE:-}" in
      "") ;;
      1) NOISE_SENTINEL_TRACE=1 ;;
      *) die "invalid pure-test sentinel trace mode" ;;
    esac
else
    [ -z "${EXITOS_TEST_APP_PROC_ROOT+x}" ] ||
        die "app proc root override is pure-test-only"
    [ -z "${EXITOS_TEST_CPU_UTIL_SEQUENCE_DIR+x}" ] ||
        die "CPU-util sequence override is pure-test-only"
    [ -z "${EXITOS_TEST_CPU_UTIL_WINDOWS+x}" ] ||
        die "CPU-util window override is pure-test-only"
    [ -z "${EXITOS_TEST_SENTINEL_INTERVAL+x}" ] ||
        die "sentinel interval override is pure-test-only"
    [ -z "${EXITOS_TEST_SENTINEL_TRACE+x}" ] ||
        die "sentinel trace override is pure-test-only"
fi

if [ "$MODE" = test-poll-snapshot ]; then
    if [[ -e /proc/$$/fd/9 || -L /proc/$$/fd/9 ]]; then
        echo "poll snapshot: parent_fd9_in_use" >&2
        exit 1
    fi
    DEVICE_TEST_MODE=1
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires poll sysfs root}
    POLL_PROC_ROOT=${EXITOS_TEST_POLL_PROC_ROOT:?pure test requires poll proc root}
    POLL_FAKE_DISKSEQ_FILE=${EXITOS_TEST_BLKGETDISKSEQ_FILE:?pure test requires fake diskseq file}
    CTRL_DEVICE_PATH=${EXITOS_TEST_CTRLDEV:?pure test requires controller device path}
    POLL_EXPECTED_CTRL_SYSFS=${EXITOS_TEST_CTRL_SYSFS_PATH:?pure test requires controller sysfs path}
    POLL_HELPER_PAUSE=${EXITOS_TEST_POLL_HELPER_PAUSE:-}
    POLL_ALIAS_PAUSE=${EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN:-}
    POLL_DEVICE_LINK_PAUSE=${EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN:-}
    POLL_MQ_LIST_PAUSE=${EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST:-}
    POLL_CAPTURE1_PAUSE=${EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1:-}
    POLL_PARTIAL_OUTPUT=${EXITOS_TEST_POLL_PARTIAL_OUTPUT:-}
    for poll_hook in "${EXITOS_TEST_POLL_PAUSE_BEFORE:-}" \
                     "${EXITOS_TEST_POLL_PAUSE_AFTER:-}" \
                     "$POLL_HELPER_PAUSE" "$POLL_ALIAS_PAUSE" \
                     "$POLL_DEVICE_LINK_PAUSE" "$POLL_MQ_LIST_PAUSE" \
                     "$POLL_CAPTURE1_PAUSE" \
                     "$POLL_PARTIAL_OUTPUT"; do
        case "$poll_hook" in ""|1) ;; *) die "invalid pure-test poll snapshot hook" ;; esac
    done
    prepare_poll_state
    retain_poll_test_devices
    if [ "${EXITOS_TEST_POLL_PAUSE_BEFORE:-}" = 1 ]; then
        poll_parent_pause POLL_SNAPSHOT_PARENT_READY || {
            echo "poll snapshot: parent_pause_failed" >&2
            exit 1
        }
    fi
    poll_snapshot_rc=0
    launch_poll_snapshot_helper || poll_snapshot_rc=$?
    if [ "$poll_snapshot_rc" -ne 0 ]; then
        cleanup_resources || true
        trap - EXIT INT TERM
        exit "$poll_snapshot_rc"
    fi
    if [ "${EXITOS_TEST_POLL_PAUSE_AFTER:-}" = 1 ]; then
        poll_parent_pause POLL_SNAPSHOT_PARENT_AFTER || {
            echo "poll snapshot: parent_pause_failed" >&2
            exit 1
        }
    fi
    poll_read_rc=0
    IFS= read -r -d '' POLL_FRAME <"$POLL_CAPTURE_FILE" || poll_read_rc=$?
    if [ "$poll_read_rc" -ne 1 ] || [ -z "$POLL_FRAME" ] ||
       ! validate_poll_frame_builtin "$POLL_FRAME"; then
        echo "poll snapshot: frame_malformed" >&2
        cleanup_resources || true
        trap - EXIT INT TERM
        exit 1
    fi
    cleanup_resources || {
        trap - EXIT INT TERM
        exit 1
    }
    trap - EXIT INT TERM
    printf '%s' "$POLL_FRAME"
    exit 0
elif [ "$MODE" = test-cpufreq-profile ]; then
    SIBLING=$TEST_PROFILE_SIBLING
    normalize_uint SIBLING 0 1023
    retain_cpufreq_profile
    case "${EXITOS_TEST_CPUFREQ_PAUSE:-}" in
      1)
        printf 'CPUFREQ_PROFILE_READY\n'
        kill -STOP "$$"
        ;;
      "") ;;
      *) die "invalid pure-test cpufreq profile pause" ;;
    esac
    verify_cpufreq_profile test-post ||
        die "cpufreq profile failed pure-test post readback"
    printf 'CPUFREQ_PROFILE policies=%s token=%s\n' \
        "$CPUFREQ_POLICY_COUNT" "$CPUFREQ_PROFILE_TOKEN"
    exit 0
elif [ "$MODE" = test-irq-counters ]; then
    RESOLVED_IRQ=$TEST_COUNTER_IRQ
    SIBLING=$TEST_COUNTER_SIBLING
    normalize_uint RESOLVED_IRQ 1 2147483647
    normalize_uint SIBLING 0 1023
    IRQ_PROC_ROOT=${EXITOS_IRQ_PROC_ROOT:?pure test requires proc IRQ root}
    [[ $IRQ_PROC_ROOT == /* && $IRQ_PROC_ROOT != *$'\n'* &&
       $IRQ_PROC_ROOT != /proc && $IRQ_PROC_ROOT != /proc/* &&
       -d $IRQ_PROC_ROOT && ! -L $IRQ_PROC_ROOT ]] ||
        die "invalid pure-test proc IRQ root"
    capture_irq_counters || die "cannot capture pure-test IRQ counters"
    printf 'IRQ_COUNTERS target_selected=%s target_total=%s selected_non_target=%s sibling_non_target=%s\n' \
        "$IRQ_TARGET_SELECTED" "$IRQ_TARGET_TOTAL" \
        "$IRQ_SELECTED_NON_TARGET" "$IRQ_SIBLING_NON_TARGET"
    exit 0
elif [ "$MODE" = test-cell ]; then
    [ -x "$QDSPLIT" ] || die "fake qdsplit is not executable"
    configure_buffer_env "$TEST_BUFFER"
    "$QDSPLIT" --capture-env
    exit 0
elif [ "$MODE" = test-resolver ]; then
    resolve_testdisk
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$EXITOS_DEV" "$EXITOS_EXPECT_SERIAL" "$EXITOS_FSPART" \
        "$EXITOS_WINDOW_IN_PART" "$EXITOS_TESTPART" "$EXITOS_CHARDEV"
    exit 0
elif [ "$MODE" = test-layout ]; then
    buffer_layout_from_output "$TEST_LAYOUT_OUTPUT"
    exit $?
elif [ "$MODE" = test-layout-contract ]; then
    test_layout=$(buffer_layout_from_output "$TEST_LAYOUT_OUTPUT") || exit $?
    validate_buffer_layout "$TEST_LAYOUT_BUFFER" "$test_layout" ||
        die "PFN proof does not match the 8 KiB $TEST_LAYOUT_BUFFER contract"
    printf '%s\n' "$test_layout"
    exit 0
elif [ "$MODE" = test-paths ]; then
    prepare_output
    prepare_run_dir
    printf 'OUTPUT=%s\n' "$OUT"
    printf 'WORKDIR=%s mode=%s owner=%s\n' "$RUN_DIR" \
        "$(stat -Lc '%a' -- "$RUN_DIR")" "$(stat -Lc '%u' -- "$RUN_DIR")"
    exit 0
elif [ "$MODE" = test-output-retained ]; then
    prepare_output
    printf 'OUTPUT_FD_READY\n'
    kill -STOP "$$"
    printf 'RETAINED_FD_WRITE\n' >&3
    exit 0
elif [ "$MODE" = test-retained-devices ]; then
    DEVICE_TEST_MODE=1
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires sysfs root}
    NVME_BIN=${EXITOS_TEST_NVME_BIN:?pure test requires nvme NSID helper}
    [[ $DEVICE_SYSFS_ROOT == /* && $DEVICE_SYSFS_ROOT != *$'\n'* ]] ||
        die "invalid pure-test sysfs root"
    [[ $NVME_BIN == /* && -x $NVME_BIN ]] ||
        die "invalid pure-test nvme NSID helper"
    retain_partition
    retain_whole_device
    retain_controller_device
    retain_char_device
    case "${EXITOS_TEST_RETAIN_PAUSE:-}" in
      1)
        printf 'RETAINED_DEVICES_READY\n'
        kill -STOP "$$"
        ;;
      "") ;;
      *) die "invalid pure-test retained-device pause" ;;
    esac
    verify_retained_devices setup || die "retained device identity verification failed"
    exit 0
elif [ "$MODE" = test-child-fd-hygiene ]; then
    DEVICE_TEST_MODE=1
    DEVICE_SYSFS_ROOT=${EXITOS_DEVICE_SYSFS_ROOT:?pure test requires sysfs root}
    IRQ_PROC_ROOT=${EXITOS_IRQ_PROC_ROOT:?pure test requires proc root}
    NVME_BIN=${EXITOS_TEST_NVME_BIN:?pure test requires nvme NSID helper}
    TMP=${EXITOS_TEST_CHILD_FD_TMP:?pure test requires child-fd temp directory}
    audit_dir=${EXITOS_TEST_CHILD_FD_AUDIT_DIR:?pure test requires child-fd audit directory}
    fd_auditor=${EXITOS_TEST_FD_AUDITOR:?pure test requires independent fd auditor}
    MOUNT_ID=${EXITOS_TEST_MOUNT_ID:?pure test requires retained mount ID}
    [[ $DEVICE_SYSFS_ROOT == /* && $DEVICE_SYSFS_ROOT != *$'\n'* &&
       $IRQ_PROC_ROOT == /* && $IRQ_PROC_ROOT != *$'\n'* &&
       $TMP == /* && $TMP != *$'\n'* && -d $TMP && ! -L $TMP &&
       $audit_dir == /* && $audit_dir != *$'\n'* &&
       -d $audit_dir && ! -L $audit_dir &&
       $MNT == /* && $MNT != *$'\n'* && -d $MNT && ! -L $MNT &&
       $MOUNT_ID =~ ^[1-9][0-9]*$ ]] ||
        die "invalid pure-test child-fd hygiene fixture"
    [[ $NVME_BIN == /* && -x $NVME_BIN &&
       $QDSPLIT == /* && -x $QDSPLIT &&
       $KEEPER_BIN == /* && -x $KEEPER_BIN &&
       $fd_auditor == /* && -x $fd_auditor ]] ||
        die "invalid pure-test child-fd helper"
    NOISE_SENTINEL_INTERVAL=0.01
    NOISE_SENTINEL_TRACE=1
    retain_partition
    retain_whole_device
    retain_controller_device
    retain_char_device
    verify_retained_devices hygiene-setup >/dev/null ||
        die "retained device identity failed before child-fd hygiene test"
    saved_whole_fd=$WHOLE_FD
    saved_part_fd=$PART_FD
    saved_char_fd=$CHAR_FD
    saved_ctrl_fd=$CTRL_FD

    launch_qdsplit "$TMP/app.out"
    child_rc=0
    wait "$APP" || child_rc=$?
    APP=""
    [ "$child_rc" -eq 0 ] || die "pure-test qdsplit launch failed"
    verify_retained_devices hygiene-after-qdsplit >/dev/null ||
        die "parent device fd changed after qdsplit child close"
    printf 'PARENT_DEVICE_FDS phase=after-qdsplit status=verified\n'

    launch_keeper "$CPU" "$TMP/keeper.out"
    child_rc=0
    wait "$KEEPER" || child_rc=$?
    KEEPER=""
    [ "$child_rc" -eq 0 ] || die "pure-test keeper launch failed"
    verify_retained_devices hygiene-after-keeper >/dev/null ||
        die "parent device fd changed after keeper child close"
    printf 'PARENT_DEVICE_FDS phase=after-keeper status=verified\n'

    start_cell_noise_sentinel
    (
        close_inherited_device_fds || exit 120
        exec "$fd_auditor" noise-sentinel "$NOISE_SENTINEL"
    ) || die "independent noise-sentinel fd audit failed"
    stop_cell_noise_sentinel || die "pure-test noise sentinel cleanup failed"
    verify_retained_devices hygiene-after-noise-sentinel >/dev/null ||
        die "parent device fd changed after noise-sentinel child close"
    printf 'PARENT_DEVICE_FDS phase=after-noise-sentinel status=verified\n'

    start_mount_root_holder
    stop_mount_root_holder || die "pure-test mount-root holder cleanup failed"
    verify_retained_devices hygiene-after-mount-root-holder >/dev/null ||
        die "parent device fd changed after mount-root-holder child close"
    printf 'PARENT_DEVICE_FDS phase=after-mount-root-holder status=verified\n'

    cleanup_resources || die "pure-test child-fd resource cleanup failed"
    for saved_fd in "$saved_whole_fd" "$saved_part_fd" \
                    "$saved_char_fd" "$saved_ctrl_fd"; do
        [ ! -e "/proc/$$/fd/$saved_fd" ] ||
            die "parent retained device fd survived final cleanup"
    done
    trap - EXIT INT TERM
    printf 'PARENT_DEVICE_FDS phase=cleanup status=closed\n'
    exit 0
elif [ "$MODE" = test-owned-mount-acquire ]; then
    DEVICE_TEST_MODE=1
    retain_partition
    prepare_mount_dir
    mount_test_partition
    retained_mount_is_present || die "mount identity was not retained"
    if [ "$MOUNT_WAS_EXISTING" = 1 ]; then
        cleanup_resources || die "existing-mount resource cleanup failed"
        trap - EXIT INT TERM
        printf 'MOUNT_ACQUIRE_RESULT status=ok existing=1 mount=preserved\n'
        exit 0
    fi
    cleanup_resources || die "owned mount cleanup failed"
    trap - EXIT INT TERM
    printf 'MOUNT_ACQUIRE_RESULT status=ok mount=private-namespace\n'
    exit 0
elif [ "$MODE" = test-owned-mount-cleanup ]; then
    DEVICE_TEST_MODE=1
    retain_partition
    MOUNT_ID=${EXITOS_TEST_MOUNT_ID:?pure test requires retained mount ID}
    MOUNT_SOURCE=${EXITOS_TEST_MOUNT_SOURCE:?pure test requires retained mount SOURCE}
    [[ $MOUNT_ID =~ ^[1-9][0-9]*$ && $MOUNT_SOURCE == /* &&
       $MOUNT_SOURCE != *$'\n'* ]] || die "invalid pure-test mount identity"
    MOUNT_MAJMIN=$PART_MAJMIN
    MOUNT_TARGET=$MNT
    MOUNTED_HERE=1
    start_mount_root_holder
    if ! cleanup_resources; then
        trap - EXIT INT TERM
        exit 1
    fi
    trap - EXIT INT TERM
    printf 'MOUNT_CLEANUP_RESULT status=ok mount=private-namespace\n'
    exit 0
fi

if [ "$MODE" = run ]; then
    if [ "$FULL_RUN_TEST" = 1 ]; then
        :
    else
        if [ "$PRIVATE_NAMESPACE_ENTRY" = 0 ]; then
            command -v unshare >/dev/null || die "unshare is required for private mount ownership"
            if [ "$SCREEN" = opt ]; then
                namespace_entry=--private-mount-namespace-opt-run
            else
                namespace_entry=--private-mount-namespace-run
            fi
            exec unshare --mount --propagation private -- \
                bash "$ROOT/tools/campaign-cstate-keeper.sh" "$namespace_entry"
            die "cannot enter private mount namespace"
        fi
        self_mount_ns=$(stat -Lc '%d:%i' -- /proc/self/ns/mnt) ||
            die "cannot identify private mount namespace"
        parent_mount_ns=$(stat -Lc '%d:%i' -- "/proc/$PPID/ns/mnt") ||
            die "cannot identify parent mount namespace"
        [ "$self_mount_ns" != "$parent_mount_ns" ] ||
            die "private mount-namespace entry did not create a new namespace"
        awk '
            {
                for (i = 7; i <= NF && $i != "-"; i++)
                    if ($i ~ /^(shared|master):/) exit 1
            }
        ' /proc/self/mountinfo || die "mount namespace is not recursively private"
        PRIVATE_MOUNT_NAMESPACE=1
    fi
fi

topology=$(EXITOS_CPU_SYSFS_ROOT="$SYSROOT" \
    EXITOS_ALLOWED_CPUS="${EXITOS_ALLOWED_CPUS:-}" \
    bash tools/cstate-topology.sh "$CPU")
IFS=$'\t' read -r RESOLVED_CPU SIBLING PLACEBO PLACEBO_SCOPE <<<"$topology"
[ "$RESOLVED_CPU" = "$CPU" ] || die "topology resolver returned wrong CPU"

emit_blockpoll_plan() {
    local rep layout mode keeper_cpu cell=0 offset arm_order
    printf 'PLAN cpu=%s sibling=%s placebo=%s placebo_scope=%s\n' \
        "$CPU" "$SIBLING" "$PLACEBO" "$PLACEBO_SCOPE"
    printf 'PLAN_ENV EXITOS_PAIRED=blockpoll EXITOS_ONLY=unset EXITOS_STAGE=0\n'
    printf 'PLAN_EXECUTION selected_cpu=%s sibling_cpu=%s prelaunch_windows=%s threshold_lt_pct=%s policy=SCHED_OTHER nice=-20\n' \
        "$CPU" "$SIBLING" "$CORE_UTIL_WINDOWS" "$CORE_UTIL_THRESHOLD_PCT"
    printf 'PLAN_SCREEN screen=blockpoll required_profile=%s io_size=8KiB io_bytes=8192 layouts=scattered,contiguous keeper_modes=%s reps=%s cells=%s arms_per_cell=3\n' \
        "$CPUFREQ_PROFILE_NAME" "$KEEPER_MODES_TOKEN" "$REPS" \
        "$EXPECTED_CELLS"
    printf 'PLAN_ARMS arms=0:fs-8k-qd1,18:prod-pt-8k-iopoll,19:prod-blk-8k-iopoll\n'
    for ((rep = 1; rep <= REPS; rep++)); do
        for layout in scattered contiguous; do
            for mode in "${KEEPER_MODES[@]}"; do
                cell=$((cell + 1))
                offset=$(((cell - 1) % 3))
                case "$mode" in
                  off) keeper_cpu=- ;;
                  same) keeper_cpu=$CPU ;;
                esac
                case "$offset" in
                  0) arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll ;;
                  1) arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1 ;;
                  2) arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll ;;
                esac
                printf 'CELL cell=%s rep=%s layout=%s keeper=%s keeper_cpu=%s order_offset=%s arm_order=%s\n' \
                    "$cell" "$rep" "$layout" "$mode" "$keeper_cpu" \
                    "$offset" "$arm_order"
            done
        done
    done
}

archive_blockpoll_plan() {
    local archive_mnt=${EXITOS_TESTMNT:-/mnt/exitos_fs}
    local retain_hook=${EXITOS_TEST_ARCHIVE_AFTER_PARENT_RETAIN_HOOK:-}
    local checked_hook=${EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK:-}

    command -v python3 >/dev/null ||
        die "python3 is required for safe metadata archive creation"
    if [ -n "${EXITOS_TEST_ARCHIVE_AFTER_PARENT_RETAIN_HOOK+x}" ]; then
        [[ $retain_hook == /* && -f $retain_hook && ! -L $retain_hook &&
           -x $retain_hook ]] || die "invalid pure-test archive hook"
    fi
    if [ -n "${EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK+x}" ]; then
        [[ $checked_hook == /* && -f $checked_hook && ! -L $checked_hook &&
           -x $checked_hook ]] ||
            die "invalid pure-test archive hook"
    fi
    case "${EXITOS_TEST_ARCHIVE_FAIL_AFTER_SEAL_RENAME:-}" in
      ""|1) ;;
      *) die "invalid pure-test archive seal fsync hook" ;;
    esac
    case "${EXITOS_TEST_BLOCKPOLL_PLAN_PRODUCER_FAIL:-}" in
      ""|1) ;;
      *) die "invalid pure-test blockpoll plan producer hook" ;;
    esac
    case "${EXITOS_TEST_ARCHIVE_HASH_FAIL_AFTER_PLAN:-}" in
      ""|1) ;;
      *) die "invalid pure-test archive hash hook" ;;
    esac

    python3 - "$ARCHIVE_RESULT" "$archive_mnt" "$ROOT" \
        "$retain_hook" "$checked_hook" <<'PY'
import errno
import hashlib
import os
import stat
import subprocess
import sys


def fail(message):
    print(f"cstate campaign: {message}", file=sys.stderr)
    raise SystemExit(2)


def open_absolute_dir_nofollow(path):
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    current = os.open("/", flags)
    try:
        for component in path.split("/")[1:]:
            if not component:
                continue
            try:
                next_fd = os.open(component, flags, dir_fd=current)
            except FileNotFoundError:
                fail("archive parent must already exist")
            except OSError as error:
                if error.errno in (errno.ELOOP, errno.ENOTDIR):
                    fail("archive parent must be a real canonical directory")
                raise
            os.close(current)
            current = next_fd
        return current
    except BaseException:
        os.close(current)
        raise


def retained_path(directory_fd, label):
    try:
        path = os.readlink(f"/proc/self/fd/{directory_fd}")
    except OSError:
        fail(f"cannot resolve retained {label} identity")
    if (not path.startswith("/") or path.endswith(" (deleted)") or
            "\n" in path or "\r" in path):
        fail(f"retained {label} identity is deleted or unreachable")
    return os.path.normpath(path)


def retained_mnt_id(directory_fd, label):
    values = []
    try:
        with open(
            f"/proc/self/fdinfo/{directory_fd}", "r", encoding="ascii"
        ) as stream:
            for line in stream:
                if line.startswith("mnt_id:"):
                    fields = line.split()
                    if len(fields) == 2 and fields[1].isdigit():
                        values.append(int(fields[1]))
                    else:
                        fail(f"retained {label} mnt_id is malformed")
    except OSError:
        fail(f"cannot read retained {label} mnt_id")
    if len(values) != 1 or values[0] <= 0:
        fail(f"retained {label} mnt_id is not unique")
    return values[0]


def entry_exists(directory_fd, name):
    try:
        os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError:
        return False
    return True


def sha256_descriptor(descriptor):
    digest = hashlib.sha256()
    while True:
        block = os.read(descriptor, 1024 * 1024)
        if not block:
            return digest.hexdigest()
        digest.update(block)


def sha256_at(directory_fd, name):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    descriptor = os.open(name, flags, dir_fd=directory_fd)
    try:
        identity = os.fstat(descriptor)
        if not stat.S_ISREG(identity.st_mode):
            fail("archive hash source is not a regular file")
        return sha256_descriptor(descriptor)
    finally:
        os.close(descriptor)


def sha256_beneath(root_fd, relative_path):
    components = relative_path.split("/")
    current = os.dup(root_fd)
    try:
        directory_flags = (
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        )
        for component in components[:-1]:
            next_fd = os.open(component, directory_flags, dir_fd=current)
            os.close(current)
            current = next_fd
        return sha256_at(current, components[-1])
    finally:
        os.close(current)


def write_exclusive(directory_fd, name, contents):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
    descriptor = os.open(name, flags, 0o600, dir_fd=directory_fd)
    try:
        view = memoryview(contents)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("short archive metadata write")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


result, test_mount, root, retain_hook, checked_hook = sys.argv[1:]
if (not result.startswith("/") or result == "/" or result.endswith("/") or
        any(character.isspace() or character in '\\\"' for character in result)):
    fail("archive result directory must be an absolute safe path")
result_components = result.split("/")[1:]
if any(component in ("", ".", "..") for component in result_components):
    fail("archive result directory must be an absolute safe path")
if not test_mount.startswith("/"):
    fail("EXITOS_TESTMNT must be an absolute path")

producer = subprocess.run(
    ["/bin/bash", os.path.join(root, "tools/campaign-cstate-keeper.sh"),
     "--blockpoll-plan"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
)
if producer.returncode != 0 or producer.stderr:
    if producer.stderr:
        sys.stderr.buffer.write(producer.stderr)
        sys.stderr.buffer.flush()
    if producer.returncode != 0:
        fail(f"blockpoll plan producer failed with status {producer.returncode}")
    fail("blockpoll plan producer emitted stderr")
plan = producer.stdout

parent = os.path.dirname(result) or "/"
basename = os.path.basename(result)
parent_fd = open_absolute_dir_nofollow(parent)
child_fd = None
mount_fd = None
root_fd = None
root_mount_fd = None
try:
    parent_identity = os.fstat(parent_fd)
    if parent_identity.st_uid != os.getuid():
        fail("archive parent is not owned by the current uid")
    if parent_identity.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        fail("archive parent permissions are unsafe")

    parent_canonical = retained_path(parent_fd, "archive parent")
    try:
        mount_fd = os.open(
            test_mount,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC,
        )
    except FileNotFoundError:
        mount_canonical = os.path.realpath(test_mount)
    except OSError:
        fail("EXITOS_TESTMNT is not a readable directory")
    else:
        mount_canonical = retained_path(mount_fd, "EXITOS_TESTMNT")

    root_mount_fd = open_absolute_dir_nofollow("/")
    root_mnt_id = retained_mnt_id(root_mount_fd, "root mount")
    mount_is_nonroot = False
    if mount_fd is not None:
        mount_mnt_id = retained_mnt_id(mount_fd, "EXITOS_TESTMNT")
        mount_is_nonroot = mount_mnt_id != root_mnt_id
    root_fd = open_absolute_dir_nofollow(root)

    if retain_hook:
        completed = subprocess.run(
            [retain_hook, parent, result], check=False
        )
        if completed.returncode != 0:
            fail("pure-test archive hook failed")

    candidate_canonical = os.path.join(parent_canonical, basename)
    try:
        inside_mount = os.path.commonpath(
            (candidate_canonical, mount_canonical)
        ) == mount_canonical
    except ValueError:
        inside_mount = False
    if inside_mount:
        fail("archive result must not be inside EXITOS_TESTMNT")
    if (mount_is_nonroot and
            parent_identity.st_dev == os.fstat(mount_fd).st_dev):
        fail("archive result must not share the EXITOS_TESTMNT backing device")

    checked_fd = open_absolute_dir_nofollow(parent)
    try:
        checked_identity = os.fstat(checked_fd)
        if ((checked_identity.st_dev, checked_identity.st_ino) !=
                (parent_identity.st_dev, parent_identity.st_ino)):
            fail("archive parent identity changed after validation")
    finally:
        os.close(checked_fd)
    if entry_exists(parent_fd, basename):
        fail("archive result directory must not already exist")

    if checked_hook:
        completed = subprocess.run([checked_hook, parent, result], check=False)
        if completed.returncode != 0:
            fail("pure-test archive hook failed")
        checked_fd = open_absolute_dir_nofollow(parent)
        try:
            checked_identity = os.fstat(checked_fd)
            if ((checked_identity.st_dev, checked_identity.st_ino) !=
                    (parent_identity.st_dev, parent_identity.st_ino)):
                fail("archive parent identity changed after validation")
        finally:
            os.close(checked_fd)

    old_umask = os.umask(0o077)
    try:
        try:
            os.mkdir(basename, 0o700, dir_fd=parent_fd)
        except FileExistsError:
            fail("archive result directory must not already exist")
    finally:
        os.umask(old_umask)

    child_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    child_fd = os.open(basename, child_flags, dir_fd=parent_fd)
    child_identity = os.fstat(child_fd)
    if (child_identity.st_uid != os.getuid() or
            stat.S_IMODE(child_identity.st_mode) != 0o700):
        fail("archive result directory identity or permissions are unsafe")

    write_exclusive(child_fd, "plan.txt", plan)
    plan_sha = sha256_at(child_fd, "plan.txt")
    if os.environ.get("EXITOS_TEST_ARCHIVE_HASH_FAIL_AFTER_PLAN") == "1":
        fail("injected archive hash failure after plan")

    sources = (
        ("tools/campaign-cstate-keeper.sh", "tools/campaign-cstate-keeper.sh"),
        ("attribution/qdsplit.c", "attribution/qdsplit.c"),
        ("src/iopath.c", "src/iopath.c"),
        ("src/intercept.c", "src/intercept.c"),
        ("tools/README.md", "tools/README.md"),
    )
    source_hashes = [
        (label, sha256_beneath(root_fd, path)) for label, path in sources
    ]
    manifest_lines = [
        "format=exitos-cstate-blockpoll-plan-archive",
        "version=1",
        "mode=metadata-only",
        f"plan_sha256={plan_sha}",
    ]
    manifest_lines.extend(
        f"source_sha256 {label}={digest}" for label, digest in source_hashes
    )
    manifest = ("\n".join(manifest_lines) + "\n").encode("ascii")
    write_exclusive(child_fd, "manifest.txt", manifest)
    manifest_sha = sha256_at(child_fd, "manifest.txt")

    seal = ("\n".join((
        "format=exitos-cstate-blockpoll-plan-archive",
        "version=1",
        "mode=metadata-only",
        f"plan_sha256={plan_sha}",
        f"manifest_sha256={manifest_sha}",
        "status=complete",
    )) + "\n").encode("ascii")
    temporary_seal = ".ARCHIVE_COMPLETE.tmp"
    published_seal = False
    try:
        write_exclusive(child_fd, temporary_seal, seal)
        os.rename(
            temporary_seal, "ARCHIVE_COMPLETE",
            src_dir_fd=child_fd, dst_dir_fd=child_fd,
        )
        published_seal = True
        if os.environ.get("EXITOS_TEST_ARCHIVE_FAIL_AFTER_SEAL_RENAME") == "1":
            raise OSError("injected archive seal directory fsync failure")
        os.fsync(child_fd)
    except BaseException as error:
        try:
            os.unlink(
                "ARCHIVE_COMPLETE" if published_seal else temporary_seal,
                dir_fd=child_fd,
            )
        except FileNotFoundError:
            pass
        try:
            os.fsync(child_fd)
        except OSError:
            pass
        if str(error) == "injected archive seal directory fsync failure":
            fail(str(error))
        raise

    sys.stdout.buffer.write(plan)
    sys.stdout.buffer.flush()
finally:
    if child_fd is not None:
        os.close(child_fd)
    if root_fd is not None:
        os.close(root_fd)
    if root_mount_fd is not None:
        os.close(root_mount_fd)
    if mount_fd is not None:
        os.close(mount_fd)
    os.close(parent_fd)
PY
}

emit_plan() {
    local rep buffer mode keeper_cpu
    printf 'PLAN cpu=%s sibling=%s placebo=%s placebo_scope=%s\n' \
        "$CPU" "$SIBLING" "$PLACEBO" "$PLACEBO_SCOPE"
    printf 'PLAN_ENV EXITOS_ONLY=unset\n'
    printf 'PLAN_EXECUTION selected_cpu=%s sibling_cpu=%s prelaunch_windows=%s threshold_lt_pct=%s policy=SCHED_OTHER nice=-20\n' \
        "$CPU" "$SIBLING" "$CORE_UTIL_WINDOWS" "$CORE_UTIL_THRESHOLD_PCT"
    if [ "$SCREEN" = opt ]; then
        printf 'PLAN_SCREEN screen=opt required_profile=%s keeper_modes=%s cells=%s\n' \
            "$CPUFREQ_PROFILE_NAME" "$KEEPER_MODES_TOKEN" "$EXPECTED_CELLS"
        printf 'PLAN_ARMS EXITOS_PAIRED=opt arms=0:fs-8k-qd1,1:pt-8k-qd1,15:pt-8k-fixedbuf,16:pt-8k-bounce,17:pt-8k-bounce-fixed\n'
    else
        printf 'PLAN_SCREEN required_profile=%s keeper_modes=%s cells=%s\n' \
            "$CPUFREQ_PROFILE_NAME" "$KEEPER_MODES_TOKEN" "$EXPECTED_CELLS"
    fi
    for ((rep = 1; rep <= REPS; rep++)); do
        for buffer in normal contiguous; do
            for mode in "${KEEPER_MODES[@]}"; do
                case "$mode" in
                  off) keeper_cpu=- ;;
                  same) keeper_cpu=$CPU ;;
                esac
                printf 'CELL rep=%s buffer=%s keeper=%s keeper_cpu=%s\n' \
                    "$rep" "$buffer" "$mode" "$keeper_cpu"
            done
        done
    done
}

if [ "$MODE" = plan ]; then
    if [ "$SCREEN" = blockpoll ]; then
        if [ "${EXITOS_TEST_BLOCKPOLL_PLAN_PRODUCER_FAIL:-}" = 1 ]; then
            printf 'PLAN_PARTIAL injected=yes\n'
            echo "cstate campaign: injected blockpoll plan producer failure" >&2
            exit 96
        fi
        emit_blockpoll_plan
    else
        emit_plan
    fi
    exit 0
elif [ "$MODE" = archive-plan ]; then
    archive_blockpoll_plan
    exit 0
fi

retain_cpufreq_profile
verify_cpufreq_profile setup ||
    die "required $CPUFREQ_PROFILE_NAME cpufreq profile failed setup readback"

[ -x "$QDSPLIT" ] || die "build $QDSPLIT first"
[ -x "$KEEPER_BIN" ] || die "build $KEEPER_BIN first"
command -v shuf >/dev/null || die "shuf is required for randomized cells"
command -v nice >/dev/null || die "nice is required for bounded measurement priority"
command -v taskset >/dev/null || die "taskset is required for measurement affinity"
if [ "$FULL_RUN_TEST" = 1 ]; then
    [[ $DEVICE_SYSFS_ROOT == /* && $DEVICE_SYSFS_ROOT != *$'\n'* ]] ||
        die "invalid pure-test sysfs root"
    [[ $IRQ_PROC_ROOT == /* && $IRQ_PROC_ROOT != *$'\n'* &&
       -d $IRQ_PROC_ROOT && ! -L $IRQ_PROC_ROOT ]] ||
        die "invalid pure-test proc IRQ root"
    [[ $NVME_BIN == /* && -x $NVME_BIN ]] ||
        die "invalid pure-test nvme NSID helper"
else
    [ -r "$TESTDISK" ] || die "missing test-disk resolver $TESTDISK"
    [ -z "${EXITOS_DEVICE_SYSFS_ROOT+x}" ] ||
        die "device sysfs root override is pure-test-only"
    [ -z "${EXITOS_IRQ_PROC_ROOT+x}" ] ||
        die "proc IRQ root override is pure-test-only"
    [ -z "${EXITOS_TEST_NVME_BIN+x}" ] ||
        die "nvme helper override is pure-test-only"
    [ -z "${EXITOS_TEST_MOUNT_FD_HELPER+x}" ] ||
        die "mount-fd helper override is pure-test-only"
    [ -z "${EXITOS_TEST_MOUNT_FD_ID_FILE+x}" ] ||
        die "mount-fd mnt_id override is pure-test-only"
    [ -z "${EXITOS_TEST_REPLACE_MOUNT_HOOK+x}" ] ||
        die "mount replacement hook is pure-test-only"
    resolve_testdisk
fi
: "${EXITOS_CHARDEV:?serial resolver found no matching NVMe generic character device}"
retain_partition
retain_whole_device
retain_controller_device
retain_char_device
verify_retained_devices setup || die "retained device identity failed during setup"
prepare_mount_dir
mount_test_partition

verify_retained_mount workdir-pre ||
    die "retained mount identity failed before work-directory creation"
prepare_run_dir
verify_retained_mount workdir-post ||
    die "retained mount identity failed after work-directory creation"
prepare_output
TMP=$(mktemp -d "${TMPDIR:-/tmp}/exitos-cstate-campaign.XXXXXX") ||
    die "cannot create private campaign state directory"

export EXITOS_SAMEFILE=1 EXITOS_LBA_START=0 EXITOS_LBA_COUNT=0
export EXITOS_PAUSE=4
if [ "$SCREEN" = opt ]; then
    export EXITOS_PAIRED=opt
    header=$'rep\tbuffer\tkeeper\tkeeper_cpu\tfs_median_us\tpt_median_us\tpt_fixedbuf_median_us\tpt_bounce_median_us\tpt_bounce_fixed_median_us\tGUP_saved_us\tstaged_saved_us\tbounce_net_us\tbuffer_layout\tselected_cpu\tsibling_cpu\thctx\tqid\tirq\ttarget_irq_delta\tselected_non_target_irq_delta\tsibling_non_target_irq_delta\tcpufreq_profile\texecution_contract\tcore_util_contract'
    metadata="# campaign=cstate-opt required_profile=$CPUFREQ_PROFILE_NAME expected_cells=$EXPECTED_CELLS"
else
    export EXITOS_PAIRED=8k
    header=$'rep\tbuffer\tkeeper\tkeeper_cpu\tfs_median_us\tpt_median_us\tpt_minus_fs_us\tbuffer_layout\tselected_cpu\tsibling_cpu\thctx\tqid\tirq\ttarget_irq_delta\tselected_non_target_irq_delta\tsibling_non_target_irq_delta\tcpufreq_profile\texecution_contract\tcore_util_contract'
    metadata="# campaign=cstate-primary required_profile=$CPUFREQ_PROFILE_NAME expected_cells=$EXPECTED_CELLS"
fi
printf '%s\n' "$metadata"
printf '%s\n' "$metadata" >&3
printf '%s\n' "$header"
printf '%s\n' "$header" >&3
for ((rep = 1; rep <= REPS; rep++)); do
    for buffer in normal contiguous; do
        for mode in "${KEEPER_MODES[@]}"; do
            printf '%s\t%s\t%s\n' "$rep" "$buffer" "$mode"
        done
    done
done | shuf >"$TMP/schedule"

while IFS=$'\t' read -r rep buffer mode; do
    cell_phase="cell-${rep}-${buffer}-${mode}"
    CURRENT_FILE="$RUN_DIR/cell_${rep}_${buffer}_${mode}.dat"
    export EXITOS_FSFILE="$CURRENT_FILE"
    configure_buffer_env "$buffer"
    sleep "$SETTLE"
    : >"$TMP/app.out"
    : >"$TMP/keeper.out"
    verify_cpufreq_profile "${cell_phase}-pre" ||
        die "cpufreq profile failed before $cell_phase"
    verify_retained_devices "${cell_phase}-pre" ||
        die "retained device identity failed before $cell_phase"
    verify_retained_mount "${cell_phase}-pre" ||
        die "retained mount identity failed before $cell_phase"
    resolve_irq_binding ||
        die "cannot resolve current CPU-to-IRQ binding before $cell_phase"
    cell_binding_token=$RESOLVED_BINDING_TOKEN
    cell_hctx=$RESOLVED_HCTX
    cell_qid=$RESOLVED_QID
    cell_irq=$RESOLVED_IRQ

    verify_target_core_utilization "${cell_phase}-prelaunch" ||
        die "target-core utilization gate failed before $cell_phase"
    start_cell_noise_sentinel
    launch_qdsplit "$TMP/app.out"
    ready=0
    for ((attempt = 0; attempt < 300; attempt++)); do
        if grep -q '^READY$' "$TMP/app.out"; then ready=1; break; fi
        kill -0 "$APP" 2>/dev/null || break
        sleep 0.1
    done
    if [ "$ready" != 1 ]; then
        echo "cell $rep/$buffer/$mode failed before READY" >&2
        sed -n '1,100p' "$TMP/app.out" >&2
        exit 1
    fi
    verify_app_execution_contract "$APP" "${cell_phase}-ready" ||
        die "qdsplit execution contract failed at READY for $cell_phase"

    keeper_cpu=-
    case "$mode" in
      same) keeper_cpu=$CPU ;;
    esac
    if [ "$mode" != off ]; then
        launch_keeper "$keeper_cpu" "$TMP/keeper.out"
        kready=0
        for ((attempt = 0; attempt < 100; attempt++)); do
            if grep -q "^READY cpu=$keeper_cpu policy=SCHED_IDLE$" "$TMP/keeper.out"; then
                kready=1
                break
            fi
            kill -0 "$KEEPER" 2>/dev/null || break
            sleep 0.01
        done
        [ "$kready" = 1 ] || {
            echo "keeper failed: $(tr '\n' ' ' < "$TMP/keeper.out")" >&2
            exit 1
        }
    fi

    resolve_irq_binding ||
        die "cannot revalidate current CPU-to-IRQ binding at READY for $cell_phase"
    [ "$RESOLVED_BINDING_TOKEN" = "$cell_binding_token" ] ||
        die "IRQ binding changed before measurement in $cell_phase"
    capture_irq_counters ||
        die "cannot capture pre-measurement IRQ counters for $cell_phase"
    pre_target_selected=$IRQ_TARGET_SELECTED
    pre_target_total=$IRQ_TARGET_TOTAL
    pre_selected_non_target=$IRQ_SELECTED_NON_TARGET
    pre_sibling_non_target=$IRQ_SIBLING_NON_TARGET

    app_rc=0
    wait "$APP" || app_rc=$?
    APP=""
    stop_cell_noise_sentinel
    capture_irq_counters ||
        die "cannot capture post-measurement IRQ counters for $cell_phase"
    post_target_selected=$IRQ_TARGET_SELECTED
    post_target_total=$IRQ_TARGET_TOTAL
    post_selected_non_target=$IRQ_SELECTED_NON_TARGET
    post_sibling_non_target=$IRQ_SIBLING_NON_TARGET
    verify_cpufreq_profile "${cell_phase}-post" ||
        die "cpufreq profile failed after $cell_phase"
    verify_retained_devices "${cell_phase}-post" ||
        die "retained device identity failed after $cell_phase"
    verify_retained_mount "${cell_phase}-post" ||
        die "retained mount identity failed after $cell_phase"
    [ "$app_rc" -eq 0 ] || die "qdsplit failed in $cell_phase with status $app_rc"
    resolve_irq_binding ||
        die "cannot revalidate current CPU-to-IRQ binding after $cell_phase"
    [ "$RESOLVED_BINDING_TOKEN" = "$cell_binding_token" ] ||
        die "IRQ binding changed during $cell_phase"
    (( post_target_selected >= pre_target_selected &&
       post_target_total >= pre_target_total &&
       post_selected_non_target >= pre_selected_non_target &&
       post_sibling_non_target >= pre_sibling_non_target )) ||
        die "IRQ counters decreased during $cell_phase"
    target_irq_delta=$((post_target_total - pre_target_total))
    target_selected_delta=$((post_target_selected - pre_target_selected))
    selected_non_target_delta=$((post_selected_non_target - pre_selected_non_target))
    sibling_non_target_delta=$((post_sibling_non_target - pre_sibling_non_target))
    [ "$target_irq_delta" -gt 0 ] ||
        die "target IRQ delta is not positive in $cell_phase"
    [ "$target_selected_delta" -eq "$target_irq_delta" ] ||
        die "target IRQ was delivered outside selected CPU during $cell_phase"
    if [ -n "$KEEPER" ]; then
        kill -TERM "$KEEPER" 2>/dev/null || true
        wait "$KEEPER"
        KEEPER=""
    fi
    if [ "$CELL_NOISE_DETECTED" = 1 ]; then
        PRESERVE_APP_OUTPUT=1
        echo "cstate campaign: noisy cell app output preserved: $TMP/app.out" >&2
        APP_OUTPUT_PRESERVE_REPORTED=1
        die "cell noise sentinel rejected $cell_phase: $CELL_NOISE_REASON"
    fi

    case "$buffer" in
      normal)
        grep -Eq '^buffer contract: scattered proven runs=[2-9][0-9]*$' "$TMP/app.out" ||
            die "qdsplit did not prove the scattered-buffer contract"
        ;;
      contiguous)
        grep -q '^buffer contract: contiguous proven runs=1$' "$TMP/app.out" ||
            die "qdsplit did not prove the contiguous-buffer contract"
        ;;
    esac
    layout=$(buffer_layout_from_output "$TMP/app.out") ||
        die "qdsplit did not emit one unambiguous submitted-span PFN proof"
    validate_buffer_layout "$buffer" "$layout" ||
        die "qdsplit PFN proof does not match the 8 KiB $buffer contract"
    if [ "$SCREEN" = opt ]; then
        if grep -Eq '(^|[[:space:]])FAILED($|[[:space:]])' "$TMP/app.out"; then
            die "qdsplit emitted FAILED in $cell_phase"
        fi
        verify_opt_post_run_record "$TMP/app.out" ||
            die "qdsplit opt post-run readback record is missing or invalid in $cell_phase"
        strict_opt_median_from_output "$TMP/app.out" fs-8k-qd1 fs ||
            die "invalid fs-8k-qd1 timing row in $cell_phase"
        strict_opt_median_from_output "$TMP/app.out" pt-8k-qd1 pt ||
            die "invalid pt-8k-qd1 timing row in $cell_phase"
        strict_opt_median_from_output "$TMP/app.out" pt-8k-fixedbuf pt_fixed ||
            die "invalid pt-8k-fixedbuf timing row in $cell_phase"
        strict_opt_median_from_output "$TMP/app.out" pt-8k-bounce pt_bounce ||
            die "invalid pt-8k-bounce timing row in $cell_phase"
        strict_opt_median_from_output "$TMP/app.out" pt-8k-bounce-fixed pt_bounce_fixed ||
            die "invalid pt-8k-bounce-fixed timing row in $cell_phase"
        gup_saved=$(LC_ALL=C awk -v p="$pt" -v f="$pt_fixed" \
            'BEGIN { printf "%.3f", p-f }')
        staged_saved=$(LC_ALL=C awk -v b="$pt_bounce" -v f="$pt_bounce_fixed" \
            'BEGIN { printf "%.3f", b-f }')
        bounce_net=$(LC_ALL=C awk -v p="$pt" -v b="$pt_bounce" \
            'BEGIN { printf "%.3f", p-b }')
        row=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
            "$rep" "$buffer" "$mode" "$keeper_cpu" "$fs" "$pt" \
            "$pt_fixed" "$pt_bounce" "$pt_bounce_fixed" "$gup_saved" \
            "$staged_saved" "$bounce_net" "$layout" "$CPU" "$SIBLING" \
            "$cell_hctx" "$cell_qid" "$cell_irq" "$target_irq_delta" \
            "$selected_non_target_delta" "$sibling_non_target_delta" \
            "$CPUFREQ_PROFILE_TOKEN" "$APP_EXECUTION_TOKEN" "$CORE_UTIL_TOKEN")
    else
        fs=$(awk '$1=="fs-8k-qd1" {print $3}' "$TMP/app.out")
        pt=$(awk '$1=="pt-8k-qd1" {print $3}' "$TMP/app.out")
        [ -n "$fs" ] && [ -n "$pt" ] || {
            echo "missing timing row for $rep/$buffer/$mode" >&2
            sed -n '1,120p' "$TMP/app.out" >&2
            exit 1
        }
        delta=$(awk -v p="$pt" -v f="$fs" 'BEGIN { printf "%.3f", p-f }')
        row=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
            "$rep" "$buffer" "$mode" "$keeper_cpu" "$fs" "$pt" "$delta" "$layout" \
            "$CPU" "$SIBLING" "$cell_hctx" "$cell_qid" "$cell_irq" \
            "$target_irq_delta" "$selected_non_target_delta" "$sibling_non_target_delta" \
            "$CPUFREQ_PROFILE_TOKEN" "$APP_EXECUTION_TOKEN" "$CORE_UTIL_TOKEN")
    fi
    printf '%s\n' "$row"
    printf '%s\n' "$row" >&3
    idle=$(sed -n '/^idle states on cpu/,/^$/p' "$TMP/app.out" | sed 's/^/# /')
    if [ -n "$idle" ]; then
        printf '%s\n' "$idle"
        printf '%s\n' "$idle" >&3
    fi
    CURRENT_FILE=""
done <"$TMP/schedule"

if [ "$(stat -Lc '%d:%i:%u' -- "$OUT" 2>/dev/null || true)" != "$OUTPUT_ID" ]; then
    die "OUT pathname no longer names the retained result inode"
fi
if [ "$FULL_RUN_TEST" = 1 ] &&
   [ "${EXITOS_TEST_FAILPOINT:-}" = cleanup:replacement ]; then
    replace_hook=${EXITOS_TEST_REPLACE_MOUNT_HOOK:?pure test requires replacement hook}
    [[ $replace_hook == /* && -x $replace_hook ]] ||
        die "invalid pure-test mount replacement hook"
    "$replace_hook"
fi
cleanup_resources || die "campaign resource cleanup failed"
trap - EXIT INT TERM
echo "results: $OUT"
