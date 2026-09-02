#!/bin/bash
# Reproducible QD1/thread-scaling pilot for the one authorized NVMe target.
# This runner NEVER creates a partition, formats, mounts, unmounts, rebinds, or
# changes polling/IRQ/CPU-frequency state.  It only overwrites owned w*.log
# files after proving the exact current disk, partition, and filesystem
# generation.  Full fio JSON, stderr, environment metadata, and per-process
# interception counters are retained for every cell.
set -euo pipefail

SERIAL=EXAMPLESERIAL0001
WWID=eui.00000000000000000000000000000000
PCI=0000:01:00.0
UUID=00000000-0000-0000-0000-000000000000
start=2048
count=419430400
MNT=/mnt/exitos_fs
REPO=$(cd "$(dirname "$0")/.." && pwd -P)
MAX_JOBS=8
FILE_MIB=${EXITOS_SCALE_FILE_MIB:-128}
RUNTIME=${EXITOS_SCALE_RUNTIME:-10}
# Eight cores, one per physical core, all on the NUMA node the target
# controller is attached to.  The list below is a placeholder: derive your own
# from read-only topology, idle and IRQ sampling on the machine you are
# measuring, and set EXITOS_SCALE_CPU_POOL to it.  Preserve the order for every
# arm; never manufacture a contiguous range without checking it, because a
# range can silently include SMT siblings or a busy IRQ target.  A controller
# reprobe invalidates whatever map was derived.
CPU_POOL=${EXITOS_SCALE_CPU_POOL:-8,9,10,11,12,13,14,15}
QUIET_SAMPLES=${EXITOS_SCALE_QUIET_SAMPLES:-21}
QUIET_INTERVAL=${EXITOS_SCALE_QUIET_INTERVAL:-30}
IDLE_INTERVAL=${EXITOS_SCALE_IDLE_INTERVAL:-2}
CPU_UTIL_WINDOW_MS=${EXITOS_SCALE_CPU_UTIL_WINDOW_MS:-5000}
CPU_UTIL_MAX=${EXITOS_SCALE_CPU_UTIL_MAX:-15.0}
NOISE_POLL_MS=${EXITOS_SCALE_NOISE_POLL_MS:-500}
MAX_NOISE_BLIND_MS=1000
IO_NICE=-20
RESULT_ROOT_EXPLICIT=${EXITOS_SCALE_RESULT_ROOT+x}
RESULT_ROOT=${EXITOS_SCALE_RESULT_ROOT-}
VARIANTS_DIR=${EXITOS_SCALE_VARIANTS_DIR:-}
LOCK_ROOT=/run
CPUFREQ_TARGET_KHZ=3800000
CPUFREQ_SYSROOT=/sys/devices/system/cpu
CPUFREQ_LOCK_ROOT=/run/lock

CPUFREQ_ATTEST_FD=
CPUFREQ_ATTEST_TOKEN=
CPUFREQ_ATTESTATION_SHA256=
CPUFREQ_LIVE_PROFILE_SHA256=
CPUFREQ_ATTESTATION_IDENTITY=
CPUFREQ_REQUESTED_CPUS=
CPUFREQ_CANONICAL_SELECTED_CPUS=
CPUFREQ_LOGICAL_CPUS=
CPUFREQ_POLICY_COUNT=

CELL_IO_PID=
CELL_IO_PGID=
CELL_SENTINEL_PID=
CELL_SENTINEL_WAIT_PID=
CELL_SENTINEL_RC=
CELL_SENTINEL_LOG=
CELL_SENTINEL_STOP_TOKEN=

CELL_EVIDENCE_ACTIVE=0
CELL_EVIDENCE_DIR=
CELL_EVIDENCE_DIR_FD=
CELL_EVIDENCE_FD_PATH=
CELL_EVIDENCE_IDENTITIES=
CELL_EVIDENCE_PROC_ROOT=/proc
CELL_EVIDENCE_SYS_ROOT=/sys
CELL_EVIDENCE_CPU_ROOT=/sys/devices/system/cpu
CELL_EVIDENCE_CONTROLLER_ROOT=
CELL_EVIDENCE_BASE=
CELL_EVIDENCE_CTRL=
CELL_EVIDENCE_PCI=
CELL_EVIDENCE_CHAR_DEV_T=
CELL_EVIDENCE_MAP=
CELL_SUMMARY_VALUE=

die() { echo "FIO_SCALE_REFUSE $*" >&2; exit 3; }

cell_evidence_python()
{
    local operation=$1
    shift
    python3 - "$operation" "$CELL_EVIDENCE_FD_PATH" "$CELL_EVIDENCE_DIR" \
        "$CELL_EVIDENCE_IDENTITIES" "$CELL_EVIDENCE_PROC_ROOT" \
        "$CELL_EVIDENCE_SYS_ROOT" "$CELL_EVIDENCE_CPU_ROOT" \
        "$CELL_EVIDENCE_CONTROLLER_ROOT" "$CELL_EVIDENCE_BASE" \
        "$CELL_EVIDENCE_CTRL" "$CELL_EVIDENCE_PCI" \
        "$CELL_EVIDENCE_CHAR_DEV_T" "$CELL_EVIDENCE_MAP" \
        "$CPU_UTIL_MAX" "$@" <<'PY'
import glob
import os
import re
import stat
import sys
import time

(operation, retained_path, original_path, identities_text, proc_root,
 sys_root, cpu_root, controller_root, base, controller, expected_pci,
 char_dev_t, selected_text, util_reference, *extra) = sys.argv[1:]
names = ("diskstats-pre.txt", "diskstats-post.txt", "poll-pre.txt",
         "poll-post.txt", "cell-summary.txt", "cell-result.txt")
owner = os.geteuid()

def refuse(message):
    raise SystemExit(message)

def read_one(path, label):
    try:
        with open(path, "r", encoding="ascii") as source:
            value = source.read()
    except (OSError, UnicodeError) as exc:
        refuse("%s_read_failed=%s" % (label, exc))
    if not value.endswith("\n") or "\x00" in value:
        refuse("%s_malformed" % label)
    return value.rstrip("\n")

def parse_cpu_list(value, label):
    cpus = set()
    if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?(?:,[0-9]+(?:-[0-9]+)?)*", value):
        refuse("%s_malformed=%s" % (label, value))
    for part in value.split(","):
        if "-" in part:
            low, high = map(int, part.split("-", 1))
            if high < low:
                refuse("%s_malformed=%s" % (label, value))
            cpus.update(range(low, high + 1))
        else:
            cpus.add(int(part))
    return cpus

def canonical_cpu_list(cpus):
    return ",".join(str(cpu) for cpu in sorted(cpus))

def verify_directory(directory):
    dst = os.fstat(directory)
    try:
        pst = os.stat(original_path, follow_symlinks=False)
    except OSError:
        refuse("evidence_directory_identity_changed")
    if (not stat.S_ISDIR(dst.st_mode) or not stat.S_ISDIR(pst.st_mode) or
            dst.st_uid != owner or pst.st_uid != owner or
            stat.S_IMODE(dst.st_mode) != 0o700 or
            (dst.st_dev, dst.st_ino) != (pst.st_dev, pst.st_ino)):
        refuse("evidence_directory_identity_changed")

def decode_identities():
    result = {}
    if not identities_text:
        return result
    for token in identities_text.split(","):
        match = re.fullmatch(r"([^:]+):([0-9]+):([0-9]+)", token)
        if not match:
            refuse("evidence_identity_token_malformed")
        result[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    if set(result) != set(names):
        refuse("evidence_identity_set_malformed")
    return result

def open_verified(directory, name, flags=os.O_RDWR):
    expected = decode_identities()[name]
    try:
        fd = os.open(name, flags | os.O_NOFOLLOW | os.O_CLOEXEC,
                     dir_fd=directory)
    except OSError:
        refuse("evidence_leaf_open_failed=%s" % name)
    fst = os.fstat(fd)
    try:
        pst = os.stat(name, dir_fd=directory, follow_symlinks=False)
    except OSError:
        os.close(fd)
        refuse("evidence_leaf_identity_changed=%s" % name)
    if (not stat.S_ISREG(fst.st_mode) or not stat.S_ISREG(pst.st_mode) or
            fst.st_uid != owner or pst.st_uid != owner or
            fst.st_nlink != 1 or pst.st_nlink != 1 or
            (fst.st_dev, fst.st_ino) != expected or
            (pst.st_dev, pst.st_ino) != expected):
        os.close(fd)
        refuse("evidence_leaf_identity_changed=%s" % name)
    return fd

def write_all(fd, payload):
    view = memoryview(payload.encode("ascii", "strict"))
    while view:
        count = os.write(fd, view)
        if count <= 0:
            refuse("evidence_short_write")
        view = view[count:]
    os.fsync(fd)

def disk_snapshot(phase):
    timestamp = time.monotonic_ns()
    boot_id = read_one(os.path.join(proc_root, "sys/kernel/random/boot_id"),
                       "boot_id").strip()
    diskseq = read_one(os.path.join(sys_root, "block", base, "diskseq"),
                       "diskseq").strip()
    dev_t = read_one(os.path.join(sys_root, "block", base, "dev"),
                     "whole_dev_t").strip()
    raw_text = read_one(os.path.join(proc_root, "diskstats"),
                        "diskstats")
    matches = []
    for line in raw_text.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[2] == base:
            matches.append((line.strip(), fields))
    error = None
    raw_line = matches[0][0] if matches else ""
    fields = matches[0][1] if matches else []
    raw = fields[3:] if len(fields) >= 3 else []
    if len(matches) != 1:
        error = "diskstats_row_count=%d" % len(matches)
    elif not re.fullmatch(r"[0-9]+:[0-9]+", dev_t):
        error = "whole_dev_t_malformed"
    elif fields[0] + ":" + fields[1] != dev_t:
        error = "diskstats_dev_t_mismatch"
    elif len(raw) < 17:
        error = "diskstats_fields_short=%d" % len(raw)
    elif any(not re.fullmatch(r"[0-9]+", value) for value in raw):
        error = "diskstats_counter_malformed"
    elif not re.fullmatch(r"[0-9]+", diskseq):
        error = "diskseq_malformed"
    lines = ["format=exitos-cell-diskstats-v1", "phase=" + phase,
             "timestamp_monotonic_ns=%d" % timestamp,
             "boot_id=" + boot_id, "diskseq=" + diskseq,
             "dev_t=" + dev_t, "device=" + base,
             "raw_fields=%d" % len(raw), "raw=" + " ".join(raw),
             "raw_line=" + raw_line]
    if error:
        lines.append("capture_error=" + error)
    return "\n".join(lines) + "\n", {
        "timestamp": timestamp, "boot_id": boot_id, "diskseq": diskseq,
        "dev_t": dev_t, "device": base, "raw": raw, "error": error}

def controller_identity():
    if os.path.basename(os.path.normpath(controller_root)) != controller:
        refuse("controller_name_drift")
    device_link = os.path.join(controller_root, "device")
    try:
        resolved = os.path.realpath(device_link)
    except OSError:
        refuse("controller_identity_read_failed")
    if not os.path.exists(resolved) or os.path.basename(resolved) != expected_pci:
        refuse("controller_identity_drift")
    current_char_dev_t = read_one(os.path.join(controller_root, "dev"),
                                  "controller_dev_t").strip()
    if current_char_dev_t != char_dev_t:
        refuse("controller_identity_drift")
    return "%s|%s|%s" % (controller, expected_pci, current_char_dev_t)

def poll_snapshot(phase):
    timestamp = time.monotonic_ns()
    boot_id = read_one(os.path.join(proc_root, "sys/kernel/random/boot_id"),
                       "boot_id").strip()
    diskseq = read_one(os.path.join(sys_root, "block", base, "diskseq"),
                       "diskseq").strip()
    dev_t = read_one(os.path.join(sys_root, "block", base, "dev"),
                     "whole_dev_t").strip()
    if not re.fullmatch(r"[0-9]+", diskseq):
        refuse("diskseq_malformed")
    if not re.fullmatch(r"[0-9]+:[0-9]+", dev_t):
        refuse("whole_dev_t_malformed")
    identity = controller_identity()
    poll_queues = read_one(os.path.join(sys_root, "module", "nvme",
                                        "parameters", "poll_queues"),
                           "poll_queues").strip()
    if not re.fullmatch(r"[1-9][0-9]*", poll_queues):
        refuse("poll_queues_invalid=%s" % poll_queues)
    io_poll = read_one(os.path.join(sys_root, "block", base, "queue",
                                    "io_poll"), "io_poll").strip()
    if io_poll != "1":
        refuse("io_poll_invalid=%s" % io_poll)
    mq_root = os.path.join(sys_root, "block", base, "mq")
    try:
        hctx_names = sorted((entry.name for entry in os.scandir(mq_root)
                             if entry.is_dir(follow_symlinks=False) and
                             entry.name.isdigit()), key=int)
    except OSError as exc:
        refuse("mq_generation_read_failed=%s" % exc)
    if not hctx_names:
        refuse("mq_generation_empty")
    mq_raw = []
    for hctx in hctx_names:
        raw = read_one(os.path.join(mq_root, hctx, "cpu_list"),
                       "mq_cpu_list_%s" % hctx).strip()
        parse_cpu_list(raw, "mq_cpu_list_%s" % hctx)
        mq_raw.append("%s|%s" % (hctx, raw))
    namespace = "%s|%s|%s" % (base, dev_t, diskseq)
    state = {"boot_id": boot_id, "diskseq": diskseq, "dev_t": dev_t,
             "namespace": namespace, "controller": identity,
             "poll_queues": poll_queues, "io_poll": io_poll,
             "mq_raw": mq_raw}
    lines = ["format=exitos-cell-poll-v1", "phase=" + phase,
             "timestamp_monotonic_ns=%d" % timestamp,
             "boot_id=" + boot_id, "diskseq=" + diskseq,
             "namespace=" + namespace, "controller=" + identity,
             "poll_queues=" + poll_queues, "io_poll=" + io_poll,
             "mq_count=%d" % len(mq_raw)]
    lines.extend("mq_cpu_list=" + item for item in mq_raw)
    return "\n".join(lines) + "\n", state

def parse_disk_pre(payload):
    fields = {}
    for line in payload.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    raw = fields.get("raw", "").split()
    if fields.get("capture_error") or len(raw) < 17 or any(
            not value.isdigit() for value in raw):
        refuse("diskstats_pre_malformed")
    return fields, [int(value) for value in raw]

def parse_poll_pre(payload):
    fields = {}
    mq_raw = []
    for line in payload.splitlines():
        if line.startswith("mq_cpu_list="):
            mq_raw.append(line.split("=", 1)[1])
        elif "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    required = ("boot_id", "diskseq", "namespace", "controller",
                "poll_queues", "io_poll", "mq_count")
    if any(not fields.get(key) for key in required):
        refuse("poll_pre_malformed")
    if (not fields["mq_count"].isdigit() or int(fields["mq_count"]) != len(mq_raw)
            or not mq_raw):
        refuse("poll_pre_malformed")
    return {"boot_id": fields["boot_id"], "diskseq": fields["diskseq"],
            "dev_t": fields["namespace"].split("|")[1],
            "namespace": fields["namespace"], "controller": fields["controller"],
            "poll_queues": fields["poll_queues"], "io_poll": fields["io_poll"],
            "mq_raw": mq_raw}

directory = os.open(retained_path, os.O_RDONLY | os.O_DIRECTORY)
try:
    verify_directory(directory)
    if operation == "init":
        created = {}
        for name in names:
            try:
                os.stat(name, dir_fd=directory, follow_symlinks=False)
            except FileNotFoundError:
                continue
            refuse("pre-existing evidence object: %s" % name)
        try:
            for name in names:
                fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL |
                             os.O_NOFOLLOW | os.O_CLOEXEC, 0o600,
                             dir_fd=directory)
                fst = os.fstat(fd)
                if (not stat.S_ISREG(fst.st_mode) or fst.st_uid != owner or
                        stat.S_IMODE(fst.st_mode) != 0o600 or
                        fst.st_nlink != 1 or fst.st_size != 0):
                    refuse("unsafe created evidence object: %s" % name)
                created[name] = fd
            disk_payload, disk = disk_snapshot("pre")
            if disk["error"]:
                refuse(disk["error"])
            poll_payload, poll = poll_snapshot("pre")
            write_all(created["diskstats-pre.txt"], disk_payload)
            write_all(created["poll-pre.txt"], poll_payload)
            os.fchmod(created["diskstats-pre.txt"], 0o400)
            os.fchmod(created["poll-pre.txt"], 0o400)
            os.fsync(directory)
            print(",".join("%s:%d:%d" %
                  (name, os.fstat(created[name]).st_dev,
                   os.fstat(created[name]).st_ino) for name in names))
        finally:
            for fd in created.values():
                os.close(fd)
    elif operation == "record":
        if len(extra) != 1 or not re.fullmatch(r"[0-9]+", extra[0]):
            refuse("fio_rc_malformed")
        fds = [open_verified(directory, name) for name in names]
        try:
            result = fds[names.index("cell-result.txt")]
            if os.fstat(result).st_size != 0:
                refuse("fio_rc_already_recorded")
            write_all(result, "fio_rc=%s\n" % extra[0])
        finally:
            for fd in fds:
                os.close(fd)
    elif operation == "summary":
        if (len(extra) != 1 or not extra[0].startswith("CELL_OK ") or
                "\n" in extra[0] or "\x00" in extra[0]):
            refuse("cell_summary_malformed")
        fds = [open_verified(directory, name) for name in names]
        try:
            summary = fds[names.index("cell-summary.txt")]
            if os.fstat(summary).st_size != 0:
                refuse("cell_summary_already_recorded")
            payload = extra[0] + "\n"
            write_all(summary, payload)
            os.fchmod(summary, 0o400)
            os.fsync(directory)
            print("CELL_SUMMARY_WRITTEN")
        finally:
            for fd in fds:
                os.close(fd)
    elif operation == "post":
        fds = {name: open_verified(directory, name) for name in names}
        try:
            for name in ("diskstats-post.txt", "poll-post.txt"):
                if os.fstat(fds[name]).st_size != 0:
                    refuse("evidence_post_already_recorded=%s" % name)
            os.lseek(fds["diskstats-pre.txt"], 0, os.SEEK_SET)
            disk_pre_payload = os.read(fds["diskstats-pre.txt"], 1024 * 1024).decode("ascii")
            os.lseek(fds["poll-pre.txt"], 0, os.SEEK_SET)
            poll_pre_payload = os.read(fds["poll-pre.txt"], 1024 * 1024).decode("ascii")
            disk_pre, disk_pre_raw = parse_disk_pre(disk_pre_payload)
            poll_pre = parse_poll_pre(poll_pre_payload)
            disk_payload, disk_post = disk_snapshot("post")
            disk_error = disk_post["error"]
            try:
                poll_payload, poll_post = poll_snapshot("post")
                poll_error = None
            except SystemExit as exc:
                poll_error = str(exc)
                poll_payload = ("format=exitos-cell-poll-v1\nphase=post\n"
                                "timestamp_monotonic_ns=%d\n"
                                "capture_error=%s\n" %
                                (time.monotonic_ns(), poll_error))
                poll_post = None
            write_all(fds["diskstats-post.txt"], disk_payload)
            write_all(fds["poll-post.txt"], poll_payload)
            os.fchmod(fds["diskstats-post.txt"], 0o400)
            os.fchmod(fds["poll-post.txt"], 0o400)
            os.fsync(directory)
            if disk_error:
                refuse(disk_error)
            if (disk_post["boot_id"] != disk_pre.get("boot_id") or
                    disk_post["diskseq"] != disk_pre.get("diskseq") or
                    disk_post["dev_t"] != disk_pre.get("dev_t") or
                    disk_post["device"] != disk_pre.get("device")):
                refuse("diskstats_identity_drift")
            if disk_post["timestamp"] < int(disk_pre["timestamp_monotonic_ns"]):
                refuse("diskstats_timestamp_regression")
            disk_post_raw = [int(value) for value in disk_post["raw"]]
            if len(disk_post_raw) != len(disk_pre_raw):
                refuse("diskstats_shape_drift")
            # Linux field 9 (zero-based 8) is instantaneous I/O in progress;
            # every other whole-disk field is a cumulative counter.
            for index, (old, new) in enumerate(zip(disk_pre_raw, disk_post_raw)):
                if index != 8 and new < old:
                    refuse("diskstats_counter_regression=index%d:%d:%d" %
                           (index, old, new))
            if poll_error:
                refuse(poll_error)
            if poll_post != poll_pre:
                refuse("poll_state_drift")
        finally:
            for fd in fds.values():
                os.close(fd)
    elif operation == "seal":
        if (len(extra) != 1 or not extra[0].startswith("CELL_OK ") or
                "\n" in extra[0] or "\x00" in extra[0]):
            refuse("cell_summary_expected_malformed")
        fds = [open_verified(directory, name) for name in names]
        try:
            for name in ("diskstats-pre.txt", "diskstats-post.txt",
                         "poll-pre.txt", "poll-post.txt", "cell-summary.txt"):
                fst = os.fstat(fds[names.index(name)])
                if fst.st_size == 0 or stat.S_IMODE(fst.st_mode) != 0o400:
                    if name == "cell-summary.txt" and fst.st_size == 0:
                        refuse("cell_summary_empty")
                    refuse("evidence_not_sealed=%s" % name)
            summary = fds[names.index("cell-summary.txt")]
            os.lseek(summary, 0, os.SEEK_SET)
            summary_content = os.read(summary, 1024 * 1024).decode("ascii")
            if (not re.fullmatch(r"CELL_OK [^\n]+\n", summary_content) or
                    summary_content != extra[0] + "\n"):
                refuse("cell_summary_content_mismatch")
            result = fds[names.index("cell-result.txt")]
            os.lseek(result, 0, os.SEEK_SET)
            content = os.read(result, 4096).decode("ascii")
            if not re.fullmatch(r"fio_rc=0\n", content):
                refuse("cell_result_not_sealable")
            os.lseek(result, 0, os.SEEK_END)
            write_all(result, "summary_verified=exact\nstatus=CELL_OK\n")
            os.fchmod(result, 0o400)
            os.fsync(directory)
        finally:
            for fd in fds:
                os.close(fd)
    else:
        refuse("unknown_cell_evidence_operation")
finally:
    os.close(directory)
PY
}

retain_cell_evidence_directory()
{
    local path=$1 path_id fd_id meta
    [ -n "$path" ] && [[ $path == /* ]] && [ -d "$path" ] && [ ! -L "$path" ] ||
        die "unsafe_cell_evidence_directory=$path"
    exec {CELL_EVIDENCE_DIR_FD}<"$path" || die cell_evidence_directory_open_failed
    CELL_EVIDENCE_FD_PATH=/proc/$$/fd/$CELL_EVIDENCE_DIR_FD
    path_id=$(stat -Lc '%d:%i' -- "$path") || die cell_evidence_path_stat_failed
    fd_id=$(stat -Lc '%d:%i' -- "$CELL_EVIDENCE_FD_PATH") ||
        die cell_evidence_fd_stat_failed
    meta=$(stat -Lc '%u:%a' -- "$CELL_EVIDENCE_FD_PATH") ||
        die cell_evidence_meta_failed
    [ "$path_id" = "$fd_id" ] && [ "$meta" = "$(id -u):700" ] ||
        die evidence_directory_identity_changed
    CELL_EVIDENCE_DIR=$path
}

initialize_cell_evidence()
{
    local cell=$1 map=$2 identities
    retain_cell_evidence_directory "$cell"
    CELL_EVIDENCE_MAP=$map
    CELL_EVIDENCE_IDENTITIES=
    identities=$(cell_evidence_python init) || die cell_evidence_init_failed
    CELL_EVIDENCE_IDENTITIES=$identities
    CELL_SUMMARY_VALUE=
    CELL_EVIDENCE_ACTIVE=1
    echo "CELL_EVIDENCE_PRE_CAPTURED cell=$cell"
}

release_cell_evidence_directory()
{
    if [[ ${CELL_EVIDENCE_DIR_FD:-} =~ ^[0-9]+$ ]]; then
        exec {CELL_EVIDENCE_DIR_FD}<&- || return 1
    fi
    CELL_EVIDENCE_DIR_FD=
    CELL_EVIDENCE_FD_PATH=
    CELL_EVIDENCE_DIR=
    CELL_EVIDENCE_IDENTITIES=
    CELL_SUMMARY_VALUE=
}

record_cell_fio_rc()
{
    local rc=$1
    [ "$CELL_EVIDENCE_ACTIVE" = 1 ] || return 1
    cell_evidence_python record "$rc" || return 1
    echo "CELL_EVIDENCE_RC_RECORDED fio_rc=$rc"
}

capture_cell_evidence_post()
{
    [ "$CELL_EVIDENCE_ACTIVE" = 1 ] || return 1
    cell_evidence_python post || return 1
    echo CELL_EVIDENCE_POST_VALIDATED
}

persist_cell_summary()
{
    local summary=$1 record
    [ "$CELL_EVIDENCE_ACTIVE" = 1 ] || die cell_evidence_not_active
    record=$(cell_evidence_python summary "$summary") ||
        die cell_summary_persist_failed
    [ "$record" = CELL_SUMMARY_WRITTEN ] || die cell_summary_record_invalid
    CELL_SUMMARY_VALUE=$summary
    echo "CELL_SUMMARY_PERSISTED verification=exact"
}

seal_cell_evidence_success()
{
    [ "$CELL_EVIDENCE_ACTIVE" = 1 ] || die cell_evidence_not_active
    [[ ${CELL_SUMMARY_VALUE:-} == CELL_OK\ * ]] ||
        die cell_summary_not_persisted
    cell_evidence_python seal "$CELL_SUMMARY_VALUE" ||
        die cell_evidence_seal_failed
    CELL_EVIDENCE_ACTIVE=0
    release_cell_evidence_directory || die cell_evidence_directory_release_failed
    echo CELL_EVIDENCE_SEALED
}

append_global_summary()
{
    local destination=$1 summary=$2
    printf '%s\n' "$summary" >>"$destination" ||
        die "global_summary_append_failed=$destination"
}

protected_root_home()
{
    local protected=/root resolved

    if [ -n "${EXITOS_SCALE_TEST_PROTECTED_ROOT_HOME:-}" ]; then
        [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] ||
            die test_protected_root_home_override_forbidden
        protected=$EXITOS_SCALE_TEST_PROTECTED_ROOT_HOME
    fi
    [ -n "$protected" ] && [[ $protected == /* ]] &&
        [ -d "$protected" ] && [ ! -L "$protected" ] ||
        die "protected_root_home_unsafe=$protected"
    resolved=$(readlink -f -- "$protected") ||
        die "protected_root_home_unresolvable=$protected"
    [ "$resolved" = "$protected" ] ||
        die "protected_root_home_not_canonical=$protected"
    printf '%s' "$resolved"
}

validate_result_root_policy()
{
    local candidate=$1 canonical parent protected

    [ -n "$candidate" ] && [[ $candidate == /* ]] ||
        die "result_root_not_absolute=$candidate"
    canonical=$(readlink -m -- "$candidate") ||
        die "result_root_unresolvable=$candidate"
    [ "$candidate" = "$canonical" ] ||
        die "result_root_not_canonical=$candidate"
    protected=$(protected_root_home)
    [ "$canonical" != "$protected" ] ||
        die "result_root_is_protected_root=$canonical"
    parent=${canonical%/*}
    [ -n "$parent" ] || parent=/
    [ "$parent" != "$protected" ] ||
        die "result_root_is_direct_root_child=$canonical"
}

require_result_root_for_mode()
{
    local mode=$1

    case "$mode" in
        --pilot-*)
            [ "$RESULT_ROOT_EXPLICIT" = x ] && [ -n "$RESULT_ROOT" ] ||
                die "EXITOS_SCALE_RESULT_ROOT_missing mode=$mode"
            validate_result_root_policy "$RESULT_ROOT"
            ;;
        --plan) ;;
    esac
}

run_test_result_root_required()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_result_root_required_usage
    require_result_root_for_mode "$1"
    echo "RESULT_ROOT_REQUIRED_OK mode=$1 root=$RESULT_ROOT"
}

run_test_result_root_policy()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_result_root_policy_usage
    validate_result_root_policy "$1"
    echo "RESULT_ROOT_POLICY_OK root=$1"
}

# Retain an already-created private work directory.  Every later pathname used
# for workload files is rooted at this descriptor, so replacing the directory
# pathname after this point cannot redirect a write.  The directory is useful
# only as an exclusive, per-campaign object: accepting group/world access would
# reopen the child-name race that retaining the parent is meant to close.
retain_owned_workdir()
{
    local path=$1 path_id fd_id meta

    [ -n "$path" ] && [[ $path == /* ]] || die "workdir_not_absolute=$path"
    [ ! -L "$path" ] || die "workdir_is_symlink=$path"
    [ -d "$path" ] || die "workdir_not_directory=$path"
    exec {WORK_DIR_FD}<"$path" || die "workdir_open_failed=$path"
    WORK_FD_PATH=/proc/$$/fd/$WORK_DIR_FD
    [ -d "$WORK_FD_PATH" ] || die "retained_workdir_not_directory=$path"
    path_id=$(stat -Lc '%d:%i' -- "$path") || die "workdir_path_stat_failed=$path"
    fd_id=$(stat -Lc '%d:%i' -- "$WORK_FD_PATH") || die "workdir_fd_stat_failed=$path"
    [ "$path_id" = "$fd_id" ] || die "workdir_changed_during_open=$path"
    meta=$(stat -Lc '%u:%a' -- "$WORK_FD_PATH") || die "workdir_meta_failed=$path"
    [ "$meta" = "$(id -u):700" ] || die "workdir_not_owned_0700=$meta"
    WORK_DIR=$path
    export WORK_DIR WORK_FD_PATH
}

# Create exactly the workload leaf names without ever opening an existing
# object.  The preliminary scan gives an honest all-or-nothing refusal for the
# ordinary case; O_EXCL|O_NOFOLLOW remains the authority at each open if a name
# appears between the scan and creation.
secure_init_workfiles()
{
    local dirfd_path=$1

    python3 - "$dirfd_path" "$MAX_JOBS" <<'PY'
import errno
import os
import stat
import sys

path = sys.argv[1]
count = int(sys.argv[2])
owner = os.geteuid()
# path is deliberately /proc/<retaining-shell>/fd/<n>, a procfs magic link to
# the already-verified directory object.  Following that one link is the point;
# child leaf opens below still use O_NOFOLLOW.
directory = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
created = []
try:
    dst = os.fstat(directory)
    if not stat.S_ISDIR(dst.st_mode) or dst.st_uid != owner or stat.S_IMODE(dst.st_mode) != 0o700:
        raise SystemExit("unsafe retained work directory")
    names = [f"w{i}.log" for i in range(count)]
    for name in names:
        try:
            os.stat(name, dir_fd=directory, follow_symlinks=False)
        except FileNotFoundError:
            continue
        raise SystemExit(f"pre-existing workload object: {name}")
    for name in names:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
        fd = os.open(name, flags, 0o600, dir_fd=directory)
        try:
            st = os.fstat(fd)
            if (not stat.S_ISREG(st.st_mode) or st.st_uid != owner or
                    stat.S_IMODE(st.st_mode) != 0o600 or st.st_nlink != 1 or
                    st.st_size != 0):
                raise SystemExit(f"unsafe created workload object: {name}")
            created.append((name, st.st_dev, st.st_ino))
        finally:
            os.close(fd)
    os.fsync(directory)
finally:
    os.close(directory)
print("WORKFILES_CREATED count=%d" % len(created))
PY
}

seal_workset()
{
    local generation=$1 expected_size=$2

    python3 - "$WORK_FD_PATH" "$MAX_JOBS" "$expected_size" "$generation" <<'PY'
import os
import re
import signal
import stat
import sys

path, count_s, size_s, generation = sys.argv[1:]
count = int(count_s)
expected_size = int(size_s)
if not re.fullmatch(r"[A-Za-z0-9._:+-]+", generation):
    raise SystemExit("unsafe generation token")
directory = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
try:
    lines = ["format=exitos-workset-v1", f"generation={generation}",
             f"count={count}", f"size={expected_size}"]
    for i in range(count):
        name = f"w{i}.log"
        st = os.stat(name, dir_fd=directory, follow_symlinks=False)
        if (not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or
                stat.S_IMODE(st.st_mode) != 0o600 or st.st_nlink != 1 or
                st.st_size != expected_size):
            raise SystemExit(f"unsafe workload object while sealing: {name}")
        lines.append("file=%s dev=%d ino=%d uid=%d mode=%03o nlink=%d size=%d" %
                     (name, st.st_dev, st.st_ino, st.st_uid,
                      stat.S_IMODE(st.st_mode), st.st_nlink, st.st_size))
    payload = ("\n".join(lines) + "\n").encode("ascii")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open("WORKSET.manifest", flags, 0o600, dir_fd=directory)
    try:
        view = memoryview(payload)
        while view:
            n = os.write(fd, view)
            if n <= 0:
                raise SystemExit("short workset-manifest write")
            view = view[n:]
        os.fsync(fd)
        os.fchmod(fd, 0o400)
        mst = os.fstat(fd)
    finally:
        os.close(fd)
    os.fsync(directory)
finally:
    os.close(directory)
identity = "%d:%d:%d:%o:%d:%d:%d:%d" % (
    mst.st_dev, mst.st_ino, mst.st_uid, stat.S_IMODE(mst.st_mode),
    mst.st_nlink, mst.st_size, mst.st_mtime_ns, mst.st_ctime_ns)
print("WORKSET_SEALED identity=" + identity)
PY
}

verify_workset()
{
    local phase=$1 generation=$2 expected_size=$3 expected_identity=$4

    python3 - "$WORK_FD_PATH" "$MAX_JOBS" "$expected_size" \
        "$generation" "$expected_identity" "$phase" <<'PY'
import os
import re
import stat
import sys
import time

path, count_s, size_s, generation, expected_identity, phase = sys.argv[1:]
count = int(count_s)
expected_size = int(size_s)
if not re.fullmatch(r"[0-9]+:[0-9]+:[0-9]+:[0-7]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+",
                    expected_identity):
    raise SystemExit("invalid expected workset manifest identity")
directory = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
try:
    dst = os.fstat(directory)
    if (not stat.S_ISDIR(dst.st_mode) or dst.st_uid != os.geteuid() or
            stat.S_IMODE(dst.st_mode) != 0o700):
        raise SystemExit("retained work directory metadata changed")
    mst = os.stat("WORKSET.manifest", dir_fd=directory, follow_symlinks=False)
    if (not stat.S_ISREG(mst.st_mode) or mst.st_uid != os.geteuid() or
            stat.S_IMODE(mst.st_mode) != 0o400 or mst.st_nlink != 1):
        raise SystemExit("unsafe workset manifest object")
    identity = "%d:%d:%d:%o:%d:%d:%d:%d" % (
        mst.st_dev, mst.st_ino, mst.st_uid, stat.S_IMODE(mst.st_mode),
        mst.st_nlink, mst.st_size, mst.st_mtime_ns, mst.st_ctime_ns)
    if identity != expected_identity:
        raise SystemExit("workset manifest identity changed")
    fd = os.open("WORKSET.manifest", os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                 dir_fd=directory)
    try:
        payload = b""
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            payload += chunk
            if len(payload) > 65536:
                raise SystemExit("oversized workset manifest")
    finally:
        os.close(fd)
    try:
        lines = payload.decode("ascii").splitlines()
    except UnicodeDecodeError:
        raise SystemExit("non-ASCII workset manifest")
    head = ["format=exitos-workset-v1", f"generation={generation}",
            f"count={count}", f"size={expected_size}"]
    if lines[:4] != head or len(lines) != count + 4:
        raise SystemExit("workset manifest generation/shape mismatch")
    pattern = re.compile(r"file=(w([0-9]+)\.log) dev=([0-9]+) ino=([0-9]+) "
                         r"uid=([0-9]+) mode=([0-7]{3}) nlink=([0-9]+) size=([0-9]+)")
    for i, line in enumerate(lines[4:]):
        match = pattern.fullmatch(line)
        if not match or match.group(1) != f"w{i}.log" or int(match.group(2)) != i:
            raise SystemExit(f"malformed workset entry: {line}")
        st = os.stat(match.group(1), dir_fd=directory, follow_symlinks=False)
        current = (st.st_dev, st.st_ino, st.st_uid, stat.S_IMODE(st.st_mode),
                   st.st_nlink, st.st_size)
        recorded = tuple(int(match.group(j), 8 if j == 6 else 10)
                         for j in range(3, 9))
        if not stat.S_ISREG(st.st_mode) or current != recorded:
            raise SystemExit(f"workfile identity changed: {match.group(1)}")
finally:
    os.close(directory)
print(f"WORKSET_OK phase={phase} identity={expected_identity}")
PY
}

validate_scale_variants()
{
    local requested=$1 report kind label path hash identity count=0
    [ -n "$requested" ] && [[ $requested == /* ]] ||
        die "variants_dir_not_absolute=$requested"
    if ! report=$(python3 - "$requested" <<'PY'
import hashlib
import os
import re
import stat
import sys

requested=sys.argv[1]
if os.path.islink(requested) or not os.path.isdir(requested):
    raise SystemExit("unsafe variants directory")
root=os.path.realpath(requested)
if root != requested:
    raise SystemExit("variants directory is not canonical")
rst=os.stat(root)
if rst.st_uid != os.geteuid() or stat.S_IMODE(rst.st_mode) & 0o022:
    raise SystemExit("variants directory is not privately owned")

manifest_path=os.path.join(root,"MANIFEST.txt")
mst=os.lstat(manifest_path)
if (not stat.S_ISREG(mst.st_mode) or mst.st_uid != os.geteuid() or
        mst.st_nlink != 1 or stat.S_IMODE(mst.st_mode) != 0o400):
    raise SystemExit("unsafe variants manifest")
mfd=os.open(manifest_path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
try:
    payload=b""
    while True:
        chunk=os.read(mfd,65536)
        if not chunk: break
        payload+=chunk
        if len(payload)>1024*1024: raise SystemExit("oversized variants manifest")
finally:
    os.close(mfd)
try: lines=payload.decode("ascii").splitlines()
except UnicodeDecodeError: raise SystemExit("non-ASCII variants manifest")
if lines.count("format=exitos-scale-variants-v2") != 1:
    raise SystemExit("wrong variants manifest format")
if lines.count("snapshot_dir=source-snapshot") != 1:
    raise SystemExit("wrong source snapshot membership")

def one(prefix):
    values=[line[len(prefix):] for line in lines if line.startswith(prefix)]
    if len(values)!=1: raise SystemExit("duplicate/missing "+prefix)
    return values[0]

def identity(st):
    return "%d:%d:%d:%o:%d:%d:%d:%d" % (
        st.st_dev, st.st_ino, st.st_uid, stat.S_IMODE(st.st_mode),
        st.st_nlink, st.st_size, st.st_mtime_ns, st.st_ctime_ns)

before=one("snapshot_hash_before=")
after=one("snapshot_hash_after=")
if not re.fullmatch(r"[0-9a-f]{64}",before) or before!=after:
    raise SystemExit("source snapshot pre/post hash mismatch")
try:
    begin=lines.index("source_hashes_begin")
    end=lines.index("source_hashes_end")
except ValueError:
    raise SystemExit("source hash block missing")
if begin>=end or lines.count("source_hashes_begin")!=1 or lines.count("source_hashes_end")!=1:
    raise SystemExit("source hash block malformed")
records=lines[begin+1:end]
record_pattern=re.compile(r"([0-9a-f]{64})  ((?:src|include)/[A-Za-z0-9_.+-]+)")
record_map={}
for line in records:
    match=record_pattern.fullmatch(line)
    if not match or match.group(2) in record_map:
        raise SystemExit("source hash membership malformed")
    record_map[match.group(2)]=match.group(1)
snapshot=os.path.join(root,"source-snapshot")
sst=os.lstat(snapshot)
if not stat.S_ISDIR(sst.st_mode) or sst.st_uid!=os.geteuid() or stat.S_IMODE(sst.st_mode)&0o222:
    raise SystemExit("unsafe source snapshot directory")
actual={}
for walk_root,dirs,files in os.walk(snapshot,followlinks=False):
    for name in dirs:
        st=os.lstat(os.path.join(walk_root,name))
        if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.geteuid() or stat.S_IMODE(st.st_mode)&0o222:
            raise SystemExit("unsafe source snapshot subdirectory")
    for name in files:
        full=os.path.join(walk_root,name)
        st=os.lstat(full)
        if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.geteuid() or stat.S_IMODE(st.st_mode)&0o222:
            raise SystemExit("unsafe source snapshot file")
        rel=os.path.relpath(full,snapshot)
        actual[rel]=hashlib.sha256(open(full,"rb").read()).hexdigest()
if actual!=record_map:
    raise SystemExit("source snapshot hash/membership mismatch")
canonical_records=[digest+"  "+name for name,digest in
                   sorted(record_map.items(),key=lambda item:item[0])]
tree=hashlib.sha256(("\n".join(canonical_records)+"\n").encode()).hexdigest()
if tree!=before:
    raise SystemExit("source snapshot tree hash mismatch")

expected={
 "preload-table_global-stats_global.so":("gg","global","global","EXITOS_TABLE_GLOBAL_BASELINE+EXITOS_STATS_GLOBAL_BASELINE"),
 "preload-table_global-stats_sharded.so":("gs","global","sharded","EXITOS_TABLE_GLOBAL_BASELINE"),
 "preload-table_sharded-stats_global.so":("sg","sharded","global","EXITOS_STATS_GLOBAL_BASELINE"),
 "preload-table_sharded-stats_sharded.so":("ss","sharded","sharded","none"),
}
artifact_pattern=re.compile(r"artifact=([^ ]+) table=(global|sharded) "
                            r"stats=(global|sharded) defs=([^ ]+) sha256=([0-9a-f]{64})")
seen={}
for line in lines:
    if not line.startswith("artifact="): continue
    match=artifact_pattern.fullmatch(line)
    if not match or match.group(1) in seen:
        raise SystemExit("artifact membership malformed")
    seen[match.group(1)]=(match.group(2),match.group(3),match.group(4),match.group(5))
if set(seen)!=set(expected):
    raise SystemExit("artifact membership mismatch")
actual_names={name for name in os.listdir(root) if name.endswith(".so")}
if actual_names!=set(expected):
    raise SystemExit("artifact directory membership mismatch")
for name,(short,table_mode,stats_mode,want_defs) in expected.items():
    got_table,got_stats,got_defs,want_hash=seen[name]
    if (got_table,got_stats)!=(table_mode,stats_mode):
        raise SystemExit("artifact mode membership mismatch")
    if got_defs!=want_defs:
        raise SystemExit("artifact defs membership mismatch")
    full=os.path.join(root,name)
    st=os.lstat(full)
    if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.geteuid() or st.st_nlink!=1:
        raise SystemExit("unsafe artifact object")
    have_hash=hashlib.sha256(open(full,"rb").read()).hexdigest()
    if have_hash!=want_hash:
        raise SystemExit("artifact hash mismatch: "+name)
    if stat.S_IMODE(st.st_mode)!=0o500:
        raise SystemExit("unsafe artifact mode")
    print("VARIANT\t%s\t%s\t%s\t%s"%
          (short,full,want_hash,identity(st)))
print("MANIFEST\tmanifest\t%s\t%s\t%s"%
      (manifest_path,hashlib.sha256(payload).hexdigest(),identity(mst)))
PY
    )
    then
        die variant_validation_failed
    fi

    declare -gA VARIANT_PATH=()
    declare -gA VARIANT_SHA=()
    declare -gA VARIANT_IDENTITY=()
    while IFS=$'\t' read -r kind label path hash identity; do
        case "$kind:$label" in
            VARIANT:gg|VARIANT:gs|VARIANT:sg|VARIANT:ss)
                [ -z "${VARIANT_PATH[$label]+x}" ] || die duplicate_variant_record
                VARIANT_PATH[$label]=$path
                VARIANT_SHA[$label]=$hash
                VARIANT_IDENTITY[$label]=$identity
                count=$((count + 1))
                ;;
            MANIFEST:manifest)
                VARIANT_MANIFEST_PATH=$path
                VARIANT_MANIFEST_SHA=$hash
                VARIANT_MANIFEST_IDENTITY=$identity
                ;;
            *) die malformed_variant_validation_record ;;
        esac
    done <<<"$report"
    [ "$count" -eq 4 ] && [ -n "${VARIANT_MANIFEST_SHA:-}" ] &&
        [ -n "${VARIANT_MANIFEST_IDENTITY:-}" ] ||
        die incomplete_variant_validation
    VARIANTS_DIR=$requested
    export VARIANTS_DIR VARIANT_MANIFEST_PATH VARIANT_MANIFEST_SHA \
        VARIANT_MANIFEST_IDENTITY
    echo 'VARIANTS_VALIDATED endpoints=gg,ss all=gg,gs,sg,ss'
}

assert_scale_variants()
{
    local label args=("manifest" "$VARIANT_MANIFEST_PATH" \
        "$VARIANT_MANIFEST_IDENTITY")
    for label in gg gs sg ss; do
        args+=("$label" "${VARIANT_PATH[$label]}" \
            "${VARIANT_IDENTITY[$label]}")
    done
    python3 - "${args[@]}" <<'PY' || die variants_identity_changed
import os
import re
import stat
import sys

values=sys.argv[1:]
if len(values)!=15:
    raise SystemExit("variant identity argument count changed")
pattern=re.compile(r"[0-9]+:[0-9]+:[0-9]+:[0-7]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+")
for offset in range(0,len(values),3):
    label,path,expected=values[offset:offset+3]
    if not pattern.fullmatch(expected):
        raise SystemExit("malformed frozen identity: "+label)
    st=os.lstat(path)
    mode=stat.S_IMODE(st.st_mode)
    current="%d:%d:%d:%o:%d:%d:%d:%d"%(
        st.st_dev,st.st_ino,st.st_uid,mode,st.st_nlink,st.st_size,
        st.st_mtime_ns,st.st_ctime_ns)
    wanted_mode=0o400 if label=="manifest" else 0o500
    if (not stat.S_ISREG(st.st_mode) or st.st_uid!=os.geteuid() or
            st.st_nlink!=1 or mode!=wanted_mode or current!=expected):
        raise SystemExit("frozen object identity changed: "+label)
PY
}

# Bind the exact objects that the measured process will use.  Descriptors
# 100..107 are the eight workload inodes and descriptor 120 is the preload
# artifact.  fio names the former as /proc/self/fd/10$jobnum; the dynamic
# loader names the latter as /proc/self/fd/120.  Path replacement after this
# helper has opened and verified an object therefore cannot redirect either
# workload I/O or code loading.
exec_bound_objects()
{
    local work_mode=$1 artifact=$2 artifact_identity=$3 expected_parent=$$
    shift 3
    [ "$#" -gt 0 ] || die bound_command_missing
    if [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] &&
       [ -n "${EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE:-}" ]; then
        expected_parent=$EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE
    fi
    exec python3 - "$work_mode" "${WORK_FD_PATH:--}" "$MAX_JOBS" \
        "$((FILE_MIB * 1024 * 1024))" "${CAMPAIGN_GENERATION_SHA:--}" \
        "${WORKSET_MANIFEST_IDENTITY:--}" "$artifact" "$artifact_identity" \
        "$expected_parent" "$@" <<'PY'
import ctypes
import os
import re
import signal
import stat
import sys
import time

(work_mode,work_path,count_s,size_s,generation,manifest_identity,
 artifact,artifact_identity,expected_parent_s,*command)=sys.argv[1:]
count=int(count_s)
expected_size=int(size_s)
expected_parent=int(expected_parent_s)
owner=os.geteuid()

def read_all(fd,limit):
    out=b""
    while True:
        chunk=os.read(fd,65536)
        if not chunk: break
        out+=chunk
        if len(out)>limit: raise SystemExit("bound object is oversized")
    return out

if expected_parent <= 1:
    raise SystemExit("invalid expected parent")
if (os.environ.get("EXITOS_SCALE_TEST_MODE") == "1" and
        os.environ.get("EXITOS_SCALE_TEST_PAUSE_BEFORE_PDEATHSIG") == "1"):
    print("BOUND_PRE_PDEATHSIG_READY pid=%d expected_parent=%d" %
          (os.getpid(), expected_parent), flush=True)
    os.kill(os.getpid(), signal.SIGSTOP)
parent_before_prctl = os.getppid()
if parent_before_prctl != expected_parent:
    raise SystemExit(143)
libc=ctypes.CDLL(None,use_errno=True)
if libc.prctl(1,signal.SIGKILL,0,0,0) != 0:
    raise SystemExit("PR_SET_PDEATHSIG failed errno=%d" % ctypes.get_errno())
parent_after_prctl = os.getppid()
if parent_after_prctl != expected_parent:
    raise SystemExit(143)

# The runner needs the wrapper's retained attestation for campaign and cell
# gates, but the measured command does not.  Close this launcher's inherited
# copy before opening any workload/artifact object, and remove both capability
# variables before the final exec (prepare and timed fio share this path).
attest_fd_text = os.environ.pop("EXITOS_CPUFREQ_ATTEST_FD", None)
attest_token = os.environ.pop("EXITOS_CPUFREQ_ATTEST_TOKEN", None)
if (attest_fd_text is None) != (attest_token is None):
    raise SystemExit("incomplete cpufreq attestation launch contract")
if attest_fd_text is not None:
    if not re.fullmatch(r"[0-9]+", attest_fd_text):
        raise SystemExit("invalid cpufreq attestation descriptor")
    attest_fd = int(attest_fd_text)
    if not (3 <= attest_fd <= 1048575):
        raise SystemExit("cpufreq attestation descriptor out of range")
    try:
        os.close(attest_fd)
    except OSError as error:
        raise SystemExit("cannot close inherited cpufreq attestation descriptor: %s" % error)
os.setsid()
if os.getpgrp() != os.getpid() or os.getsid(0) != os.getpid():
    raise SystemExit("bound I/O private process group setup failed")

io_cpu_text = os.environ.get("EXITOS_BOUND_IO_CPU_MAP")
io_nice_text = os.environ.get("EXITOS_BOUND_IO_NICE")
if (io_cpu_text is None) != (io_nice_text is None):
    raise SystemExit("incomplete bound I/O priority contract")
io_cpus = None
io_nice = None
if io_cpu_text is not None:
    if not re.fullmatch(r"[0-9,-]+", io_cpu_text):
        raise SystemExit("invalid bound I/O CPU map")
    io_cpus = set()
    for item in io_cpu_text.split(","):
        if "-" in item:
            pieces = item.split("-")
            if len(pieces) != 2:
                raise SystemExit("invalid bound I/O CPU range")
            first, last = (int(piece) for piece in pieces)
            if first > last:
                raise SystemExit("reversed bound I/O CPU range")
            io_cpus.update(range(first, last + 1))
        elif item:
            io_cpus.add(int(item))
        else:
            raise SystemExit("empty bound I/O CPU item")
    if not io_cpus:
        raise SystemExit("empty bound I/O CPU map")
    try:
        io_nice = int(io_nice_text)
    except ValueError:
        raise SystemExit("invalid bound I/O nice value")
    if io_nice != -20:
        raise SystemExit("bound I/O nice contract changed")
    # Normalize explicitly: inheriting SCHED_OTHER from the invoking shell is
    # not evidence that the measured child was launched with the contract.
    os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))
    os.setpriority(os.PRIO_PROCESS, 0, io_nice)
    os.sched_setaffinity(0, io_cpus)

if work_mode not in ("none","unsealed","sealed"):
    raise SystemExit("invalid bound-work mode")
if work_mode != "none":
    directory=os.open(work_path,os.O_RDONLY|os.O_DIRECTORY)
    try:
        dst=os.fstat(directory)
        if (not stat.S_ISDIR(dst.st_mode) or dst.st_uid!=owner or
                stat.S_IMODE(dst.st_mode)!=0o700):
            raise SystemExit("unsafe bound work directory")
        recorded={}
        if work_mode == "sealed":
            identity_pattern = (r"[0-9]+:[0-9]+:[0-9]+:[0-7]+:"
                                r"[0-9]+:[0-9]+:[0-9]+:[0-9]+")
            if not re.fullmatch(identity_pattern,manifest_identity):
                raise SystemExit("invalid bound workset manifest identity")
            mfd=os.open("WORKSET.manifest",os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,
                        dir_fd=directory)
            try:
                mst=os.fstat(mfd)
                current_manifest_identity="%d:%d:%d:%o:%d:%d:%d:%d"%(
                    mst.st_dev,mst.st_ino,mst.st_uid,stat.S_IMODE(mst.st_mode),
                    mst.st_nlink,mst.st_size,mst.st_mtime_ns,mst.st_ctime_ns)
                if (not stat.S_ISREG(mst.st_mode) or mst.st_uid!=owner or
                        stat.S_IMODE(mst.st_mode)!=0o400 or mst.st_nlink!=1 or
                        current_manifest_identity!=manifest_identity):
                    raise SystemExit("bound workset manifest identity changed")
                payload=read_all(mfd,65536)
            finally: os.close(mfd)
            lines=payload.decode("ascii").splitlines()
            head=["format=exitos-workset-v1",f"generation={generation}",
                  f"count={count}",f"size={expected_size}"]
            if lines[:4]!=head or len(lines)!=count+4:
                raise SystemExit("bound workset manifest shape changed")
            pattern=re.compile(r"file=(w([0-9]+)\.log) dev=([0-9]+) ino=([0-9]+) "
                               r"uid=([0-9]+) mode=([0-7]{3}) nlink=([0-9]+) size=([0-9]+)")
            for i,line in enumerate(lines[4:]):
                match=pattern.fullmatch(line)
                if not match or match.group(1)!=f"w{i}.log" or int(match.group(2))!=i:
                    raise SystemExit("malformed bound workset entry")
                recorded[i]=tuple(int(match.group(j),8 if j==6 else 10)
                                  for j in range(3,9))
        for i in range(count):
            name=f"w{i}.log"
            fd=os.open(name,os.O_RDWR|os.O_NOFOLLOW,dir_fd=directory)
            st=os.fstat(fd)
            current=(st.st_dev,st.st_ino,st.st_uid,stat.S_IMODE(st.st_mode),
                     st.st_nlink,st.st_size)
            safe=(stat.S_ISREG(st.st_mode) and st.st_uid==owner and
                  stat.S_IMODE(st.st_mode)==0o600 and st.st_nlink==1)
            if not safe:
                os.close(fd)
                raise SystemExit("unsafe bound workload object: "+name)
            if work_mode=="unsealed" and st.st_size!=0:
                os.close(fd)
                raise SystemExit("unsealed workload object is nonempty: "+name)
            if work_mode=="sealed" and current!=recorded.get(i):
                os.close(fd)
                raise SystemExit("bound workload identity changed: "+name)
            target=100+i
            os.dup2(fd,target,inheritable=True)
            if fd!=target: os.close(fd)
    finally:
        os.close(directory)

if artifact != "-":
    identity_pattern = (r"[0-9]+:[0-9]+:[0-9]+:[0-7]+:"
                        r"[0-9]+:[0-9]+:[0-9]+:[0-9]+")
    if not re.fullmatch(identity_pattern,artifact_identity):
        raise SystemExit("invalid bound artifact identity")
    afd=os.open(artifact,os.O_RDONLY|os.O_NOFOLLOW)
    ast=os.fstat(afd)
    current_artifact_identity="%d:%d:%d:%o:%d:%d:%d:%d"%(
        ast.st_dev,ast.st_ino,ast.st_uid,stat.S_IMODE(ast.st_mode),
        ast.st_nlink,ast.st_size,ast.st_mtime_ns,ast.st_ctime_ns)
    if (not stat.S_ISREG(ast.st_mode) or ast.st_uid!=owner or ast.st_nlink!=1 or
            stat.S_IMODE(ast.st_mode)!=0o500 or
            current_artifact_identity!=artifact_identity):
        os.close(afd)
        raise SystemExit("bound artifact identity changed")
    os.dup2(afd,120,inheritable=True)
    if afd!=120: os.close(afd)

if os.environ.get("EXITOS_SCALE_TEST_PAUSE_AFTER_BOUND_FDS")=="1":
    print("BOUND_FDS_READY",flush=True)
    os.kill(os.getpid(),signal.SIGSTOP)
if io_cpus is not None:
    def format_cpu_list(cpus):
        values = sorted(cpus)
        groups = []
        first = last = values[0]
        for value in values[1:]:
            if value == last + 1:
                last = value
                continue
            groups.append(str(first) if first == last else "%d-%d" % (first, last))
            first = last = value
        groups.append(str(first) if first == last else "%d-%d" % (first, last))
        return ",".join(groups)

    canonical_cpus = format_cpu_list(io_cpus)
    if (os.sched_getscheduler(0) != os.SCHED_OTHER or
            os.getpriority(os.PRIO_PROCESS, 0) != io_nice or
            os.sched_getaffinity(0) != io_cpus):
        raise SystemExit("bound I/O priority self-check failed")
    print("BOUND_IO_READY pid=%d expected_parent=%d pgid=%d sid=%d policy=%d nice=%d affinity=%s" %
          (os.getpid(), expected_parent, os.getpgrp(), os.getsid(0),
           os.SCHED_OTHER, io_nice, canonical_cpus), flush=True)
    ready_delay_ms = 0
    if os.environ.get("EXITOS_SCALE_TEST_MODE") == "1":
        try:
            ready_delay_ms = int(os.environ.get(
                "EXITOS_SCALE_TEST_DELAY_AFTER_BOUND_READY_MS", "0"))
        except ValueError:
            raise SystemExit("invalid bound READY delay")
        if not (0 <= ready_delay_ms <= 1000):
            raise SystemExit("bound READY delay out of range")
    if ready_delay_ms:
        time.sleep(ready_delay_ms / 1000.0)
    os.kill(os.getpid(), signal.SIGSTOP)
    if (os.getppid() != expected_parent or
            os.sched_getscheduler(0) != os.SCHED_OTHER or
            os.getpriority(os.PRIO_PROCESS, 0) != io_nice or
            os.sched_getaffinity(0) != io_cpus):
        raise SystemExit("bound I/O contract changed while stopped")
    os.environ.pop("EXITOS_BOUND_IO_CPU_MAP", None)
    os.environ.pop("EXITOS_BOUND_IO_NICE", None)
if not command:
    raise SystemExit("empty bound command")
allowed_fds = {0, 1, 2}
if work_mode != "none":
    allowed_fds.update(range(100, 100 + count))
if artifact != "-":
    allowed_fds.add(120)
for item in os.listdir("/proc/self/fd"):
    if not item.isdecimal():
        continue
    fd = int(item)
    if fd not in allowed_fds:
        try:
            os.close(fd)
        except OSError:
            pass
os.execvpe(command[0],command,os.environ)
PY
}

verify_bound_io_ready()
{
    local pid=$1 map=$2 output=$3 ready=0 state

    [[ $pid =~ ^[0-9]+$ ]] || return 1
    for _ in $(seq 1 500); do
        if grep -q "^BOUND_IO_READY pid=$pid " "$output" 2>/dev/null; then
            state=$(awk '$1 == "State:" {print $2}' "/proc/$pid/status" \
                2>/dev/null || true)
            case "$state" in
                T|t) ready=1; break ;;
            esac
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.01
    done
    [ "$ready" -eq 1 ] || return 1
    python3 - "$pid" "$$" "$map" "$IO_NICE" <<'PY'
import os
import re
import sys

pid = int(sys.argv[1])
expected_parent = int(sys.argv[2])
expected_map = sys.argv[3]
expected_nice = int(sys.argv[4])

def parse_cpu_list(value):
    if not re.fullmatch(r"[0-9,-]+", value):
        raise SystemExit("invalid CPU list")
    cpus = set()
    for item in value.split(","):
        if "-" in item:
            pieces = item.split("-")
            if len(pieces) != 2:
                raise SystemExit("invalid CPU range")
            first, last = (int(piece) for piece in pieces)
            if first > last:
                raise SystemExit("reversed CPU range")
            cpus.update(range(first, last + 1))
        elif item:
            cpus.add(int(item))
        else:
            raise SystemExit("empty CPU item")
    if not cpus:
        raise SystemExit("empty CPU list")
    return cpus

def format_cpu_list(cpus):
    values = sorted(cpus)
    groups = []
    first = last = values[0]
    for value in values[1:]:
        if value == last + 1:
            last = value
            continue
        groups.append(str(first) if first == last else "%d-%d" % (first, last))
        first = last = value
    groups.append(str(first) if first == last else "%d-%d" % (first, last))
    return ",".join(groups)

with open("/proc/%d/stat" % pid, "r", encoding="ascii") as source:
    stat_text = source.read().strip()
right = stat_text.rfind(")")
if right < 0:
    raise SystemExit("malformed stat")
tail = stat_text[right + 2:].split()
if len(tail) < 17:
    raise SystemExit("short stat")
state = tail[0]
parent = int(tail[1])
pgrp = int(tail[2])
session = int(tail[3])
nice = int(tail[16])

policy = None
with open("/proc/%d/sched" % pid, "r", encoding="ascii") as source:
    for line in source:
        match = re.fullmatch(r"\s*policy\s*:\s*([0-9]+)\s*", line)
        if match:
            policy = int(match.group(1))
            break

actual_map = None
with open("/proc/%d/status" % pid, "r", encoding="ascii") as source:
    for line in source:
        if line.startswith("Cpus_allowed_list:"):
            actual_map = line.split(":", 1)[1].strip()
            break

expected_cpus = parse_cpu_list(expected_map)
actual_cpus = parse_cpu_list(actual_map or "")
if (state not in ("T", "t") or parent != expected_parent or pgrp != pid or
        session != pid or policy != 0 or
        nice != expected_nice or actual_cpus != expected_cpus):
    raise SystemExit("priority verification failed state=%s parent=%s pgrp=%s session=%s policy=%s nice=%s affinity=%s" %
                     (state, parent, pgrp, session, policy, nice, actual_map))
print("IO_PRIORITY_VERIFIED pid=%d ppid=%d pgid=%d sid=%d policy=%d nice=%d affinity=%s" %
      (pid, parent, pgrp, session, policy, nice, format_cpu_list(actual_cpus)))
PY
}

start_bound_io()
{
    local map=$1 output=$2 error=$3 work_mode=$4 artifact=$5 artifact_identity=$6
    shift 6
    [ "$#" -gt 0 ] || die bound_io_command_missing
    [ -n "$output" ] && [ -n "$error" ] &&
        [ ! -e "$output" ] && [ ! -L "$output" ] &&
        [ ! -e "$error" ] && [ ! -L "$error" ] ||
        die "unsafe_bound_io_output=$output:$error"
    [ -z "${CELL_IO_PID:-}" ] || die bound_io_already_active

    EXITOS_BOUND_IO_CPU_MAP=$map EXITOS_BOUND_IO_NICE=$IO_NICE \
        exec_bound_objects "$work_mode" "$artifact" "$artifact_identity" "$@" \
        >"$output" 2>"$error" &
    CELL_IO_PID=$!
    CELL_IO_PGID=$CELL_IO_PID
    if ! verify_bound_io_ready "$CELL_IO_PID" "$map" "$output"; then
        sed -n '1,200p' "$output" >&2
        sed -n '1,200p' "$error" >&2
        cleanup_active_cell
        die "bound_io_priority_not_verified output=$output error=$error"
    fi
    kill -CONT "$CELL_IO_PID" 2>/dev/null || {
        cleanup_active_cell
        die bound_io_resume_failed
    }
}

run_test_bound_workfiles()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 2 ] || die test_bound_workfiles_usage
    retain_owned_workdir "$1"
    (exec_bound_objects unsealed - - "$2")
}

run_test_bound_artifact()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 3 ] || die test_bound_artifact_usage
    (exec_bound_objects none "$1" "$2" "$3")
}

run_test_bound_parent_death()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_bound_parent_death_usage
    [ -x "$1" ] || die test_bound_parent_command_not_executable
    exec_bound_objects none - - "$1" &
    wait "$!"
}

run_test_priority_launch()
{
    local map=$1 output=$2 command=$3 rc
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 3 ] || die test_priority_launch_usage
    [ -x "$command" ] || die test_priority_command_not_executable
    umask 077
    CELL_IO_PID=
    CELL_IO_PGID=
    start_bound_io "$map" "$output" "$output.stderr" none - - "$command"
    if wait "$CELL_IO_PID"; then
        rc=0
    else
        rc=$?
    fi
    CELL_IO_PID=
    CELL_IO_PGID=
    [ "$rc" -eq 0 ] || die "test_priority_command_rc=$rc"
}

bound_io_group_has_live_process()
{
    local pgid=$1
    python3 - "$pgid" <<'PY'
import pathlib
import sys
pgid=int(sys.argv[1])
for entry in pathlib.Path("/proc").iterdir():
    if not entry.name.isdecimal():
        continue
    try:
        text=(entry/"stat").read_text(encoding="ascii")
    except (FileNotFoundError,PermissionError,ProcessLookupError):
        continue
    right=text.rfind(")")
    if right < 0:
        continue
    fields=text[right+2:].split()
    if len(fields) >= 3 and fields[0] != "Z" and int(fields[2]) == pgid:
        raise SystemExit(0)
raise SystemExit(1)
PY
}

finish_bound_io_group_after_wait()
{
    local pid=${CELL_IO_PID:-} pgid=${CELL_IO_PGID:-} survivor=0
    [[ $pid =~ ^[0-9]+$ && $pgid =~ ^[0-9]+$ ]] &&
        [ "$pid" = "$pgid" ] && [ "$pgid" -gt 1 ] && [ "$pgid" != "$$" ] ||
        die invalid_bound_io_group_after_wait
    if bound_io_group_has_live_process "$pgid"; then
        survivor=1
        kill -TERM -- "-$pgid" 2>/dev/null || true
        for _ in $(seq 1 100); do
            bound_io_group_has_live_process "$pgid" || break
            sleep 0.01
        done
        if bound_io_group_has_live_process "$pgid"; then
            kill -KILL -- "-$pgid" 2>/dev/null || true
            for _ in $(seq 1 100); do
                bound_io_group_has_live_process "$pgid" || break
                sleep 0.01
            done
        fi
        bound_io_group_has_live_process "$pgid" &&
            die bound_io_group_cleanup_failed
    fi
    CELL_IO_PID=
    CELL_IO_PGID=
    [ "$survivor" -eq 0 ]
}

run_test_bound_group_exit()
{
    local command=$1 map rc group_clean=0
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] && [ -x "$command" ] || die test_bound_group_exit_usage
    map=$(awk '/^Cpus_allowed_list:/ {print $2; exit}' /proc/self/status)
    map=${map%%,*}
    map=${map%%-*}
    start_bound_io "$map" /tmp/exitos-bound-group-$$.out \
        /tmp/exitos-bound-group-$$.err none - - "$command"
    if wait "$CELL_IO_PID"; then rc=0; else rc=$?; fi
    finish_bound_io_group_after_wait || group_clean=$?
    [ "$rc" -eq 0 ] || die "test_bound_group_leader_rc=$rc"
    [ "$group_clean" -eq 0 ] || die bound_io_group_not_empty
}

freeze_generation_value()
{
    CAMPAIGN_GENERATION=$1
    [ -n "$CAMPAIGN_GENERATION" ] || die empty_campaign_generation
    CAMPAIGN_GENERATION_SHA=$(printf '%s' "$CAMPAIGN_GENERATION" |
        sha256sum | awk '{print $1}')
    export CAMPAIGN_GENERATION CAMPAIGN_GENERATION_SHA
}

assert_generation_value()
{
    local current=$1
    [ "$current" = "$CAMPAIGN_GENERATION" ] ||
        die "generation_drift frozen=$CAMPAIGN_GENERATION_SHA"
}

capture_required()
{
    local destination=$1 captured
    shift
    captured=$("$@") || return 1
    [ -n "$captured" ] || return 1
    printf -v "$destination" '%s' "$captured"
}

# Consume the retained, authenticated v1 record produced by
# cpufreq-multicore-transaction.sh.  Parsing and validation are deliberately
# data-only: no attestation text is ever interpreted by the shell.  The same
# probe is used at campaign start, generation checks, and both cell boundaries.
cpufreq_attestation_probe()
{
    local phase=$1 observation=$2
    local expected_attest=${CPUFREQ_ATTESTATION_SHA256:--}
    local expected_profile=${CPUFREQ_LIVE_PROFILE_SHA256:--}
    local expected_identity=${CPUFREQ_ATTESTATION_IDENTITY:--}

    [ -n "${CPUFREQ_ATTEST_FD:-}" ] || {
        echo cpufreq_attestation_fd_missing >&2
        return 1
    }
    [[ $CPUFREQ_ATTEST_FD =~ ^[0-9]+$ ]] || {
        echo cpufreq_attestation_fd_malformed >&2
        return 1
    }
    [ -n "${CPUFREQ_ATTEST_TOKEN:-}" ] || {
        echo cpufreq_attestation_token_missing >&2
        return 1
    }

    python3 - "$CPUFREQ_ATTEST_FD" "$CPUFREQ_ATTEST_TOKEN" \
        "$CPUFREQ_SYSROOT" "$CPUFREQ_LOCK_ROOT" "$CPU_POOL" \
        "$CPUFREQ_TARGET_KHZ" "$phase" "$observation" \
        "$expected_attest" "$expected_profile" "$expected_identity" <<'PY'
import errno
import fcntl
import hashlib
import os
import re
import stat
import sys
import time

(fd_text, token, sysroot_input, lockroot_input, requested_pool,
 target_text, phase, observation_path, expected_attest,
 expected_profile, expected_identity) = sys.argv[1:]

def fail(code):
    print(code, file=sys.stderr)
    raise SystemExit(4)

def test_pause(point):
    if os.environ.get("EXITOS_SCALE_TEST_MODE") != "1":
        return
    if os.environ.get("EXITOS_SCALE_TEST_CPUFREQ_PAUSE_POINT") != point:
        return
    marker = os.environ.get("EXITOS_SCALE_TEST_CPUFREQ_PAUSE_MARKER", "")
    if not marker or not os.path.isabs(marker) or marker.endswith("/"):
        fail("cpufreq_test_pause_marker_invalid")
    parent = os.path.dirname(marker)
    try:
        if os.path.realpath(parent, strict=True) != parent:
            fail("cpufreq_test_pause_marker_invalid")
        marker_fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                            os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            os.write(marker_fd, (point + "\n").encode("ascii"))
            os.fsync(marker_fd)
        finally:
            os.close(marker_fd)
    except OSError:
        fail("cpufreq_test_pause_marker_invalid")
    resume = marker + ".resume"
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            metadata = os.lstat(resume)
        except FileNotFoundError:
            time.sleep(0.005)
            continue
        except OSError:
            fail("cpufreq_test_pause_resume_invalid")
        if (not stat.S_ISREG(metadata.st_mode) or
                stat.S_ISLNK(metadata.st_mode) or
                metadata.st_uid != os.geteuid() or
                stat.S_IMODE(metadata.st_mode) != 0o600 or
                metadata.st_nlink != 1):
            fail("cpufreq_test_pause_resume_invalid")
        return
    fail("cpufreq_test_pause_timeout")

def decode_one(payload, code):
    if len(payload) > 4096:
        fail(code)
    try:
        value = payload.decode("ascii")
    except UnicodeDecodeError:
        fail(code)
    if not value.endswith("\n") or value.count("\n") != 1:
        fail(code)
    value = value[:-1]
    if not value or "\r" in value or "\x00" in value:
        fail(code)
    return value

def read_one(path, code):
    try:
        value_fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            payload = os.read(value_fd, 4097)
        finally:
            os.close(value_fd)
    except OSError:
        fail(code)
    return decode_one(payload, code)

def read_one_at(directory_fd, name, code):
    if not re.fullmatch(r"[a-z0-9_]+", name):
        fail(code)
    try:
        value_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                           dir_fd=directory_fd)
        try:
            payload = os.read(value_fd, 4097)
        finally:
            os.close(value_fd)
    except OSError:
        fail(code)
    return decode_one(payload, code)

def canonical_decimal(value, code, maximum):
    if not re.fullmatch(r"0|[1-9][0-9]*", value):
        fail(code)
    if len(value) > 10:
        fail(code)
    number = int(value)
    if number > maximum:
        fail(code)
    return number

def parse_csv(value, code, require_sorted=False):
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:,(?:0|[1-9][0-9]*))*", value):
        fail(code)
    numbers = [canonical_decimal(item, code, 1023) for item in value.split(",")]
    if len(numbers) != len(set(numbers)):
        fail(code)
    if require_sorted and numbers != sorted(numbers):
        fail(code)
    return numbers

def parse_linux_list(value, code):
    normalized = value.replace("\n", " ").replace("\r", " ").replace("\t", " ")
    tokens = normalized.replace(",", " ").split()
    if not tokens:
        fail(code)
    numbers = []
    seen = set()
    for item in tokens:
        match = re.fullmatch(r"([0-9]+)(?:-([0-9]+))?", item)
        if not match:
            fail(code)
        first = canonical_decimal(match.group(1), code, 1023)
        last = first if match.group(2) is None else canonical_decimal(
            match.group(2), code, 1023)
        if first > last:
            fail(code)
        for number in range(first, last + 1):
            if number in seen:
                fail(code)
            seen.add(number)
            numbers.append(number)
    return sorted(numbers)

def csv(numbers):
    return ",".join(str(number) for number in numbers)

def read_linux_list(path, code):
    return parse_linux_list(read_one(path, code), code)

def read_linux_list_at(directory_fd, name, code):
    return parse_linux_list(read_one_at(directory_fd, name, code), code)

def cpu_online(cpu):
    cpu_dir = os.path.join(sysroot, "cpu%d" % cpu)
    if not os.path.isdir(cpu_dir):
        fail("cpufreq_cpu_topology_mismatch")
    online_path = os.path.join(cpu_dir, "online")
    if os.path.exists(online_path):
        value = read_one(online_path, "cpufreq_cpu_topology_mismatch")
        if value not in ("0", "1"):
            fail("cpufreq_cpu_topology_mismatch")
        return value == "1"
    return cpu in global_online

def resolve_policy(cpu):
    link = os.path.join(sysroot, "cpu%d" % cpu, "cpufreq")
    try:
        path = os.path.realpath(link, strict=True)
    except OSError:
        fail("cpufreq_policy_path_mismatch")
    parent, name = os.path.split(path)
    match = re.fullmatch(r"policy(0|[1-9][0-9]*)", name)
    if not match:
        fail("cpufreq_policy_name_not_canonical")
    if parent != policy_root or not os.path.isdir(path) or os.path.islink(path):
        fail("cpufreq_policy_path_mismatch")
    return path, int(match.group(1))

if phase not in ("campaign-start", "generation", "pre-cell", "post-cell"):
    fail("cpufreq_attestation_phase_invalid")
if not re.fullmatch(r"[0-9]+", fd_text):
    fail("cpufreq_attestation_fd_malformed")
fd = int(fd_text)
if not (3 <= fd <= 1048575):
    fail("cpufreq_attestation_fd_malformed")
if not re.fullmatch(r"[0-9a-f]{32}", token):
    fail("cpufreq_attestation_token_malformed")
try:
    metadata_before = os.fstat(fd)
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
except OSError:
    fail("cpufreq_attestation_fd_unavailable")
if flags & os.O_ACCMODE != os.O_RDONLY:
    fail("cpufreq_attestation_fd_not_read_only")
if (not stat.S_ISREG(metadata_before.st_mode) or
        metadata_before.st_uid != os.geteuid() or
        stat.S_IMODE(metadata_before.st_mode) != 0o400):
    fail("cpufreq_attestation_fd_bad_metadata")
if metadata_before.st_nlink != 0:
    fail("cpufreq_attestation_fd_not_unlinked")
attestation_identity = "%d:%d:%d:%o:%d:%d:%d:%d" % (
    metadata_before.st_dev, metadata_before.st_ino, metadata_before.st_uid,
    stat.S_IMODE(metadata_before.st_mode), metadata_before.st_nlink,
    metadata_before.st_size, metadata_before.st_mtime_ns,
    metadata_before.st_ctime_ns)
if expected_identity != "-" and attestation_identity != expected_identity:
    fail("cpufreq_attestation_identity_mismatch")
if not (1 <= metadata_before.st_size <= 65536):
    fail("cpufreq_attestation_size_invalid")
try:
    payload = os.pread(fd, 65537, 0)
    metadata_after = os.fstat(fd)
except OSError:
    fail("cpufreq_attestation_fd_unavailable")
identity_before = (metadata_before.st_dev, metadata_before.st_ino,
                   metadata_before.st_uid, stat.S_IMODE(metadata_before.st_mode),
                   metadata_before.st_nlink, metadata_before.st_size)
identity_after = (metadata_after.st_dev, metadata_after.st_ino,
                  metadata_after.st_uid, stat.S_IMODE(metadata_after.st_mode),
                  metadata_after.st_nlink, metadata_after.st_size)
if identity_before != identity_after or len(payload) != metadata_before.st_size:
    fail("cpufreq_attestation_fd_changed")
try:
    text = payload.decode("ascii")
except UnicodeDecodeError:
    fail("cpufreq_attestation_non_ascii")
if not text.endswith("\n") or "\r" in text or "\x00" in text:
    fail("cpufreq_attestation_shape_invalid")
lines = text.splitlines()

header_pattern = re.compile(
    r"EXITOS_CPUFREQ_ATTEST version=1 token=([0-9a-f]{32}) "
    r"requested_cpus=([^ ]+) canonical_selected_cpus=([^ ]+) "
    r"logical_cpus=([^ ]+) policy_count=([0-9]+) target_khz=([0-9]+) "
    r"control_profile_verified=yes "
    r"actual_frequency_verified=post-keeper-pre-fork locks_held=yes")
if not lines:
    fail("cpufreq_attestation_shape_invalid")
header = header_pattern.fullmatch(lines[0])
if not header:
    fail("cpufreq_attestation_shape_invalid")
(header_token, att_requested_text, att_selected_text, att_logical_text,
 policy_count_text, att_target_text) = header.groups()
if header_token != token:
    fail("cpufreq_attestation_token_mismatch")
att_requested = parse_csv(att_requested_text,
                          "cpufreq_requested_cpus_mismatch")
att_selected = parse_csv(att_selected_text,
                         "cpufreq_canonical_selected_cpus_mismatch", True)
att_logical = parse_csv(att_logical_text,
                        "cpufreq_logical_cpus_mismatch", True)
policy_count = canonical_decimal(policy_count_text,
                                 "cpufreq_attestation_shape_invalid", 1024)
att_target = canonical_decimal(att_target_text,
                               "cpufreq_target_khz_mismatch", 100000000)
target = canonical_decimal(target_text, "cpufreq_target_khz_mismatch", 100000000)
if att_target != target:
    fail("cpufreq_target_khz_mismatch")
if len(lines) != policy_count + 2:
    fail("cpufreq_attestation_shape_invalid")

try:
    sysroot = os.path.realpath(sysroot_input, strict=True)
    lockroot = os.path.realpath(lockroot_input, strict=True)
except OSError:
    fail("cpufreq_validation_root_invalid")
if (sysroot != sysroot_input or lockroot != lockroot_input or
        not os.path.isdir(sysroot) or os.path.islink(sysroot) or
        not os.path.isdir(lockroot) or os.path.islink(lockroot)):
    fail("cpufreq_validation_root_invalid")
policy_root = os.path.join(sysroot, "cpufreq")
if not os.path.isdir(policy_root) or os.path.islink(policy_root):
    fail("cpufreq_policy_path_mismatch")
global_online = set(read_linux_list(os.path.join(sysroot, "online"),
                                    "cpufreq_cpu_topology_mismatch"))
requested = parse_csv(requested_pool, "cpufreq_requested_cpus_mismatch")
if att_requested != requested or att_requested_text != requested_pool:
    fail("cpufreq_requested_cpus_mismatch")

core_groups = []
all_siblings = set()
logical = set()
for selected in requested:
    if not cpu_online(selected):
        fail("cpufreq_cpu_topology_mismatch")
    siblings = read_linux_list(
        os.path.join(sysroot, "cpu%d" % selected, "topology",
                     "thread_siblings_list"),
        "cpufreq_cpu_topology_mismatch")
    if selected not in siblings:
        fail("cpufreq_cpu_topology_mismatch")
    sibling_set = set(siblings)
    if sibling_set & all_siblings:
        fail("cpufreq_cpu_topology_mismatch")
    all_siblings.update(sibling_set)
    core_groups.append(siblings)
    for sibling in siblings:
        if not cpu_online(sibling):
            continue
        sibling_view = read_linux_list(
            os.path.join(sysroot, "cpu%d" % sibling, "topology",
                         "thread_siblings_list"),
            "cpufreq_cpu_topology_mismatch")
        if sibling_view != siblings:
            fail("cpufreq_cpu_topology_mismatch")
        logical.add(sibling)

canonical_selected = sorted(group[0] for group in core_groups)
logical_sorted = sorted(logical)
if att_selected != canonical_selected:
    fail("cpufreq_canonical_selected_cpus_mismatch")
if att_logical != logical_sorted:
    fail("cpufreq_logical_cpus_mismatch")

cpu_policy = {}
path_numbers = {}
number_paths = {}
for cpu in logical_sorted:
    path, number = resolve_policy(cpu)
    cpu_policy[cpu] = path
    path_numbers[path] = number
    previous = number_paths.get(number)
    if previous is not None and previous != path:
        fail("cpufreq_policy_name_not_canonical")
    number_paths[number] = path
expected_paths = sorted(path_numbers, key=lambda path: path_numbers[path])
if policy_count != len(expected_paths):
    fail("cpufreq_policy_count_mismatch")

policy_pattern = re.compile(
    r"EXITOS_CPUFREQ_ATTEST_POLICY index=([0-9]+) path=([^ ]+) "
    r"identity=([0-9]+:[0-9]+) related_cpus=([^ ]+) "
    r"cpuinfo_max_khz=([0-9]+) governor=performance epp=performance "
    r"min_khz=([0-9]+) max_khz=([0-9]+)")
scope_owner = {}
policy_state = {}
profile_lines = [
    "format=exitos-cpufreq-live-profile-v1",
    "requested_cpus=" + requested_pool,
    "canonical_selected_cpus=" + csv(canonical_selected),
    "logical_cpus=" + csv(logical_sorted),
    "policy_count=%d" % policy_count,
    "target_khz=%d" % target,
]
for offset, path in enumerate(expected_paths, 1):
    match = policy_pattern.fullmatch(lines[offset])
    if not match:
        fail("cpufreq_attestation_shape_invalid")
    (index_text, att_path, att_identity, att_scope_text,
     att_info_text, att_min_text, att_max_text) = match.groups()
    index = canonical_decimal(index_text,
                              "cpufreq_attestation_shape_invalid", 1024)
    if index != offset:
        fail("cpufreq_attestation_shape_invalid")
    att_name = os.path.basename(att_path)
    name_match = re.fullmatch(r"policy(0|[1-9][0-9]*)", att_name)
    if not name_match:
        fail("cpufreq_policy_name_not_canonical")
    if att_path != path:
        fail("cpufreq_policy_path_mismatch")
    try:
        policy_path_metadata = os.lstat(path)
        if (not stat.S_ISDIR(policy_path_metadata.st_mode) or
                stat.S_ISLNK(policy_path_metadata.st_mode)):
            fail("cpufreq_policy_identity_mismatch")
        policy_fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY |
                            os.O_NOFOLLOW | os.O_CLOEXEC)
        policy_fd_metadata = os.fstat(policy_fd)
    except OSError:
        fail("cpufreq_policy_identity_mismatch")
    path_identity = (policy_path_metadata.st_dev, policy_path_metadata.st_ino)
    fd_identity = (policy_fd_metadata.st_dev, policy_fd_metadata.st_ino)
    if path_identity != fd_identity or not stat.S_ISDIR(policy_fd_metadata.st_mode):
        fail("cpufreq_policy_identity_mismatch")
    live_identity = "%d:%d" % fd_identity
    if att_identity != live_identity:
        fail("cpufreq_policy_identity_mismatch")
    test_pause("policy-after-identity:" + att_name)
    live_scope = read_linux_list_at(policy_fd, "related_cpus",
                                    "cpufreq_policy_scope_mismatch")
    att_scope = parse_csv(att_scope_text,
                          "cpufreq_policy_scope_mismatch", True)
    if att_scope != live_scope or att_scope_text != csv(live_scope):
        fail("cpufreq_policy_scope_mismatch")
    for cpu in live_scope:
        if cpu not in all_siblings or cpu in scope_owner:
            fail("cpufreq_policy_scope_mismatch")
        scope_owner[cpu] = path
    info = read_one_at(policy_fd, "cpuinfo_max_freq",
                       "cpufreq_cpuinfo_max_mismatch")
    info_value = canonical_decimal(info, "cpufreq_cpuinfo_max_mismatch",
                                   100000000)
    att_info = canonical_decimal(att_info_text,
                                 "cpufreq_cpuinfo_max_mismatch", 100000000)
    if info_value != target or att_info != target:
        fail("cpufreq_cpuinfo_max_mismatch")
    governor = read_one_at(policy_fd, "scaling_governor",
                           "cpufreq_governor_mismatch")
    epp = read_one_at(policy_fd, "energy_performance_preference",
                      "cpufreq_epp_mismatch")
    minimum_text = read_one_at(policy_fd, "scaling_min_freq",
                               "cpufreq_min_khz_mismatch")
    maximum_text = read_one_at(policy_fd, "scaling_max_freq",
                               "cpufreq_max_khz_mismatch")
    minimum = canonical_decimal(minimum_text, "cpufreq_min_khz_mismatch",
                                100000000)
    maximum = canonical_decimal(maximum_text, "cpufreq_max_khz_mismatch",
                                100000000)
    att_minimum = canonical_decimal(att_min_text,
                                    "cpufreq_min_khz_mismatch", 100000000)
    att_maximum = canonical_decimal(att_max_text,
                                    "cpufreq_max_khz_mismatch", 100000000)
    if governor != "performance":
        fail("cpufreq_governor_mismatch")
    if epp != "performance":
        fail("cpufreq_epp_mismatch")
    if minimum != target or att_minimum != target:
        fail("cpufreq_min_khz_mismatch")
    if maximum != target or att_maximum != target:
        fail("cpufreq_max_khz_mismatch")
    policy_state[path] = {
        "fd": policy_fd,
        "identity": fd_identity,
        "scope": live_scope,
        "cpuinfo_max": info,
        "governor": governor,
        "epp": epp,
        "minimum": minimum_text,
        "maximum": maximum_text,
    }
    profile_lines.append(
        "policy=%d path=%s identity=%s related_cpus=%s "
        "cpuinfo_max_khz=%d governor=performance epp=performance "
        "min_khz=%d max_khz=%d" %
        (offset, path, live_identity, csv(live_scope), info_value,
         minimum, maximum))

for cpu in logical_sorted:
    if scope_owner.get(cpu) != cpu_policy[cpu]:
        fail("cpufreq_policy_scope_mismatch")

lock_namespace = os.path.join(lockroot, "exitos-cpufreq")
try:
    namespace_metadata = os.lstat(lock_namespace)
    namespace_fd = os.open(lock_namespace, os.O_RDONLY | os.O_DIRECTORY |
                           os.O_NOFOLLOW | os.O_CLOEXEC)
    namespace_fd_metadata = os.fstat(namespace_fd)
except OSError:
    fail("cpufreq_policy_lock_not_held")
if (not stat.S_ISDIR(namespace_metadata.st_mode) or
        stat.S_ISLNK(namespace_metadata.st_mode) or
        namespace_metadata.st_uid != os.geteuid() or
        stat.S_IMODE(namespace_metadata.st_mode) != 0o700 or
        (namespace_metadata.st_dev, namespace_metadata.st_ino) !=
        (namespace_fd_metadata.st_dev, namespace_fd_metadata.st_ino)):
    fail("cpufreq_policy_lock_not_held")
for path in expected_paths:
    lock_name = os.path.basename(path) + ".lock"
    lock_path = os.path.join(lock_namespace, lock_name)
    try:
        lock_metadata = os.stat(lock_name, dir_fd=namespace_fd,
                                follow_symlinks=False)
        if (not stat.S_ISREG(lock_metadata.st_mode) or
                stat.S_ISLNK(lock_metadata.st_mode) or
                lock_metadata.st_uid != os.geteuid() or
                stat.S_IMODE(lock_metadata.st_mode) != 0o600 or
                lock_metadata.st_nlink != 1):
            fail("cpufreq_policy_lock_not_held")
        test_pause("lock-after-lstat:" + os.path.basename(path))
        lock_fd = os.open(lock_name, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC,
                          dir_fd=namespace_fd)
        lock_fd_metadata = os.fstat(lock_fd)
    except OSError:
        fail("cpufreq_policy_lock_not_held")
    lock_identity = (lock_metadata.st_dev, lock_metadata.st_ino,
                     lock_metadata.st_uid, stat.S_IMODE(lock_metadata.st_mode),
                     lock_metadata.st_nlink)
    opened_lock_identity = (
        lock_fd_metadata.st_dev, lock_fd_metadata.st_ino,
        lock_fd_metadata.st_uid, stat.S_IMODE(lock_fd_metadata.st_mode),
        lock_fd_metadata.st_nlink)
    if (not stat.S_ISREG(lock_fd_metadata.st_mode) or
            opened_lock_identity != lock_identity):
        os.close(lock_fd)
        fail("cpufreq_policy_lock_identity_mismatch")
    try:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in (errno.EACCES, errno.EAGAIN):
                fail("cpufreq_policy_lock_not_held")
        else:
            fcntl.flock(lock_fd, fcntl.LOCK_UN)
            fail("cpufreq_policy_lock_not_held")
        try:
            final_path_metadata = os.stat(lock_name, dir_fd=namespace_fd,
                                          follow_symlinks=False)
            final_fd_metadata = os.fstat(lock_fd)
        except OSError:
            fail("cpufreq_policy_lock_identity_mismatch")
        final_path_identity = (
            final_path_metadata.st_dev, final_path_metadata.st_ino,
            final_path_metadata.st_uid, stat.S_IMODE(final_path_metadata.st_mode),
            final_path_metadata.st_nlink)
        final_fd_identity = (
            final_fd_metadata.st_dev, final_fd_metadata.st_ino,
            final_fd_metadata.st_uid, stat.S_IMODE(final_fd_metadata.st_mode),
            final_fd_metadata.st_nlink)
        if (final_path_identity != lock_identity or
                final_fd_identity != lock_identity):
            fail("cpufreq_policy_lock_identity_mismatch")
    finally:
        os.close(lock_fd)
try:
    final_namespace_path = os.lstat(lock_namespace)
    final_namespace_fd = os.fstat(namespace_fd)
except OSError:
    fail("cpufreq_policy_lock_identity_mismatch")
if ((final_namespace_path.st_dev, final_namespace_path.st_ino) !=
        (namespace_metadata.st_dev, namespace_metadata.st_ino) or
        (final_namespace_fd.st_dev, final_namespace_fd.st_ino) !=
        (namespace_metadata.st_dev, namespace_metadata.st_ino)):
    fail("cpufreq_policy_lock_identity_mismatch")
os.close(namespace_fd)
profile_lines.append("locks_held=yes")

actual_pattern = re.compile(
    r"EXITOS_CPUFREQ_ATTEST_ACTUAL logical_cpus=([^ ]+) "
    r"tolerance_khz=([0-9]+) stable_samples=([0-9]+) loaded_khz=([^ ]+) "
    r"concurrent_keeper_verification=yes keepers_stopped_before_final=yes "
    r"verification_scope=post-keeper-pre-fork")
actual_match = actual_pattern.fullmatch(lines[-1])
if not actual_match:
    fail("cpufreq_attestation_shape_invalid")
actual_text, tolerance_text, stable_text, loaded_text = actual_match.groups()

def parse_frequency_map(value):
    if not re.fullmatch(
            r"(?:0|[1-9][0-9]*):(?:0|[1-9][0-9]*)(?:,(?:0|[1-9][0-9]*):(?:0|[1-9][0-9]*))*",
            value):
        fail("cpufreq_attestation_shape_invalid")
    result = []
    seen = set()
    for item in value.split(","):
        cpu_text, frequency_text = item.split(":", 1)
        cpu = canonical_decimal(cpu_text, "cpufreq_attestation_shape_invalid", 1023)
        frequency = canonical_decimal(frequency_text,
                                      "cpufreq_attestation_shape_invalid",
                                      100000000)
        if cpu in seen:
            fail("cpufreq_attestation_shape_invalid")
        seen.add(cpu)
        result.append((cpu, frequency))
    if [cpu for cpu, _frequency in result] != logical_sorted:
        fail("cpufreq_logical_cpus_mismatch")
    return result

actual_map = parse_frequency_map(actual_text)
loaded_map = parse_frequency_map(loaded_text)
tolerance = canonical_decimal(tolerance_text,
                              "cpufreq_attestation_shape_invalid", 100000000)
stable_samples = canonical_decimal(stable_text,
                                   "cpufreq_attestation_shape_invalid", 1000)
required_tolerance = max((target + 99) // 100, 10000)
if tolerance != required_tolerance or stable_samples != 3:
    fail("cpufreq_attestation_shape_invalid")
for _cpu, frequency in actual_map + loaded_map:
    if abs(frequency - target) > tolerance:
        fail("cpufreq_attested_actual_mismatch")

def validate_policy_end_state():
    final_online = set(read_linux_list(os.path.join(sysroot, "online"),
                                       "cpufreq_cpu_topology_mismatch"))
    if final_online != global_online:
        fail("cpufreq_cpu_topology_mismatch")
    for siblings in core_groups:
        for sibling in siblings:
            is_online = cpu_online(sibling)
            if is_online != (sibling in logical):
                fail("cpufreq_cpu_topology_mismatch")
            if is_online:
                sibling_view = read_linux_list(
                    os.path.join(sysroot, "cpu%d" % sibling, "topology",
                                 "thread_siblings_list"),
                    "cpufreq_cpu_topology_mismatch")
                if sibling_view != siblings:
                    fail("cpufreq_cpu_topology_mismatch")
    for path in expected_paths:
        state = policy_state[path]
        try:
            path_metadata = os.lstat(path)
            fd_metadata = os.fstat(state["fd"])
        except OSError:
            fail("cpufreq_policy_identity_mismatch")
        if (not stat.S_ISDIR(path_metadata.st_mode) or
                stat.S_ISLNK(path_metadata.st_mode) or
                not stat.S_ISDIR(fd_metadata.st_mode) or
                (path_metadata.st_dev, path_metadata.st_ino) !=
                state["identity"] or
                (fd_metadata.st_dev, fd_metadata.st_ino) !=
                state["identity"]):
            fail("cpufreq_policy_identity_mismatch")
        final_scope = read_linux_list_at(
            state["fd"], "related_cpus", "cpufreq_policy_scope_mismatch")
        if final_scope != state["scope"]:
            fail("cpufreq_policy_scope_mismatch")
        final_fields = (
            ("cpuinfo_max_freq", "cpufreq_cpuinfo_max_mismatch", "cpuinfo_max"),
            ("scaling_governor", "cpufreq_governor_mismatch", "governor"),
            ("energy_performance_preference", "cpufreq_epp_mismatch", "epp"),
            ("scaling_min_freq", "cpufreq_min_khz_mismatch", "minimum"),
            ("scaling_max_freq", "cpufreq_max_khz_mismatch", "maximum"),
        )
        for field, code, key in final_fields:
            if read_one_at(state["fd"], field, code) != state[key]:
                fail(code)
    for cpu in logical_sorted:
        expected_path = cpu_policy[cpu]
        link = os.path.join(sysroot, "cpu%d" % cpu, "cpufreq")
        try:
            resolved_before = os.path.realpath(link, strict=True)
            link_fd = os.open(link, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            try:
                linked_metadata = os.fstat(link_fd)
            finally:
                os.close(link_fd)
            resolved_after = os.path.realpath(link, strict=True)
        except OSError:
            fail("cpufreq_policy_path_mismatch")
        if resolved_before != expected_path or resolved_after != expected_path:
            fail("cpufreq_policy_path_mismatch")
        expected_identity = policy_state[expected_path]["identity"]
        if ((linked_metadata.st_dev, linked_metadata.st_ino) !=
                expected_identity):
            fail("cpufreq_policy_identity_mismatch")

validate_policy_end_state()

profile_payload = ("\n".join(profile_lines) + "\n").encode("ascii")
# Content digests are minted once at campaign initialization.  Repeated
# generation/cell probes validate the retained object's frozen identity and
# compare every attested/live field directly; they reuse the milestone labels
# and never rehash the same records per cell.
if expected_attest == "-" and expected_profile == "-":
    attestation_sha = hashlib.sha256(payload).hexdigest()
    profile_sha = hashlib.sha256(profile_payload).hexdigest()
elif (re.fullmatch(r"[0-9a-f]{64}", expected_attest) and
      re.fullmatch(r"[0-9a-f]{64}", expected_profile)):
    attestation_sha = expected_attest
    profile_sha = expected_profile
else:
    fail("cpufreq_milestone_hash_contract_malformed")

if observation_path != "-":
    if not os.path.isabs(observation_path):
        fail("cpufreq_boundary_output_unsafe")
    parent = os.path.dirname(observation_path)
    try:
        if os.path.realpath(parent, strict=True) != parent or not os.path.isdir(parent):
            fail("cpufreq_boundary_output_unsafe")
    except OSError:
        fail("cpufreq_boundary_output_unsafe")
    observations = []
    for cpu in logical_sorted:
        policy_fd = policy_state[cpu_policy[cpu]]["fd"]
        current = "unavailable"
        try:
            current_fd = os.open("scaling_cur_freq",
                                 os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                                 dir_fd=policy_fd)
            try:
                current_payload = os.read(current_fd, 4097)
            finally:
                os.close(current_fd)
            raw_current = current_payload.decode("ascii")
            if (len(current_payload) <= 4096 and
                    raw_current.endswith("\n") and
                    raw_current.count("\n") == 1 and
                    re.fullmatch(r"0|[1-9][0-9]*", raw_current[:-1]) and
                    len(raw_current[:-1]) <= 9 and
                    int(raw_current[:-1]) <= 100000000):
                current = raw_current[:-1]
        except (OSError, UnicodeError):
            pass
        observations.append("%d:%s" % (cpu, current))
    validate_policy_end_state()
    observation = (
        "format=exitos-cpufreq-boundary-v1\n"
        "phase=%s\n"
        "cpufreq_attestation_sha256=%s\n"
        "cpufreq_live_profile_sha256=%s\n"
        "target_khz=%d\n"
        "logical_cpus=%s\n"
        "boundary_actual_khz=%s\n"
        "frequency_claim=boundary_observation_only_not_continuous_hardware_frequency\n" %
        (phase, attestation_sha, profile_sha, target, csv(logical_sorted),
         ",".join(observations))).encode("ascii")
    try:
        output_fd = os.open(observation_path,
                            os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                            os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            view = memoryview(observation)
            while view:
                written = os.write(output_fd, view)
                if written <= 0:
                    fail("cpufreq_boundary_output_write_failed")
                view = view[written:]
            os.fsync(output_fd)
        finally:
            os.close(output_fd)
    except OSError:
        fail("cpufreq_boundary_output_unsafe")

for state in policy_state.values():
    os.close(state["fd"])

print("CPUFREQ_ATTESTATION_RECORD\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s" %
      (attestation_sha, profile_sha, requested_pool, csv(canonical_selected),
       csv(logical_sorted), policy_count, target, attestation_identity))
PY
}

decode_cpufreq_probe_record()
{
    local record=$1 tag attest_sha profile_sha requested selected logical
    local policies target identity extra
    IFS=$'\t' read -r tag attest_sha profile_sha requested selected logical \
        policies target identity extra <<<"$record"
    [ "$tag" = CPUFREQ_ATTESTATION_RECORD ] &&
        [[ $attest_sha =~ ^[0-9a-f]{64}$ ]] &&
        [[ $profile_sha =~ ^[0-9a-f]{64}$ ]] &&
        [ -n "$requested" ] && [ -n "$selected" ] && [ -n "$logical" ] &&
        [[ $policies =~ ^[1-9][0-9]*$ ]] && [[ $target =~ ^[1-9][0-9]*$ ]] &&
        [[ $identity =~ ^[0-9]+:[0-9]+:[0-9]+:[0-7]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+$ ]] &&
        [ -z "${extra:-}" ] || return 1
    CPUFREQ_ATTESTATION_SHA256=$attest_sha
    CPUFREQ_LIVE_PROFILE_SHA256=$profile_sha
    CPUFREQ_REQUESTED_CPUS=$requested
    CPUFREQ_CANONICAL_SELECTED_CPUS=$selected
    CPUFREQ_LOGICAL_CPUS=$logical
    CPUFREQ_POLICY_COUNT=$policies
    CPUFREQ_ATTESTATION_IDENTITY=$identity
    [ "$target" = "$CPUFREQ_TARGET_KHZ" ] || return 1
}

initialize_cpufreq_attestation()
{
    local record
    CPUFREQ_ATTEST_FD=${EXITOS_CPUFREQ_ATTEST_FD:-}
    CPUFREQ_ATTEST_TOKEN=${EXITOS_CPUFREQ_ATTEST_TOKEN:-}
    record=$(cpufreq_attestation_probe campaign-start -) ||
        die cpufreq_attestation_validation_failed_phase=campaign-start
    decode_cpufreq_probe_record "$record" ||
        die cpufreq_attestation_record_malformed
    export CPUFREQ_ATTESTATION_SHA256 CPUFREQ_LIVE_PROFILE_SHA256 \
        CPUFREQ_ATTESTATION_IDENTITY
    printf 'CPUFREQ_ATTESTATION_READY version=1 attestation_sha256=%s live_profile_sha256=%s requested_cpus=%s canonical_selected_cpus=%s logical_cpus=%s policies=%s target_khz=%s\n' \
        "$CPUFREQ_ATTESTATION_SHA256" "$CPUFREQ_LIVE_PROFILE_SHA256" \
        "$CPUFREQ_REQUESTED_CPUS" "$CPUFREQ_CANONICAL_SELECTED_CPUS" \
        "$CPUFREQ_LOGICAL_CPUS" "$CPUFREQ_POLICY_COUNT" "$CPUFREQ_TARGET_KHZ"
}

require_cpufreq_attestation_for_mode()
{
    case "$1" in
        --pilot-endpoints|--pilot-factorial)
            initialize_cpufreq_attestation
            ;;
        --plan|--prepare|--pilot-thread|--pilot-process|--pilot-all|--pilot-endpoints-thread|--pilot-endpoints-process|--pilot-endpoints-all)
            ;;
        *) die "cpufreq_mode_invalid=$1" ;;
    esac
}

assert_cpufreq_attestation_live()
{
    local phase=$1 record old_attest=$CPUFREQ_ATTESTATION_SHA256
    local old_profile=$CPUFREQ_LIVE_PROFILE_SHA256
    record=$(cpufreq_attestation_probe "$phase" -) ||
        die "cpufreq_attestation_validation_failed_phase=$phase"
    decode_cpufreq_probe_record "$record" ||
        die cpufreq_attestation_record_malformed
    [ "$CPUFREQ_ATTESTATION_SHA256" = "$old_attest" ] &&
        [ "$CPUFREQ_LIVE_PROFILE_SHA256" = "$old_profile" ] ||
        die "cpufreq_attestation_generation_drift_phase=$phase"
}

cpufreq_generation_snapshot()
{
    assert_cpufreq_attestation_live generation
    printf 'cpufreq_attestation_sha256=%s\n' \
        "$CPUFREQ_ATTESTATION_SHA256"
    printf 'cpufreq_live_profile_sha256=%s\n' \
        "$CPUFREQ_LIVE_PROFILE_SHA256"
    printf 'cpufreq_target_khz=%s\ncpufreq_logical_cpus=%s\n' \
        "$CPUFREQ_TARGET_KHZ" "$CPUFREQ_LOGICAL_CPUS"
}

verify_cpufreq_cell()
{
    local phase=$1 observation=$2 meta=${3:--} record
    local old_attest=$CPUFREQ_ATTESTATION_SHA256
    local old_profile=$CPUFREQ_LIVE_PROFILE_SHA256
    record=$(cpufreq_attestation_probe "$phase" "$observation") ||
        die "cpufreq_attestation_validation_failed_phase=$phase"
    decode_cpufreq_probe_record "$record" ||
        die cpufreq_attestation_record_malformed
    [ "$CPUFREQ_ATTESTATION_SHA256" = "$old_attest" ] &&
        [ "$CPUFREQ_LIVE_PROFILE_SHA256" = "$old_profile" ] ||
        die "cpufreq_attestation_generation_drift_phase=$phase"
    if [ "$meta" != - ]; then
        printf 'cpufreq_attestation_version=1\ncpufreq_attestation_sha256=%s\ncpufreq_live_profile_sha256=%s\ncpufreq_target_khz=%s\ncpufreq_logical_cpus=%s\ncpufreq_boundary_frequency_claim=point_samples_only_not_continuous_hardware_frequency\n' \
            "$CPUFREQ_ATTESTATION_SHA256" "$CPUFREQ_LIVE_PROFILE_SHA256" \
            "$CPUFREQ_TARGET_KHZ" "$CPUFREQ_LOGICAL_CPUS" >>"$meta"
    fi
    printf 'CPUFREQ_CELL_PROFILE_OK phase=%s attestation_sha256=%s live_profile_sha256=%s boundary_artifact=%s\n' \
        "$phase" "$CPUFREQ_ATTESTATION_SHA256" \
        "$CPUFREQ_LIVE_PROFILE_SHA256" "$observation"
}

cpufreq_cell_pre_gate()
{
    local window_mode=$1 map=$2 cpu_window=$3 observation=$4 meta=$5

    case "$window_mode" in
        production)
            cpu_quiet_window "$map" "$cpu_window"
            ;;
        test-fake)
            [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
            [ -n "${EXITOS_SCALE_TEST_CPU_WINDOW_HELPER:-}" ] &&
                [[ $EXITOS_SCALE_TEST_CPU_WINDOW_HELPER == /* ]] &&
                [ -x "$EXITOS_SCALE_TEST_CPU_WINDOW_HELPER" ] ||
                die test_cpufreq_cpu_window_helper_invalid
            "$EXITOS_SCALE_TEST_CPU_WINDOW_HELPER" "$cpu_window" ||
                die test_cpufreq_cpu_window_failed
            [ -f "$cpu_window" ] && [ ! -L "$cpu_window" ] ||
                die test_cpufreq_cpu_window_missing
            ;;
        *) die "cpufreq_cpu_window_mode_invalid=$window_mode" ;;
    esac
    verify_cpufreq_cell pre-cell "$observation" "$meta"
}

cpufreq_cell_post_gate()
{
    local observation=$1
    verify_cpufreq_cell post-cell "$observation"
}

campaign_generation_snapshot()
{
    local map cpu online pkg core siblings ctrl_path driver_path mount_id
    local queue_path queue cpu_list poll_queues io_poll
    local boot_id whole_dev_t part_dev_t diskseq nsid mount_source_object
    local controller_char_dev_t namespace_char_dev_t
    local namespace_char_retained_identity namespace_char_path_identity
    local online_path
    local cpus=() queue_paths=()

    if [ -n "${CPUFREQ_ATTESTATION_SHA256:-}" ]; then
        cpufreq_generation_snapshot
    fi

    map=$(cpu_map "$MAX_JOBS") || return 1
    ctrl_path=$(readlink -f -- "/sys/class/nvme/$CTRL/device") || return 1
    driver_path=$(readlink -f -- "/sys/class/nvme/$CTRL/device/driver") || return 1
    mount_id=$(findmnt -rn -T "$MNT" -o ID) || return 1
    capture_required boot_id cat -- /proc/sys/kernel/random/boot_id || return 1
    capture_required whole_dev_t cat -- "/sys/block/$BASE/dev" || return 1
    capture_required part_dev_t cat -- "/sys/block/$BASE/${BASE}p1/dev" || return 1
    capture_required diskseq cat -- "/sys/block/$BASE/diskseq" || return 1
    capture_required nsid cat -- "/sys/block/$BASE/nsid" || return 1
    capture_required poll_queues cat -- \
        /sys/module/nvme/parameters/poll_queues || return 1
    capture_required io_poll cat -- "/sys/block/$BASE/queue/io_poll" || return 1
    [[ $poll_queues =~ ^[1-9][0-9]*$ && $io_poll = 1 ]] || return 1
    capture_required mount_source_object stat -Lc '%t:%T' -- "$SOURCE" || return 1
    capture_required controller_char_dev_t stat -Lc '%t:%T' -- "$CTRL_DEV" || return 1
    capture_required namespace_char_dev_t stat -Lc '%t:%T' -- "$CHAR_FD_PATH" || return 1
    capture_required namespace_char_retained_identity stat -Lc \
        '%d:%i:%t:%T:%u:%h:%a' -- "$CHAR_FD_PATH" || return 1
    capture_required namespace_char_path_identity stat -Lc \
        '%d:%i:%t:%T:%u:%h:%a' -- "$CHAR" || return 1
    [ "$namespace_char_retained_identity" = "$CHAR_IDENTITY" ] &&
        [ "$namespace_char_path_identity" = "$CHAR_IDENTITY" ] || return 1
    printf 'boot_id=%s\n' "$boot_id"
    printf 'controller=%s\nbase=%s\n' "$CTRL" "$BASE"
    printf 'controller_path=%s\ndriver_path=%s\n' "$ctrl_path" "$driver_path"
    printf 'whole_dev_t=%s\npart_dev_t=%s\n' \
        "$whole_dev_t" "$part_dev_t"
    printf 'diskseq=%s\nnsid=%s\nmount_id=%s\n' \
        "$diskseq" "$nsid" "$mount_id"
    printf 'poll_queues=%s\nio_poll=%s\n' "$poll_queues" "$io_poll"
    printf 'mount_source=%s\nmount_source_object=%s\n' "$SOURCE" \
        "$mount_source_object"
    printf 'controller_char=%s\ncontroller_char_dev_t=%s\n' "$CTRL_DEV" \
        "$controller_char_dev_t"
    printf 'namespace_char=%s\nnamespace_char_dev_t=%s\n' "$CHAR" \
        "$namespace_char_dev_t"
    printf 'namespace_char_nsid=%s\nnamespace_char_retained_identity=%s\n' \
        "$CHAR_NSID" "$namespace_char_retained_identity"
    printf 'cpu_pool=%s\ncpu_map=%s\n' "$CPU_POOL" "$map"
    IFS=',' read -r -a cpus <<< "$map"
    for cpu in "${cpus[@]}"; do
        online_path="/sys/devices/system/cpu/cpu$cpu/online"
        if [ -e "$online_path" ]; then
            capture_required online cat -- "$online_path" || return 1
        else
            online=1
        fi
        capture_required pkg cat -- "/sys/devices/system/cpu/cpu$cpu/topology/physical_package_id" || return 1
        capture_required core cat -- "/sys/devices/system/cpu/cpu$cpu/topology/core_id" || return 1
        capture_required siblings cat -- "/sys/devices/system/cpu/cpu$cpu/topology/thread_siblings_list" || return 1
        [[ $online =~ ^[01]$ && $pkg =~ ^[0-9]+$ && $core =~ ^[0-9]+$ &&
           $siblings =~ ^[0-9,-]+$ ]] || return 1
        printf 'cpu_topology_%s=online:%s,node1:1,package:%s,core:%s,siblings:%s\n' \
            "$cpu" "$online" "$pkg" "$core" "$siblings"
    done

    shopt -s nullglob
    queue_paths=("/sys/block/$BASE/mq"/[0-9]*)
    shopt -u nullglob
    [ "${#queue_paths[@]}" -gt 0 ] || return 1
    while IFS= read -r queue_path; do
        queue=${queue_path##*/}
        cpu_list=$(<"$queue_path/cpu_list") || return 1
        [[ $queue =~ ^[0-9]+$ && $cpu_list =~ ^[0-9,-]+$ ]] || return 1
        printf 'mq_generation_%s=cpu_list:%s\n' "$queue" "$cpu_list"
    done < <(printf '%s\n' "${queue_paths[@]}" | sort -V)
    if [ -n "${VARIANT_MANIFEST_SHA:-}" ]; then
        printf 'variants_manifest_sha256=%s\n' "$VARIANT_MANIFEST_SHA"
        printf 'variants_manifest_identity=%s\n' \
            "$VARIANT_MANIFEST_IDENTITY"
        for label in gg gs sg ss; do
            printf 'variant_%s_sha256=%s\n' "$label" \
                "${VARIANT_SHA[$label]}"
            printf 'variant_%s_identity=%s\n' "$label" \
                "${VARIANT_IDENTITY[$label]}"
        done
    fi
}

freeze_campaign_generation()
{
    local snapshot
    resolve_identity
    snapshot=$(campaign_generation_snapshot) || die generation_snapshot_failed
    freeze_generation_value "$snapshot"
    printf '%s\nsha256=%s\n' "$snapshot" "$CAMPAIGN_GENERATION_SHA" \
        >"$RESULT_DIR/campaign-generation.txt"
    echo "CAMPAIGN_GENERATION_FROZEN sha256=$CAMPAIGN_GENERATION_SHA"
}

assert_campaign_generation()
{
    local current
    current=$(campaign_generation_snapshot) || die generation_snapshot_failed
    assert_generation_value "$current"
}

run_test_generation_freeze()
{
    local file=$1 current
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ -f "$file" ] && [ ! -L "$file" ] || die unsafe_test_generation_file
    freeze_generation_value "$(<"$file")"
    echo CAMPAIGN_GENERATION_FROZEN
    if [ "${EXITOS_SCALE_TEST_PAUSE_AFTER_GENERATION:-0}" = 1 ]; then
        kill -STOP "$$"
    fi
    current=$(<"$file")
    assert_generation_value "$current"
    echo CAMPAIGN_GENERATION_OK
}

run_test_post_cell_generation()
{
    local generation_file=$1 summary_marker=$2 current
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ -f "$generation_file" ] && [ ! -L "$generation_file" ] ||
        die unsafe_test_generation_file
    freeze_generation_value "$(<"$generation_file")"
    echo POST_CELL_IO_COMPLETE
    if [ "${EXITOS_SCALE_TEST_PAUSE_POST_CELL:-0}" = 1 ]; then
        kill -STOP "$$"
    fi
    current=$(<"$generation_file")
    assert_generation_value "$current"
    printf '%s\n' summarized >"$summary_marker"
}

run_test_required_generation_read()
{
    local value
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_required_generation_read_usage
    capture_required value cat -- "$1" || die required_generation_read_failed
    printf 'REQUIRED_GENERATION_READ_OK value=%s\n' "$value"
}

run_test_workset_seal()
{
    local output
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 3 ] || die test_workset_seal_usage
    retain_owned_workdir "$1"
    output=$(seal_workset "$2" "$3") || die test_workset_seal_failed
    printf '%s\n' "$output"
}

run_test_workset_verify()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 4 ] || die test_workset_verify_usage
    retain_owned_workdir "$1"
    verify_workset test "$2" "$3" "$4"
}

run_test_workfile_init()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_workfile_init_usage
    retain_owned_workdir "$1"
    if [ "${EXITOS_SCALE_TEST_PAUSE_AFTER_RETAIN:-0}" = 1 ]; then
        echo WORKDIR_RETAINED_READY
        kill -STOP "$$"
    fi
    secure_init_workfiles "$WORK_FD_PATH"
}

prepare_result_root_and_lock()
{
    local parent meta lock_path lock_id fd_id lock_root lock_root_real
    local lock_root_uid lock_root_mode

    validate_result_root_policy "$RESULT_ROOT"
    [ ! -L "$RESULT_ROOT" ] || die "result_root_is_symlink=$RESULT_ROOT"
    if [ ! -e "$RESULT_ROOT" ]; then
        parent=${RESULT_ROOT%/*}
        [ -n "$parent" ] || parent=/
        [ -d "$parent" ] && [ ! -L "$parent" ] ||
            die "result_parent_unsafe=$parent"
        mkdir -m 0700 -- "$RESULT_ROOT" || die "result_root_create_failed=$RESULT_ROOT"
    fi
    [ -d "$RESULT_ROOT" ] && [ ! -L "$RESULT_ROOT" ] ||
        die "result_root_not_directory=$RESULT_ROOT"
    meta=$(stat -Lc '%u:%a' -- "$RESULT_ROOT") || die result_root_stat_failed
    [ "$meta" = "$(id -u):700" ] || die "result_root_not_owned_0700=$meta"

    lock_root=$LOCK_ROOT
    if [ -n "${EXITOS_SCALE_TEST_LOCK_ROOT:-}" ]; then
        [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_lock_root_override_forbidden
        lock_root=$EXITOS_SCALE_TEST_LOCK_ROOT
    fi
    [ -n "$lock_root" ] && [[ $lock_root == /* ]] && [ ! -L "$lock_root" ] &&
        [ -d "$lock_root" ] || die "unsafe_lock_root=$lock_root"
    lock_root_real=$(readlink -f -- "$lock_root") || die lock_root_unresolvable
    [ "$lock_root_real" = "$lock_root" ] || die lock_root_not_canonical
    lock_root_uid=$(stat -Lc %u -- "$lock_root") || die lock_root_owner_unreadable
    lock_root_mode=$(stat -Lc %a -- "$lock_root") || die lock_root_mode_unreadable
    [ "$lock_root_uid" = "$(id -u)" ] || die "lock_root_not_owned=$lock_root_uid"
    (( (8#$lock_root_mode & 8#022) == 0 )) ||
        die "lock_root_group_or_world_writable=$lock_root_mode"
    [[ $SERIAL =~ ^[A-Za-z0-9._+-]+$ ]] || die unsafe_lock_serial
    lock_path=$lock_root/exitos-fio-scale-pilot-$SERIAL.lock
    python3 - "$lock_path" <<'PY'
import errno, os, stat, sys
p=sys.argv[1]
try:
    fd=os.open(p, os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC, 0o600)
except FileExistsError:
    st=os.lstat(p)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_nlink != 1 or stat.S_IMODE(st.st_mode) != 0o600:
        raise SystemExit("unsafe existing coordination lock")
else:
    os.close(fd)
PY
    [ ! -L "$lock_path" ] || die coordination_lock_is_symlink
    exec {CAMPAIGN_LOCK_FD}<>"$lock_path" || die coordination_lock_open_failed
    lock_id=$(stat -Lc '%d:%i:%u:%a:%h' -- "$lock_path") || die coordination_lock_stat_failed
    fd_id=$(stat -Lc '%d:%i:%u:%a:%h' -- "/proc/$$/fd/$CAMPAIGN_LOCK_FD") ||
        die coordination_lock_fd_stat_failed
    [ "$lock_id" = "$fd_id" ] || die coordination_lock_changed
    [ "$lock_id" = "$(stat -Lc '%d:%i' -- "$lock_path"):$(id -u):600:1" ] ||
        die "coordination_lock_unsafe=$lock_id"
    flock -n "$CAMPAIGN_LOCK_FD" || die another_scale_campaign_running
}

run_test_campaign_lock()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_campaign_lock_usage
    RESULT_ROOT=$1
    umask 077
    prepare_result_root_and_lock
    echo CAMPAIGN_LOCK_HELD
    if [ "${EXITOS_SCALE_TEST_PAUSE_AFTER_LOCK:-0}" = 1 ]; then
        kill -STOP "$$"
    fi
}

create_owned_workdir()
{
    identity_gate
    WORK_DIR=$(mktemp -d "$MNT/.exitos-scale-work.XXXXXXXX") ||
        die workdir_create_failed
    chmod 0700 -- "$WORK_DIR" || die workdir_chmod_failed
    retain_owned_workdir "$WORK_DIR"
    secure_init_workfiles "$WORK_FD_PATH"
    echo "WORKDIR_CREATED path=$WORK_DIR retained=$WORK_FD_PATH"
}

# Kernel 7 does not expose an nsid attribute below every nvme-generic ng*
# object.  Use sysfs only to prove namespace ownership, then ask the candidate
# character inode itself for its namespace ID through a retained descriptor.
# Never infer the namespace from the ng* basename.
query_retained_char_nsid()
{
    local destination=$1 nvme_bin=$2 fd_path=$3 raw parsed_nsid echoed_fd
    raw=$("$nvme_bin" get-ns-id "$fd_path" 2>/dev/null) || return 1
    [[ $raw != *$'\n'* && $raw != *$'\r'* &&
       $raw =~ ^(.+):[[:space:]]namespace-id:([1-9][0-9]*)$ ]] || return 1
    echoed_fd=${BASH_REMATCH[1]}
    parsed_nsid=${BASH_REMATCH[2]}
    [ "$echoed_fd" = "${fd_path##*/}" ] || return 1
    (( ${#parsed_nsid} <= 10 && 10#$parsed_nsid <= 4294967295 )) || return 1
    printf -v "$destination" '%u' "$((10#$parsed_nsid))"
}

retain_generic_char_for_namespace()
{
    local block_owner=$1 expected_nsid=$2
    local generic_root=${3:-/sys/class/nvme-generic}
    local dev_root=${4:-/dev} nvme_bin=${5:-${NVME_BIN:-}}
    local g char_owner char_name path candidate_fd candidate_fd_path
    local path_identity fd_identity owner_links normalized
    local sys_devt rdev_hex major_hex minor_hex fd_devt
    local selected_path selected_fd selected_identity selected_nsid
    local retained_identity retained_nsid fd_to_close closed_fd closed_count=0
    local candidates=() matches=() match_fds=() match_identities=() match_nsids=()

    [[ $expected_nsid =~ ^[1-9][0-9]*$ ]] || die invalid_block_nsid
    block_owner=$(readlink -f -- "$block_owner") || die block_owner_unresolvable
    generic_root=$(cd "$generic_root" && pwd -P) || die generic_sysfs_unresolvable
    dev_root=$(cd "$dev_root" && pwd -P) || die generic_dev_root_unresolvable
    nvme_bin=$(readlink -f -- "$nvme_bin") || die nvme_cli_unresolvable
    [ -x "$nvme_bin" ] || die nvme_cli_not_executable

    shopt -s nullglob
    candidates=("$generic_root"/ng*)
    shopt -u nullglob
    for g in "${candidates[@]}"; do
        [ -e "$g/device" ] || continue
        char_owner=$(readlink -f -- "$g/device") || continue
        [ "$char_owner" = "$block_owner" ] || continue
        sys_devt=$(cat -- "$g/dev" 2>/dev/null) || continue
        [[ $sys_devt =~ ^[0-9]+:[0-9]+$ ]] || continue
        char_name=${g##*/}
        path=$dev_root/$char_name
        [ -c "$path" ] && [ ! -L "$path" ] || continue
        if ! exec {candidate_fd}<"$path"; then
            continue
        fi
        candidate_fd_path=/proc/$$/fd/$candidate_fd
        path_identity=$(stat -Lc '%d:%i:%t:%T:%u:%h:%a' -- "$path") || {
            exec {candidate_fd}<&-
            continue
        }
        fd_identity=$(stat -Lc '%d:%i:%t:%T:%u:%h:%a' -- "$candidate_fd_path") || {
            exec {candidate_fd}<&-
            continue
        }
        owner_links=$(stat -Lc '%u:%h' -- "$candidate_fd_path") || {
            exec {candidate_fd}<&-
            continue
        }
        rdev_hex=$(stat -Lc '%t:%T' -- "$candidate_fd_path") || {
            exec {candidate_fd}<&-
            continue
        }
        IFS=: read -r major_hex minor_hex <<<"$rdev_hex"
        if ! [[ $major_hex =~ ^[0-9a-fA-F]+$ &&
               $minor_hex =~ ^[0-9a-fA-F]+$ ]]; then
            exec {candidate_fd}<&-
            continue
        fi
        fd_devt="$((16#$major_hex)):$((16#$minor_hex))"
        if [ "$path_identity" != "$fd_identity" ] ||
           [ "$owner_links" != "$(id -u):1" ] ||
           [ "$fd_devt" != "$sys_devt" ] ||
           [ ! -c "$candidate_fd_path" ]; then
            exec {candidate_fd}<&-
            continue
        fi
        if ! query_retained_char_nsid normalized "$nvme_bin" \
                "$candidate_fd_path"; then
            exec {candidate_fd}<&-
            continue
        fi
        if [ "$normalized" -ne "$((10#$expected_nsid))" ]; then
            exec {candidate_fd}<&-
            continue
        fi
        matches+=("$path")
        match_fds+=("$candidate_fd")
        match_identities+=("$fd_identity")
        match_nsids+=("$normalized")
    done

    if [ "${#matches[@]}" -ne 1 ]; then
        for fd_to_close in "${match_fds[@]}"; do
            closed_fd=$fd_to_close
            exec {fd_to_close}<&-
            if [ "${EXITOS_SCALE_TEST_REPORT_GENERIC_CLOSE:-0}" = 1 ]; then
                [ ! -e "/proc/$$/fd/$closed_fd" ] ||
                    die generic_char_candidate_fd_leaked
                closed_count=$((closed_count + 1))
            fi
        done
        if [ "${EXITOS_SCALE_TEST_REPORT_GENERIC_CLOSE:-0}" = 1 ]; then
            [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
            printf 'GENERIC_CHAR_REJECT_CLOSED count=%s\n' "$closed_count"
        fi
        die "generic_char_matches=${#matches[@]}"
    fi
    selected_path=${matches[0]}
    selected_fd=${match_fds[0]}
    selected_identity=${match_identities[0]}
    selected_nsid=${match_nsids[0]}

    if [ -n "${CHAR_FD:-}" ]; then
        retained_identity=$(stat -Lc '%d:%i:%t:%T:%u:%h:%a' -- \
            "$CHAR_FD_PATH") || die retained_generic_char_stat_failed
        [ "$selected_path" = "$CHAR" ] &&
            [ "$selected_identity" = "$CHAR_IDENTITY" ] &&
            [ "$retained_identity" = "$CHAR_IDENTITY" ] &&
            [ "$(stat -Lc '%d:%i:%t:%T:%u:%h:%a' -- "$CHAR")" = \
              "$CHAR_IDENTITY" ] || die generic_char_identity_drift
        query_retained_char_nsid retained_nsid "$nvme_bin" "$CHAR_FD_PATH" ||
            die retained_generic_char_nsid_failed
        [ "$retained_nsid" -eq "$selected_nsid" ] ||
            die retained_generic_char_nsid_drift
        exec {selected_fd}<&-
    else
        CHAR=$selected_path
        CHAR_FD=$selected_fd
        CHAR_FD_PATH=/proc/$$/fd/$CHAR_FD
        CHAR_IDENTITY=$selected_identity
        CHAR_DEV_T=$(stat -Lc '%t:%T' -- "$CHAR_FD_PATH") ||
            die retained_generic_char_devt_failed
        CHAR_NSID=$selected_nsid
    fi
    export CHAR CHAR_FD_PATH CHAR_IDENTITY CHAR_DEV_T CHAR_NSID
}

resolve_identity()
{
    local matches=() s got ctrl ns=() b base dev part source majmin fstype uuid
    local block_owner block_nsid

    for s in /sys/class/nvme/nvme*/serial; do
        [ -r "$s" ] || continue
        got=$(tr -d '[:space:]' < "$s")
        [ "$got" = "$SERIAL" ] && matches+=("$(basename "$(dirname "$s")")")
    done
    [ "${#matches[@]}" -eq 1 ] || die "serial_matches=${#matches[@]}"
    ctrl=${matches[0]}
    [ "$(tr -d '[:space:]' < "/sys/class/nvme/$ctrl/serial")" = "$SERIAL" ] || die serial_drift
    [ "$(basename "$(readlink -f "/sys/class/nvme/$ctrl/device")")" = "$PCI" ] || die pci_mismatch
    [ "$(basename "$(readlink -f "/sys/class/nvme/$ctrl/device/driver")")" = nvme ] || die driver_mismatch
    CTRL_DEV=/dev/$ctrl
    [ -c "$CTRL_DEV" ] || die "controller_char_missing=$CTRL_DEV"

    for b in /sys/block/${ctrl}n*; do
        [ -r "$b/nsid" ] || continue
        [ "$(tr -d '[:space:]' < "$b/nsid")" = 1 ] && ns+=("$(basename "$b")")
    done
    [ "${#ns[@]}" -eq 1 ] || die "nsid1_matches=${#ns[@]}"
    base=${ns[0]}
    dev=/dev/$base
    part=/dev/${base}p1
    [ -b "$dev" ] || die "whole_missing=$dev"
    [ -b "$part" ] || die "partition_missing=$part"
    [ "$(tr -d '[:space:]' < "/sys/block/$base/wwid")" = "$WWID" ] || die wwid_mismatch
    [ "$(cat "/sys/block/$base/${base}p1/start")" = "$start" ] || die partition_start_mismatch
    [ "$(cat "/sys/block/$base/${base}p1/size")" = "$count" ] || die partition_count_mismatch
    [ -z "$(find "/sys/block/$base/holders" -mindepth 1 -maxdepth 1 -print -quit)" ] || die whole_has_holder

    block_owner=$(readlink -f -- "/sys/class/block/$base/device") ||
        die namespace_owner_unresolvable
    block_nsid=$(cat -- "/sys/block/$base/nsid") || die block_nsid_read_failed
    retain_generic_char_for_namespace "$block_owner" "$block_nsid" \
        /sys/class/nvme-generic /dev "$NVME_BIN"

    source=$(findmnt -rn -T "$MNT" -o SOURCE)
    majmin=$(findmnt -rn -T "$MNT" -o MAJ:MIN)
    fstype=$(findmnt -rn -T "$MNT" -o FSTYPE)
    uuid=$(findmnt -rn -T "$MNT" -o UUID)
    [ "$majmin" = "$(cat "/sys/block/$base/${base}p1/dev")" ] || die mount_devt_mismatch
    [ "$fstype" = ext4 ] || die mount_not_ext4
    [ "$uuid" = "$UUID" ] || die filesystem_generation_mismatch
    [ "$(stat -Lc '%t:%T' "$source")" = "$(stat -Lc '%t:%T' "$part")" ] || die mount_source_object_mismatch
    [ "$(findmnt -rn -T "$MNT" -o TARGET)" = "$MNT" ] || die mount_target_mismatch

    CTRL=$ctrl BASE=$base DEV=$dev PART=$part SOURCE=$source
    export CTRL BASE DEV PART SOURCE CTRL_DEV CHAR CHAR_FD_PATH CHAR_IDENTITY \
        CHAR_DEV_T CHAR_NSID
}

# Keep raw, back-to-back utilization evidence for only the physical cores used
# by the measurement.  Other CPUs are deliberately outside this gate.  Before
# timing and during each cell, utilization is recorded for the selected CPUs
# and their SMT siblings.  The configured percentage is a reference marker,
# never a rejection threshold; only observation-integrity failures reject.
continuous_noise_monitor()
{
    local mode=$1 proc_root=$2 cpu_root=$3 map=$4 duration_ms=$5
    local poll_ms=$6 util_window_ms=$7 util_max=$8 stop_token=${9:--}
    local expected_parent=$$

    if [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] &&
       [ -n "${EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE:-}" ]; then
        expected_parent=$EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE
    fi

    exec python3 - "$mode" "$proc_root" "$cpu_root" "$map" "$duration_ms" \
        "$poll_ms" "$util_window_ms" "$util_max" \
        "$MAX_NOISE_BLIND_MS" "$expected_parent" "$stop_token" <<'PY'
import ctypes
import math
import os
import re
import signal
import stat
import sys
import time

(mode, proc_root, cpu_root, selected_text, duration_text, poll_text,
 util_window_text, util_max_text, max_blind_text,
 expected_parent_text, stop_token) = sys.argv[1:]

def refuse(message):
    print("NOISE_EVENT kind=monitor_error detail=" +
          re.sub(r"[^A-Za-z0-9_.:+/=-]", "_", message), flush=True)
    raise SystemExit(5)

if mode not in ("preflight", "cell"):
    refuse("bad_mode")
try:
    duration_ms = int(duration_text)
    poll_ms = int(poll_text)
    util_window_ms = int(util_window_text)
    util_max = float(util_max_text)
    max_blind_ms = int(max_blind_text)
    expected_parent = int(expected_parent_text)
except ValueError:
    refuse("bad_numeric_configuration")
if not (10 <= poll_ms <= max_blind_ms <= 1000):
    refuse("poll_ms_out_of_range")
if not (poll_ms <= util_window_ms <= 60000):
    refuse("util_window_out_of_range")
if not (0.0 < util_max <= 100.0):
    refuse("util_max_out_of_range")
if mode == "preflight" and duration_ms <= 0:
    refuse("preflight_duration_out_of_range")
if mode == "cell" and duration_ms != 0:
    refuse("cell_duration_must_be_zero")
if expected_parent <= 1:
    refuse("expected_parent_invalid")
test_mode = os.environ.get("EXITOS_SCALE_TEST_MODE") == "1"
if mode == "preflight" and stop_token != "-":
    refuse("preflight_stop_token_forbidden")
if mode == "cell" and stop_token == "-" and not test_mode:
    refuse("cell_stop_token_missing")
if not os.path.isdir(proc_root) or not os.path.isdir(cpu_root):
    refuse("monitor_root_missing")

def parse_cpu_list(value):
    cpus = set()
    if not re.fullmatch(r"[0-9,-]+", value):
        raise ValueError("bad_cpu_list")
    for item in value.split(","):
        if "-" in item:
            parts = item.split("-")
            if len(parts) != 2:
                raise ValueError("bad_cpu_range")
            first, last = (int(part) for part in parts)
            if first > last:
                raise ValueError("reversed_cpu_range")
            cpus.update(range(first, last + 1))
        elif item:
            cpus.add(int(item))
        else:
            raise ValueError("empty_cpu_item")
    return cpus

try:
    selected = parse_cpu_list(selected_text)
except ValueError as error:
    refuse(str(error))
if not selected:
    refuse("empty_selected_cpu_map")

def read_text(path):
    with open(path, "r", encoding="ascii") as source:
        return source.read().strip()

watched = set()
roles = {}
try:
    for cpu in sorted(selected):
        topology = os.path.join(cpu_root, "cpu%d" % cpu, "topology")
        siblings = parse_cpu_list(read_text(os.path.join(
            topology, "thread_siblings_list")))
        if cpu not in siblings:
            raise ValueError("selected_cpu_absent_from_siblings_%d" % cpu)
        if len(siblings) < 2:
            raise ValueError("selected_cpu_has_no_smt_sibling_%d" % cpu)
        other_selected = (siblings - {cpu}) & selected
        if other_selected:
            raise ValueError("selected_smt_sibling_%d_%s" %
                             (cpu, ",".join(str(value) for value in sorted(other_selected))))
        for sibling in siblings:
            online_path = os.path.join(cpu_root, "cpu%d" % sibling, "online")
            if os.path.exists(online_path) and read_text(online_path) != "1":
                raise ValueError("watched_cpu_offline_%d" % sibling)
        watched.update(siblings)
    roles = {cpu: ("selected" if cpu in selected else "sibling")
             for cpu in watched}
except (OSError, ValueError) as error:
    refuse("topology_" + str(error))

# Keep this low-rate observer off every selected physical core.  Fixture roots
# deliberately skip affinity changes; production always uses these exact
# proc/sys roots.
monitor_cpu = -1
if os.path.realpath(proc_root) == "/proc" and os.path.realpath(cpu_root) == "/sys/devices/system/cpu":
    try:
        candidates = sorted(os.sched_getaffinity(0) - watched)
        if not candidates:
            raise OSError("no_unwatched_allowed_cpu")
        monitor_cpu = candidates[0]
        os.sched_setaffinity(0, {monitor_cpu})
    except OSError as error:
        refuse("monitor_affinity_" + str(error))

stopping = False
stop_authorized = False
unauthorized_stop_signal = None

def valid_stop_token():
    if stop_token == "-":
        return test_mode
    fd = os.open(stop_token, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(fd)
        if (not stat.S_ISREG(metadata.st_mode) or
                metadata.st_uid != os.geteuid() or metadata.st_nlink != 1 or
                stat.S_IMODE(metadata.st_mode) != 0o600):
            return False
        payload = os.read(fd, 256)
        if os.read(fd, 1):
            return False
    finally:
        os.close(fd)
    expected = "CELL_STOP parent=%d monitor=%d\n" % (expected_parent, os.getpid())
    return payload == expected.encode("ascii")

def request_stop(signum, _frame):
    global stopping, stop_authorized, unauthorized_stop_signal
    if mode == "preflight":
        print("NOISE_EVENT kind=unauthorized_stop mode=preflight signal=%d" %
              signum, flush=True)
        raise SystemExit(4)
    try:
        authorized = valid_stop_token()
    except OSError:
        authorized = False
    if authorized:
        stop_authorized = True
        stopping = True
    else:
        unauthorized_stop_signal = signum

signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)
signal.signal(signal.SIGHUP, request_stop)

# The shell passes the runner PID rather than trusting the child's first
# getppid(): the runner may already have died before Python starts.  Check the
# expected parent both before and after prctl to close every arm-time race.
if (os.environ.get("EXITOS_SCALE_TEST_MODE") == "1" and
        os.environ.get("EXITOS_SCALE_TEST_PAUSE_BEFORE_MONITOR_PDEATHSIG") == "1"):
    print("MONITOR_PRE_PDEATHSIG_READY pid=%d expected_parent=%d" %
          (os.getpid(), expected_parent), flush=True)
    os.kill(os.getpid(), signal.SIGSTOP)
monitor_parent_before_prctl = os.getppid()
if monitor_parent_before_prctl != expected_parent:
    raise SystemExit(143)
try:
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl")
except (AttributeError, OSError) as error:
    refuse("pdeathsig_" + str(error))
monitor_parent_after_prctl = os.getppid()
if monitor_parent_after_prctl != expected_parent:
    raise SystemExit(143)

def read_stat():
    cpus = {}
    with open(os.path.join(proc_root, "stat"), "r", encoding="ascii") as source:
        for line in source:
            fields = line.split()
            if not fields:
                continue
            if fields[0].startswith("cpu") and fields[0][3:].isdigit():
                cpu = int(fields[0][3:])
                if cpu not in watched:
                    continue
                values = [int(value) for value in fields[1:]]
                if len(values) < 4:
                    raise ValueError("short_cpu_stat_%d" % cpu)
                # guest fields are already included in user/nice; the first
                # eight fields form a non-double-counted total.  I/O wait is
                # conservatively not counted as idle in the recorded sample.
                cpus[cpu] = (sum(values[:8]), values[3])
    return cpus

def emit_event(event):
    print("NOISE_EVENT " + event, flush=True)

try:
    initial_event, baseline = None, read_stat()
except (OSError, ValueError) as error:
    refuse("initial_sample_" + str(error))
if set(baseline) != watched:
    refuse("initial_cpu_snapshot_missing")

started = time.monotonic()
poll_seconds = poll_ms / 1000.0
util_seconds = util_window_ms / 1000.0
if mode == "preflight":
    finish_at = started + duration_ms / 1000.0
else:
    finish_at = float("inf")
next_util = min(started + util_seconds, finish_at)
baseline_at = started
samples = 1
latched_event = None
previous_sample_started = started
next_poll = started + poll_seconds
reference_exceedances = 0
max_util = {cpu: 0.0 for cpu in watched}
print("NOISE_MONITOR_READY mode=%s pid=%d poll_ms=%d monitor_cpu=%d watched=%s duration_ms=%d" %
      (mode, os.getpid(), poll_ms, monitor_cpu,
       ",".join(str(cpu) for cpu in sorted(watched)), duration_ms), flush=True)

test_delay_ms = 0
test_pause_after_signal_scan = False
test_signal_scan_paused = False
if os.environ.get("EXITOS_SCALE_TEST_MODE") == "1":
    try:
        test_delay_ms = int(os.environ.get("EXITOS_SCALE_TEST_SAMPLE_DELAY_MS", "0"))
    except ValueError:
        refuse("bad_test_sample_delay")
    if not (0 <= test_delay_ms <= 5000):
        refuse("test_sample_delay_out_of_range")
    pause_text = os.environ.get("EXITOS_SCALE_TEST_PAUSE_AFTER_SIGNAL_SCAN", "0")
    if pause_text not in ("0", "1"):
        refuse("bad_test_signal_scan_pause")
    test_pause_after_signal_scan = pause_text == "1"
if test_delay_ms:
    time.sleep(test_delay_ms / 1000.0)

def observe_window(current, now, scope):
    global baseline, baseline_at, reference_exceedances
    if set(current) != watched:
        return "kind=cpu_snapshot_missing"
    elapsed_window_ms = int(round((now - baseline_at) * 1000.0))
    for cpu in sorted(watched):
        total = current[cpu][0] - baseline[cpu][0]
        idle = current[cpu][1] - baseline[cpu][1]
        if total < 0 or idle < 0 or idle > total:
            return "kind=cpu_counter_invalid cpu=%d total=%d idle=%d" % (
                cpu, total, idle)
        role = roles[cpu]
        if total == 0:
            # A signal can end a cell less than one scheduler tick after the
            # previous full sample.  Preserve that tail explicitly without
            # inventing a utilization percentage.
            print("CPU_UTIL_WINDOW mode=%s scope=%s window_ms=%d cpu=%d role=%s "
                  "total_ticks=0 idle_ticks=0 util_pct=unavailable "
                  "reference=%.3f above_reference=unknown" %
                  (mode, scope, elapsed_window_ms, cpu, role, util_max),
                  flush=True)
            continue
        util_pct = 100.0 * (total - idle) / total
        max_util[cpu] = max(max_util[cpu], util_pct)
        above = util_pct + 1e-9 >= util_max
        print("CPU_UTIL_WINDOW mode=%s scope=%s window_ms=%d cpu=%d role=%s "
              "total_ticks=%d idle_ticks=%d util_pct=%.3f reference=%.3f "
              "above_reference=%d" %
              (mode, scope, elapsed_window_ms, cpu, role, total, idle,
               util_pct, util_max, 1 if above else 0), flush=True)
        if above:
            reference_exceedances += 1
            print("CPU_UTIL_REFERENCE_EXCEEDED mode=%s scope=%s window_ms=%d "
                  "cpu=%d role=%s util_pct=%.3f reference=%.3f" %
                  (mode, scope, elapsed_window_ms, cpu, role, util_pct,
                   util_max), flush=True)
    baseline = current
    baseline_at = now
    return None

while True:
    sample_started = time.monotonic()
    gap_ms = (sample_started - previous_sample_started) * 1000.0
    previous_sample_started = sample_started
    samples += 1
    if gap_ms > max_blind_ms:
        event = "kind=monitor_gap gap_ms=%.3f maximum_ms=%d" % (
            gap_ms, max_blind_ms)
        emit_event(event)
        if mode == "preflight":
            raise SystemExit(4)
        latched_event = event
    current = None
    if latched_event is None:
        try:
            current = read_stat()
            event = None
        except (OSError, ValueError) as error:
            detail = re.sub(r"[^A-Za-z0-9_.:+/=-]", "_", "sample_" + str(error))
            event = "kind=monitor_error detail=" + detail
            emit_event(event)
            if mode == "preflight":
                raise SystemExit(5)
            latched_event = event
    now = time.monotonic()
    if unauthorized_stop_signal is not None:
        event = "kind=unauthorized_stop mode=%s signal=%d" % (
            mode, unauthorized_stop_signal)
        emit_event(event)
        latched_event = event
        unauthorized_stop_signal = None
    if test_pause_after_signal_scan and not test_signal_scan_paused:
        print("NOISE_SIGNAL_SCAN_PASSED mode=%s pid=%d" %
              (mode, os.getpid()), flush=True)
        test_signal_scan_paused = True
        os.kill(os.getpid(), signal.SIGSTOP)
    if latched_event is not None:
        # A cell sentinel remains the runner's live direct child until the
        # authorized end-of-cell handshake.  Exiting on first contamination
        # would leave a reusable saved PID while fio is still running.  Once
        # latched, stop sampling and sleep only against a freshly advanced poll
        # deadline; next_util may already be stale and must not cause a spin.
        if mode == "preflight" or stop_authorized:
            break
        while next_poll <= now:
            next_poll += poll_seconds
        time.sleep(max(0.0, next_poll - time.monotonic()))
        continue
    if now >= next_util:
        scope = "tail" if mode == "preflight" and next_util >= finish_at else "full"
        event = observe_window(current, now, scope)
        if event is not None:
            emit_event(event)
            if mode == "preflight":
                raise SystemExit(4)
            latched_event = event
            continue
        if mode == "preflight" and next_util >= finish_at:
            break
        next_util += util_seconds
        while next_util <= now:
            next_util += util_seconds
        if mode == "preflight":
            next_util = min(next_util, finish_at)
    if stopping:
        if now > baseline_at:
            event = observe_window(current, now, "tail")
            if event is not None:
                emit_event(event)
                latched_event = event
        break
    while next_poll <= now:
        next_poll += poll_seconds
    wake = min(next_poll, next_util, finish_at)
    time.sleep(max(0.0, wake - time.monotonic()))

elapsed_ms = int(round((time.monotonic() - started) * 1000.0))
if latched_event is not None:
    raise SystemExit(4)
maxima = ",".join("%d:%.3f" % (cpu, max_util[cpu]) for cpu in sorted(watched))
print("CPU_UTIL_SUMMARY mode=%s reference=%.3f exceedances=%d maxima=%s" %
      (mode, util_max, reference_exceedances, maxima), flush=True)
if mode == "cell":
    if not stop_authorized:
        refuse("cell_stop_not_authorized")
    print("NOISE_MONITOR_STOP_AUTHORIZED parent=%d monitor=%d" %
          (expected_parent, os.getpid()), flush=True)
print("NOISE_MONITOR_OK mode=%s samples=%d elapsed_ms=%d requested_ms=%d" %
      (mode, samples, elapsed_ms, duration_ms), flush=True)
PY
}

quiet_duration_ms()
{
    printf '%s' "$(((QUIET_SAMPLES - 1) * QUIET_INTERVAL * 1000))"
}

quiet_window()
{
    local duration_ms map output

    [[ "$QUIET_SAMPLES" =~ ^[0-9]+$ ]] &&
        [ "$QUIET_SAMPLES" -ge 2 ] && [ "$QUIET_SAMPLES" -le 120 ] ||
        die "bad_quiet_samples=$QUIET_SAMPLES"
    [[ "$QUIET_INTERVAL" =~ ^[0-9]+$ ]] &&
        [ "$QUIET_INTERVAL" -ge 1 ] && [ "$QUIET_INTERVAL" -le 300 ] ||
        die "bad_quiet_interval=$QUIET_INTERVAL"
    duration_ms=$(quiet_duration_ms)
    map=$(cpu_map "$MAX_JOBS")
    output=$RESULT_DIR/preflight-noise-monitor.txt
    if ! (continuous_noise_monitor preflight /proc /sys/devices/system/cpu \
            "$map" "$duration_ms" "$NOISE_POLL_MS" \
            "$CPU_UTIL_WINDOW_MS" "$CPU_UTIL_MAX") >"$output" 2>&1; then
        sed -n '1,200p' "$output" >&2
        die "continuous_quiet_window_failed artifact=$output"
    fi
    echo "QUIET_WINDOW_OK duration_ms=$duration_ms poll_ms=$NOISE_POLL_MS cpus=$map artifact=$output"
}

start_cell_noise_sentinel()
{
    local output=$1 proc_root=$2 cpu_root=$3 map=$4 ready=0 monitor_pid

    [ -n "$output" ] && [ ! -e "$output" ] && [ ! -L "$output" ] ||
        die "unsafe_cell_sentinel_output=$output"
    (umask 077; set -o noclobber; : >"$output") ||
        die "cell_sentinel_output_create_failed=$output"
    CELL_SENTINEL_STOP_TOKEN=$output.stop
    [ ! -e "$CELL_SENTINEL_STOP_TOKEN" ] &&
        [ ! -L "$CELL_SENTINEL_STOP_TOKEN" ] ||
        die "unsafe_cell_sentinel_stop_token=$CELL_SENTINEL_STOP_TOKEN"
    continuous_noise_monitor cell "$proc_root" "$cpu_root" "$map" 0 \
        "$NOISE_POLL_MS" "$CPU_UTIL_WINDOW_MS" "$CPU_UTIL_MAX" \
        "$CELL_SENTINEL_STOP_TOKEN" \
        >>"$output" 2>&1 &
    CELL_SENTINEL_WAIT_PID=$!
    CELL_SENTINEL_PID=$CELL_SENTINEL_WAIT_PID
    CELL_SENTINEL_LOG=$output
    CELL_SENTINEL_RC=
    for _ in $(seq 1 200); do
        if grep -q '^NOISE_MONITOR_READY mode=cell ' "$output" 2>/dev/null; then
            ready=1
            break
        fi
        kill -0 "$CELL_SENTINEL_WAIT_PID" 2>/dev/null || break
        sleep 0.01
    done
    if [ "$ready" -ne 1 ]; then
        if wait "$CELL_SENTINEL_WAIT_PID"; then
            CELL_SENTINEL_RC=0
        else
            CELL_SENTINEL_RC=$?
        fi
        CELL_SENTINEL_PID=
        CELL_SENTINEL_WAIT_PID=
        sed -n '1,200p' "$output" >&2
        die "cell_sentinel_not_ready artifact=$output rc=$CELL_SENTINEL_RC"
    fi
    monitor_pid=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$output" | head -1)
    [[ $monitor_pid =~ ^[0-9]+$ ]] &&
        [ "$monitor_pid" = "$CELL_SENTINEL_WAIT_PID" ] &&
        kill -0 "$monitor_pid" 2>/dev/null || {
        cleanup_active_cell
        die "cell_sentinel_pid_invalid artifact=$output"
    }
}

finish_cell_noise_sentinel()
{
    local pid=${CELL_SENTINEL_PID:-} wait_pid=${CELL_SENTINEL_WAIT_PID:-}
    local token=${CELL_SENTINEL_STOP_TOKEN:-}

    [ -n "$pid" ] && [ -n "$wait_pid" ] && [ -n "$token" ] ||
        die cell_sentinel_pid_missing
    [ ! -e "$token" ] && [ ! -L "$token" ] ||
        die "unsafe_cell_sentinel_stop_token=$token"
    (umask 077; set -o noclobber
        printf 'CELL_STOP parent=%s monitor=%s\n' "$$" "$pid" >"$token") ||
        die "cell_sentinel_stop_token_create_failed=$token"
    kill -TERM "$pid" 2>/dev/null || true
    if wait "$wait_pid"; then
        CELL_SENTINEL_RC=0
    else
        CELL_SENTINEL_RC=$?
    fi
    CELL_SENTINEL_PID=
    CELL_SENTINEL_WAIT_PID=
    CELL_SENTINEL_STOP_TOKEN=
}

verify_cell_monitor()
{
    local cell=$1
    [ -n "${CELL_SENTINEL_RC:-}" ] || die cell_sentinel_result_missing
    if [ "$CELL_SENTINEL_RC" -ne 0 ]; then
        die "timed_cell_noise_detected cell=$cell sentinel_rc=$CELL_SENTINEL_RC artifact=$CELL_SENTINEL_LOG"
    fi
    grep -q '^NOISE_MONITOR_OK mode=cell ' "$CELL_SENTINEL_LOG" ||
        die "cell_sentinel_completion_missing cell=$cell artifact=$CELL_SENTINEL_LOG"
    grep -q '^NOISE_MONITOR_STOP_AUTHORIZED parent=[0-9][0-9]* monitor=[0-9][0-9]*$' \
        "$CELL_SENTINEL_LOG" ||
        die "cell_sentinel_authorization_missing cell=$cell artifact=$CELL_SENTINEL_LOG"
}

cleanup_active_cell()
{
    local pid live io_pid=${CELL_IO_PID:-} sentinel_pid=${CELL_SENTINEL_PID:-}
    local io_pgid=${CELL_IO_PGID:-} sentinel_wait_pid=${CELL_SENTINEL_WAIT_PID:-}
    local group_owned=0

    if [[ $io_pid =~ ^[0-9]+$ && $io_pgid =~ ^[0-9]+$ ]] &&
       [ "$io_pgid" = "$io_pid" ] && [ "$io_pgid" -gt 1 ] &&
       [ "$io_pgid" != "$$" ]; then
        group_owned=1
        kill -TERM -- "-$io_pgid" 2>/dev/null || true
    fi
    [ -z "$io_pid" ] || kill -TERM "$io_pid" 2>/dev/null || true
    [ -z "$sentinel_pid" ] || kill -TERM "$sentinel_pid" 2>/dev/null || true
    for _ in $(seq 1 100); do
        live=0
        if [ -n "$io_pid" ] && kill -0 "$io_pid" 2>/dev/null; then
            live=1
        fi
        if [ "$group_owned" -eq 1 ]; then
            kill -0 -- "-$io_pgid" 2>/dev/null && live=1
        fi
        if [ -n "$sentinel_pid" ] && kill -0 "$sentinel_pid" 2>/dev/null; then
            live=1
        fi
        [ "$live" -eq 1 ] || break
        sleep 0.01
    done
    if [ "$group_owned" -eq 1 ]; then
        kill -0 -- "-$io_pgid" 2>/dev/null &&
            kill -KILL -- "-$io_pgid" 2>/dev/null || true
    fi
    [ -z "$io_pid" ] || {
        kill -0 "$io_pid" 2>/dev/null && kill -KILL "$io_pid" 2>/dev/null || true
    }
    [ -z "$sentinel_pid" ] || {
        kill -0 "$sentinel_pid" 2>/dev/null &&
            kill -KILL "$sentinel_pid" 2>/dev/null || true
    }
    [ -z "$io_pid" ] || wait "$io_pid" 2>/dev/null || true
    [ -z "$sentinel_wait_pid" ] || wait "$sentinel_wait_pid" 2>/dev/null || true
    CELL_IO_PID=
    CELL_IO_PGID=
    CELL_IO_PGID=
    CELL_SENTINEL_PID=
    CELL_SENTINEL_WAIT_PID=
    CELL_SENTINEL_STOP_TOKEN=
}

signal_cleanup_exit()
{
    local rc=$1
    trap - HUP INT TERM
    if [ "${CELL_EVIDENCE_ACTIVE:-0}" = 1 ]; then
        record_cell_fio_rc "$rc" || true
    fi
    cleanup_active_cell
    if [ "${CELL_EVIDENCE_ACTIVE:-0}" = 1 ]; then
        capture_cell_evidence_post || true
        echo "FIO_SCALE_REFUSE cell_evidence_signal=$rc" >&2
    fi
    exit "$rc"
}

campaign_exit()
{
    local rc=$1
    trap - EXIT
    cleanup_active_cell
    echo "RESULT_DIR=$RESULT_DIR"
    exit "$rc"
}

opener_gate()
{
    local fs_users raw_users char_users token pid foreign_fs="" foreign_raw=""
    local retained_seen=0 char_probe_fd

    fs_users=$(fuser -mM "$MNT" 2>/dev/null || true)
    # Once this campaign has retained its private directory, fuser correctly
    # reports this shell as a mount user.  It is the sole allowed PID; any
    # other opener still refuses the cell.
    for token in $fs_users; do
        pid=${token//[^0-9]/}
        [ -z "$pid" ] || [ "$pid" = "$$" ] || foreign_fs="$foreign_fs $token"
    done
    [ -z "${foreign_fs//[[:space:]]/}" ] ||
        die "foreign_filesystem_users=$foreign_fs"
    raw_users=$(fuser "$DEV" "$PART" "$CTRL_DEV" 2>/dev/null || true)
    [ -z "$(printf '%s' "$raw_users" | tr -d '[:space:]')" ] ||
        die "foreign_raw_openers=$raw_users"
    char_users=$(
        if [ -n "${CHAR_FD:-}" ]; then
            char_probe_fd=$CHAR_FD
            exec {char_probe_fd}<&-
        fi
        fuser "$CHAR" 2>/dev/null || true
    )
    for token in $char_users; do
        pid=${token//[^0-9]/}
        if [ "$pid" = "$$" ]; then
            retained_seen=1
        elif [ -n "$pid" ]; then
            foreign_raw="$foreign_raw $token"
        fi
    done
    [ "$retained_seen" -eq 1 ] || die retained_generic_char_opener_missing
    [ -z "${foreign_raw//[[:space:]]/}" ] ||
        die "foreign_raw_openers=$foreign_raw"
    echo "OPENER_GATE_OK whole=$DEV part=$PART char=$CHAR controller=$CTRL_DEV"
}

run_test_opener_gate()
{
    local path_identity fd_identity
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 5 ] || die test_opener_gate_usage
    MNT=$1 DEV=$2 PART=$3 CHAR=$4 CTRL_DEV=$5
    [ -c "$CHAR" ] && [ ! -L "$CHAR" ] || die unsafe_test_char
    exec {CHAR_FD}<"$CHAR" || die test_char_retain_failed
    CHAR_FD_PATH=/proc/$$/fd/$CHAR_FD
    path_identity=$(stat -Lc '%d:%i:%t:%T' -- "$CHAR") ||
        die test_char_path_stat_failed
    fd_identity=$(stat -Lc '%d:%i:%t:%T' -- "$CHAR_FD_PATH") ||
        die test_char_fd_stat_failed
    [ "$path_identity" = "$fd_identity" ] || die test_char_identity_changed
    FAKE_RETAINING_PID=$$
    FAKE_CHAR_FD=$CHAR_FD
    export FAKE_RETAINING_PID FAKE_CHAR_FD
    opener_gate
    exec {CHAR_FD}<&-
}

run_test_post_opener_gate()
{
    local command=$6
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 6 ] || die test_post_opener_gate_usage
    [ -x "$command" ] || die test_post_opener_command_not_executable
    "$command" || die test_post_opener_command_failed
    run_test_opener_gate "$1" "$2" "$3" "$4" "$5"
}

run_test_generic_char()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 5 ] || die test_generic_char_usage
    retain_generic_char_for_namespace "$3" "$4" "$1" "$2" "$5"
    printf 'GENERIC_CHAR_OK path=%s nsid=%s dev_t=%s retained=%s\n' \
        "$CHAR" "$CHAR_NSID" "$CHAR_DEV_T" "$CHAR_FD_PATH"
}

count_process_fds()
{
    local destination=$1 was_nullglob=0
    local descriptors=()
    shopt -q nullglob && was_nullglob=1
    shopt -s nullglob
    descriptors=(/proc/$$/fd/*)
    [ "$was_nullglob" -eq 1 ] || shopt -u nullglob
    printf -v "$destination" '%s' "${#descriptors[@]}"
}

run_test_generic_char_repeat()
{
    local generic_root=$1 dev_root=$2 block_owner=$3 expected_nsid=$4
    local nvme_bin=$5 repeats=$6 iteration before after first_after
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [[ $repeats =~ ^[0-9]+$ ]] && [ "$repeats" -ge 2 ] &&
        [ "$repeats" -le 20 ] || die test_generic_char_repeats_bad
    count_process_fds before
    for iteration in $(seq 1 "$repeats"); do
        retain_generic_char_for_namespace "$block_owner" "$expected_nsid" \
            "$generic_root" "$dev_root" "$nvme_bin"
        count_process_fds after
        if [ "$iteration" -eq 1 ]; then
            first_after=$after
            [ "$first_after" -eq $((before + 1)) ] ||
                die "generic_char_persistent_fd_delta=$((first_after - before))"
            if [ "${EXITOS_SCALE_TEST_PAUSE_GENERIC_REPEAT:-0}" = 1 ]; then
                echo GENERIC_CHAR_RETAINED_READY
                kill -STOP "$$"
            fi
        else
            [ "$after" -eq "$first_after" ] ||
                die "generic_char_fd_growth first=$first_after current=$after"
        fi
    done
    printf 'GENERIC_CHAR_REPEAT_OK repeats=%s persistent_delta=1 stable=yes\n' \
        "$repeats"
}

exclusive_io_gate()
{
    local before=() after=()

    opener_gate

    [[ "$IDLE_INTERVAL" =~ ^[0-9]+$ ]] &&
        [ "$IDLE_INTERVAL" -ge 1 ] && [ "$IDLE_INTERVAL" -le 30 ] ||
        die "bad_idle_interval=$IDLE_INTERVAL"
    read -r -a before < "/sys/block/$BASE/stat"
    [ "${#before[@]}" -ge 9 ] || die diskstats_short_before
    [ "${before[8]}" = 0 ] || die "disk_inflight_before=${before[8]}"
    sleep "$IDLE_INTERVAL"
    read -r -a after < "/sys/block/$BASE/stat"
    [ "${#after[@]}" -ge 9 ] || die diskstats_short_after
    [ "${after[8]}" = 0 ] || die "disk_inflight_after=${after[8]}"
    [ "${after[4]}" = "${before[4]}" ] &&
        [ "${after[6]}" = "${before[6]}" ] ||
        die "disk_write_drift_ios=${before[4]}:${after[4]} sectors=${before[6]}:${after[6]}"
}

identity_gate()
{
    resolve_identity
    if [ -n "${CAMPAIGN_GENERATION+x}" ]; then
        assert_campaign_generation
    fi
    exclusive_io_gate
    echo "PRE_CELL_IDENTITY_OK serial=$SERIAL wwid=$WWID pci=$PCI whole=$DEV part=$PART start=$start count=$count uuid=$UUID dev_t=$(cat /sys/block/$BASE/dev) part_dev_t=$(cat /sys/block/$BASE/${BASE}p1/dev) diskseq=$(cat /sys/block/$BASE/diskseq) boot_id=$(cat /proc/sys/kernel/random/boot_id) load1=$(cut -d' ' -f1 /proc/loadavg)"
}

cpu_map()
{
    local jobs last cpu sib pkg core key map i online online_path
    local cpus=() selected=()
    declare -A seen_core=()

    jobs=$1
    [ "$jobs" -ge 1 ] && [ "$jobs" -le "$MAX_JOBS" ] || die "bad_jobs=$jobs"
    IFS=',' read -r -a cpus <<< "$CPU_POOL"
    [ "${#cpus[@]}" -ge "$jobs" ] || die "cpu_pool_short=${#cpus[@]} jobs=$jobs"
    last=$((jobs - 1))
    for i in $(seq 0 "$last"); do
        cpu=${cpus[$i]}
        [[ "$cpu" =~ ^[0-9]+$ ]] || die "bad_cpu=$cpu"
        online_path="/sys/devices/system/cpu/cpu$cpu/online"
        if [ -e "$online_path" ]; then
            online=$(cat -- "$online_path") || die "cpu_online_read_failed=$cpu"
        else
            online=1
        fi
        [ "$online" = 1 ] || die "cpu_offline=$cpu"
        [ -e "/sys/devices/system/cpu/cpu$cpu/node1" ] || die "cpu_not_numa1=$cpu"
        pkg=$(cat -- "/sys/devices/system/cpu/cpu$cpu/topology/physical_package_id") ||
            die "cpu_package_read_failed=$cpu"
        core=$(cat -- "/sys/devices/system/cpu/cpu$cpu/topology/core_id") ||
            die "cpu_core_read_failed=$cpu"
        [[ $pkg =~ ^[0-9]+$ && $core =~ ^[0-9]+$ ]] ||
            die "cpu_topology_value_bad=$cpu:$pkg:$core"
        key=$pkg:$core
        [ -z "${seen_core[$key]+x}" ] || die "cpu_same_physical_core=$cpu:$key"
        seen_core[$key]=1
        sib=$(cat -- "/sys/devices/system/cpu/cpu$cpu/topology/thread_siblings_list") ||
            die "cpu_siblings_read_failed=$cpu"
        [[ $sib =~ ^[0-9,-]+$ ]] || die "cpu_siblings_value_bad=$cpu:$sib"
        case ",$sib," in
            *,$cpu,* ) ;;
            *) die "cpu_topology_bad=$cpu:$sib" ;;
        esac
        selected+=("$cpu")
    done
    map=$(IFS=,; echo "${selected[*]}")
    printf '%s' "$map"
}

cpu_quiet_window()
{
    local map=$1 output=$2

    [ -n "$output" ] && [ ! -e "$output" ] && [ ! -L "$output" ] ||
        die "unsafe_cpu_window_output=$output"
    (umask 077; set -o noclobber; : >"$output") ||
        die "cpu_window_output_create_failed=$output"
    if ! (continuous_noise_monitor preflight /proc /sys/devices/system/cpu \
            "$map" "$CPU_UTIL_WINDOW_MS" "$NOISE_POLL_MS" \
            "$CPU_UTIL_WINDOW_MS" "$CPU_UTIL_MAX") >>"$output" 2>&1; then
        sed -n '1,200p' "$output" >&2
        die "cpu_quiet_window_failed map=$map artifact=$output"
    fi
    echo "CPU_OBSERVATION_WINDOW_OK duration_ms=$CPU_UTIL_WINDOW_MS reference=$CPU_UTIL_MAX map=$map artifact=$output"
}

cpu_observation_stdout()
{
    local map=$1

    if ! continuous_noise_monitor preflight /proc /sys/devices/system/cpu \
            "$map" "$CPU_UTIL_WINDOW_MS" "$NOISE_POLL_MS" \
            "$CPU_UTIL_WINDOW_MS" "$CPU_UTIL_MAX"; then
        die "cpu_observation_window_failed map=$map artifact=stdout"
    fi
    echo "CPU_OBSERVATION_WINDOW_OK duration_ms=$CPU_UTIL_WINDOW_MS reference=$CPU_UTIL_MAX map=$map artifact=stdout"
}

emit_production_plan()
{
    local plan_map=$1 manifest_sha=$2
    echo "PLAN_OK result=none file_mib=$FILE_MIB runtime=$RUNTIME cpus=$plan_map arm=uringpoll mode=thread jobs=8 blocks=4 variants=gg,ss priority='SCHED_OTHER nice -20' poll_required=yes manifest_sha256=$manifest_sha artifacts=none"
}

run_test_production_plan()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_production_plan_usage
    emit_production_plan "$CPU_POOL" "$1"
}

prepare_files()
{
    local map seal_output rc group_clean=0
    identity_gate
    [ -n "${WORK_FD_PATH:-}" ] || die workdir_not_retained
    map=$(cpu_map "$MAX_JOBS")
    echo "PREPARE_BEGIN files=$MAX_JOBS size_mib=$FILE_MIB cpus=$map"
    start_bound_io "$map" "$RESULT_DIR/prepare-stdout.txt" \
        "$RESULT_DIR/prepare-stderr.txt" unsealed - - fio --name=prepare \
        '--filename_format=/proc/self/fd/10$jobnum' --rw=write --bs=1m \
        --size="${FILE_MIB}m" --direct=1 --ioengine=psync \
        --numjobs="$MAX_JOBS" --thread=1 --group_reporting=1 \
        --fallocate=none --fsync_on_close=1 --cpus_allowed="$map" \
        --cpus_allowed_policy=split --output-format=json \
        --output="$RESULT_DIR/prepare.json"
    if wait "$CELL_IO_PID"; then
        rc=0
    else
        rc=$?
    fi
    finish_bound_io_group_after_wait || group_clean=$?
    [ "$group_clean" -eq 0 ] || die bound_io_group_not_empty_prepare
    [ "$rc" -eq 0 ] || die "prepare_fio_rc=$rc"
    for i in $(seq 0 $((MAX_JOBS - 1))); do
        [ -f "$WORK_FD_PATH/w$i.log" ] || die "prepared_file_missing=$i"
        [ ! -L "$WORK_FD_PATH/w$i.log" ] || die "prepared_file_symlink=$i"
        [ "$(stat -Lc '%u:%a:%h:%s' "$WORK_FD_PATH/w$i.log")" = \
          "$(id -u):600:1:$((FILE_MIB * 1024 * 1024))" ] || die "prepared_file_meta=$i"
    done
    sync -f "$WORK_FD_PATH/w0.log"
    resolve_identity
    assert_campaign_generation
    seal_output=$(seal_workset "$CAMPAIGN_GENERATION_SHA" \
        "$((FILE_MIB * 1024 * 1024))") || die workset_seal_failed
    [[ $seal_output =~ WORKSET_SEALED[[:space:]]identity=([0-9]+:[0-9]+:[0-9]+:[0-7]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+) ]] ||
        die workset_seal_output_malformed
    WORKSET_MANIFEST_IDENTITY=${BASH_REMATCH[1]}
    export WORKSET_MANIFEST_IDENTITY
    printf '%s\n' "$seal_output"
    verify_workset post-prepare "$CAMPAIGN_GENERATION_SHA" \
        "$((FILE_MIB * 1024 * 1024))" "$WORKSET_MANIFEST_IDENTITY"
    echo "PREPARE_OK files=$MAX_JOBS size_mib=$FILE_MIB"
}

summarize_cell()
{
    local cell=$1 arm=$2 mode=$3 jobs=$4 rep=$5 variant=$6
    python3 - "$cell" "$arm" "$mode" "$jobs" "$rep" "$variant" <<'PY'
import glob,json,math,os,re,sys
cell,arm,mode,jobs,rep,variant=sys.argv[1:]
jobs=int(jobs)
with open(os.path.join(cell,"fio.json"),"r",encoding="utf-8") as f:
    d=json.load(f)
js=d.get("jobs",[])
if not js or any(int(j.get("error",0)) != 0 for j in js):
    raise SystemExit("fio JSON reports an error or no jobs")

def finite_nonnegative(name,value):
    try:
        number=float(value)
    except (TypeError,ValueError,OverflowError):
        raise SystemExit("fio JSON metric is not numeric: %s"%name)
    if not math.isfinite(number) or number < 0:
        raise SystemExit("fio JSON metric is not finite/nonnegative: %s"%name)
    return number

for index,j in enumerate(js):
    sync_n=j.get("sync",{}).get("lat_ns",{}).get("N",0)
    metrics=[
        ("error",j.get("error",0)),
        ("write.total_ios",j["write"]["total_ios"]),
        ("write.iops",j["write"]["iops"]),
        ("write.lat_ns.mean",j["write"]["lat_ns"]["mean"]),
        ("sync.lat_ns.N",sync_n),
        ("job_runtime",j.get("job_runtime",0)),
        ("usr_cpu",j.get("usr_cpu",0)),
        ("sys_cpu",j.get("sys_cpu",0)),
        ("ctx",j.get("ctx",0)),
    ]
    if finite_nonnegative("jobs[%d].sync.lat_ns.N"%index,sync_n) > 0:
        metrics.append(("sync.lat_ns.mean",j["sync"]["lat_ns"]["mean"]))
    for name,value in metrics:
        finite_nonnegative("jobs[%d].%s"%(index,name),value)

ios=sum(int(j["write"]["total_ios"]) for j in js)
iops=sum(float(j["write"]["iops"]) for j in js)
sync_ios=sum(int(j.get("sync",{}).get("lat_ns",{}).get("N",0)) for j in js)
if ios <= 0 or not math.isfinite(iops) or iops <= 0:
    raise SystemExit("fio JSON has a zero/non-finite summary denominator")
write_lat=sum(float(j["write"]["lat_ns"]["mean"])*int(j["write"]["total_ios"]) for j in js)/ios
if sync_ios:
    sync_lat=sum(float(j["sync"]["lat_ns"]["mean"])*int(j["sync"]["lat_ns"]["N"]) for j in js)/sync_ios
else:
    sync_lat=0.0
pair_us=jobs*1e6/iops
cpu_ns=sum(float(j.get("job_runtime",0))*1e6*
           (float(j.get("usr_cpu",0))+float(j.get("sys_cpu",0)))/100.0
           for j in js)
cpu_us_per_io=cpu_ns/ios/1000.0
ctx=sum(int(j.get("ctx",0)) for j in js)
for name,value in (("iops",iops),("write_lat",write_lat),
                   ("sync_lat",sync_lat),("pair_us",pair_us),
                   ("cpu_ns",cpu_ns),("cpu_us_per_io",cpu_us_per_io)):
    finite_nonnegative("summary.%s"%name,value)
stats=[0]*5
nonzero=0
paths=glob.glob(os.path.join(cell,"stats-*.txt"))
for p in paths:
    vals=[]
    with open(p,"r",encoding="ascii") as f:
        for line in f:
            line=line.strip()
            if line:
                vals.append(int(line))
    if len(vals) != 5:
        raise SystemExit("stats file must contain exactly five lines: "+p)
    if vals[0] > 0:
        nonzero += 1
    stats=[a+b for a,b in zip(stats,vals[:5])]
if arm != "base":
    if (stats[0] != ios or stats[1] != sync_ios or stats[2] != 0 or
            stats[3] != 0 or stats[4] != 0):
        raise SystemExit("takeover mismatch stats=%r ios=%d sync_ios=%d"%(stats,ios,sync_ios))
    expected_workers=1 if mode == "thread" else jobs
    if len(paths) != expected_workers or nonzero != expected_workers:
        raise SystemExit("worker stats mismatch files=%d nonzero=%d expected=%d paths=%r"%
                         (len(paths),nonzero,expected_workers,paths))
else:
    if paths:
        raise SystemExit("base unexpectedly produced stats files: %r"%paths)
    marker=re.compile(r"exitos.*(?:armed|register|takeover)",re.IGNORECASE)
    for name in ("stdout.txt","stderr.txt"):
        path=os.path.join(cell,name)
        if os.path.exists(path):
            with open(path,"r",encoding="utf-8",errors="replace") as source:
                if marker.search(source.read()):
                    raise SystemExit("base Exitos marker in "+name)
print("CELL_OK arm=%s mode=%s jobs=%d rep=%s variant=%s iops=%.3f pair_us=%.6f cpu_us_per_io=%.6f write_lat_us=%.6f sync_lat_us=%.6f ctx=%d ios=%d sync_ios=%d fast_write=%d fast_sync=%d pass=%d declined=%d refresh=%d stats_files=%d nonzero_stats=%d" %
      (arm,mode,jobs,rep,variant,iops,pair_us,cpu_us_per_io,write_lat/1000.0,
       sync_lat/1000.0,ctx,ios,sync_ios,*stats,len(paths),nonzero))
PY
}

summarize_endpoint()
{
    local input=$1
    python3 - "$input" <<'PY'
import statistics
import sys

path=sys.argv[1]
records={}
with open(path,"r",encoding="ascii") as source:
    for line in source:
        line=line.strip()
        if not line.startswith("CELL_OK "):
            continue
        fields={}
        for token in line.split()[1:]:
            if "=" in token:
                key,value=token.split("=",1)
                fields[key]=value
        if (fields.get("arm"),fields.get("mode"),fields.get("jobs")) != ("uringpoll","thread","8"):
            raise SystemExit("non-endpoint cell in endpoint summary")
        try:
            block=int(fields["rep"])
            variant=fields["variant"]
            pair_us=float(fields["pair_us"])
        except (KeyError,ValueError):
            raise SystemExit("malformed endpoint cell")
        if block not in range(1,5) or variant not in ("gg","ss") or pair_us<=0:
            raise SystemExit("invalid endpoint cell")
        key=(block,variant)
        if key in records:
            raise SystemExit("duplicate endpoint cell")
        records[key]=pair_us
expected={(block,variant) for block in range(1,5) for variant in ("gg","ss")}
if set(records)!=expected:
    raise SystemExit("incomplete endpoint blocks")
gg=[records[(block,"gg")] for block in range(1,5)]
ss=[records[(block,"ss")] for block in range(1,5)]
saved=[a-b for a,b in zip(gg,ss)]
gg_median=statistics.median(gg)
ss_median=statistics.median(ss)
saved_median=statistics.median(saved)
saved_pct=100.0*saved_median/gg_median
print("ENDPOINT_RESULT blocks=4 gg_pair_us_median=%.6f ss_pair_us_median=%.6f "
      "paired_saved_pair_us_median=%.6f paired_saved_percent_of_gg_median=%.6f" %
      (gg_median,ss_median,saved_median,saved_pct))
PY
}

summarize_factorial()
{
    local input=$1
    python3 - "$input" <<'PY'
import statistics
import sys

path = sys.argv[1]
records = {}
conditions = {
    ("base", "thread", 1, "base"),
    ("uringpoll", "thread", 1, "gg"),
    ("uringpoll", "thread", 1, "ss"),
    ("base", "thread", 8, "base"),
    ("uringpoll", "thread", 8, "gg"),
    ("uringpoll", "thread", 8, "gs"),
    ("uringpoll", "thread", 8, "sg"),
    ("uringpoll", "thread", 8, "ss"),
    ("base", "process", 8, "base"),
    ("uringpoll", "process", 8, "ss"),
}
with open(path, "r", encoding="ascii") as source:
    for line in source:
        line = line.strip()
        if not line.startswith("CELL_OK "):
            continue
        fields = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        try:
            block = int(fields["rep"])
            condition = (fields["arm"], fields["mode"], int(fields["jobs"]),
                         fields["variant"])
            pair_us = float(fields["pair_us"])
        except (KeyError, ValueError):
            raise SystemExit("malformed factorial cell")
        if block not in range(1, 5) or condition not in conditions or pair_us <= 0:
            raise SystemExit("invalid factorial cell")
        key = (block,) + condition
        if key in records:
            raise SystemExit("duplicate factorial cell")
        records[key] = pair_us

expected = {(block,) + condition for block in range(1, 5)
            for condition in conditions}
if set(records) != expected:
    raise SystemExit("incomplete factorial matrix")

def value(block, arm, mode, jobs, variant):
    return records[(block, arm, mode, jobs, variant)]

one_gg_residual = []
one_ss_residual = []
one_saved = []
table_global_stats = []
table_sharded_stats = []
stats_global_table = []
stats_sharded_table = []
interactions = []
thread_residual = []
process_residual = []
residual_did = []
for block in range(1, 5):
    one_base = value(block, "base", "thread", 1, "base")
    one_gg = value(block, "uringpoll", "thread", 1, "gg")
    one_ss = value(block, "uringpoll", "thread", 1, "ss")
    base_t = value(block, "base", "thread", 8, "base")
    gg = value(block, "uringpoll", "thread", 8, "gg")
    gs = value(block, "uringpoll", "thread", 8, "gs")
    sg = value(block, "uringpoll", "thread", 8, "sg")
    ss = value(block, "uringpoll", "thread", 8, "ss")
    base_p = value(block, "base", "process", 8, "base")
    ss_p = value(block, "uringpoll", "process", 8, "ss")
    one_gg_residual.append(one_gg - one_base)
    one_ss_residual.append(one_ss - one_base)
    one_saved.append(one_gg - one_ss)
    table_global_stats.append(gg - sg)
    table_sharded_stats.append(gs - ss)
    stats_global_table.append(gg - gs)
    stats_sharded_table.append(sg - ss)
    interactions.append(gg - gs - sg + ss)
    thread_residual.append(ss - base_t)
    process_residual.append(ss_p - base_p)
    residual_did.append((ss - base_t) - (ss_p - base_p))

print("FACTORIAL_RESULT blocks=4 "
      "one_thread_gg_residual_pair_us_median=%.6f "
      "one_thread_ss_residual_pair_us_median=%.6f "
      "one_thread_gg_to_ss_saved_pair_us_median=%.6f "
      "table_saved_global_stats_pair_us_median=%.6f "
      "table_saved_sharded_stats_pair_us_median=%.6f "
      "stats_saved_global_table_pair_us_median=%.6f "
      "stats_saved_sharded_table_pair_us_median=%.6f "
      "interaction_pair_us_median=%.6f "
      "eight_thread_ss_residual_pair_us_median=%.6f "
      "eight_process_ss_residual_pair_us_median=%.6f "
      "thread_process_residual_did_pair_us_median=%.6f" %
      (statistics.median(one_gg_residual),
       statistics.median(one_ss_residual),
       statistics.median(one_saved),
       statistics.median(table_global_stats),
       statistics.median(table_sharded_stats),
       statistics.median(stats_global_table),
       statistics.median(stats_sharded_table),
       statistics.median(interactions),
       statistics.median(thread_residual),
       statistics.median(process_residual),
       statistics.median(residual_did)))
PY
}

run_test_endpoint_summary()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_endpoint_summary_usage
    summarize_endpoint "$1"
}

run_test_factorial_summary()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 1 ] || die test_factorial_summary_usage
    summarize_factorial "$1"
}

validate_cell_shape()
{
    case "$1:$2:$3:$4" in
        base:thread:1:base|base:thread:8:base|base:process:8:base|\
        uringpoll:thread:1:gg|uringpoll:thread:1:ss|\
        uringpoll:thread:8:gg|uringpoll:thread:8:gs|\
        uringpoll:thread:8:sg|uringpoll:thread:8:ss|\
        uringpoll:process:8:ss) return 0 ;;
        *) return 1 ;;
    esac
}

run_test_cell_shape()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 4 ] || die test_cell_shape_usage
    validate_cell_shape "$@" || die "unsupported_cell_shape=$1:$2:$3:$4"
    echo "CELL_SHAPE_OK arm=$1 mode=$2 jobs=$3 variant=$4"
}

run_test_cell_summary()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 6 ] || die test_cell_summary_usage
    validate_cell_shape "$2" "$3" "$4" "$6" ||
        die "unsupported_cell_shape=$2:$3:$4:$6"
    summarize_cell "$@"
}

run_test_noise_monitor()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 8 ] || die test_noise_monitor_usage
    (continuous_noise_monitor "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8")
}

run_test_noise_defaults()
{
    local duration_ms
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    duration_ms=$(quiet_duration_ms)
    printf 'NOISE_DEFAULTS duration_ms=%s poll_ms=%s max_blind_ms=%s idle_window_ms=%s util_max=%s\n' \
        "$duration_ms" "$NOISE_POLL_MS" "$MAX_NOISE_BLIND_MS" \
        "$CPU_UTIL_WINDOW_MS" "$CPU_UTIL_MAX"
}

run_test_cell_noise()
{
    local proc_root=$1 cpu_root=$2 map=$3 sentinel=$4 post_marker=$5
    local summary_marker=$6 command=$7 rc

    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 7 ] || die test_cell_noise_usage
    [ ! -e "$post_marker" ] && [ ! -L "$post_marker" ] ||
        die unsafe_test_post_marker
    [ ! -e "$summary_marker" ] && [ ! -L "$summary_marker" ] ||
        die unsafe_test_summary_marker
    [ -x "$command" ] || die test_cell_command_not_executable
    CELL_IO_PID=
    CELL_IO_PGID=
    CELL_SENTINEL_PID=
    trap 'signal_cleanup_exit 129' HUP
    trap 'signal_cleanup_exit 130' INT
    trap 'signal_cleanup_exit 143' TERM
    start_cell_noise_sentinel "$sentinel" "$proc_root" "$cpu_root" "$map"
    start_bound_io "$map" "$sentinel.io-stdout" "$sentinel.io-stderr" \
        none - - "$command"
    if wait "$CELL_IO_PID"; then
        rc=0
    else
        rc=$?
    fi
    CELL_IO_PID=
    CELL_IO_PGID=
    finish_cell_noise_sentinel
    # These markers stand in for the production post-workset and identity
    # gates.  Noise is deliberately interpreted only after this point.
    (umask 077; set -o noclobber; : >"$post_marker") ||
        die test_post_marker_create_failed
    verify_cell_monitor test-cell
    [ "$rc" -eq 0 ] || die "test_cell_command_rc=$rc"
    (umask 077; set -o noclobber; : >"$summary_marker") ||
        die test_summary_marker_create_failed
    trap - HUP INT TERM
}

configure_cpufreq_test_contract()
{
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 4 ] || die test_cpufreq_contract_usage
    CPUFREQ_SYSROOT=$1
    CPUFREQ_LOCK_ROOT=$2
    CPU_POOL=$3
    CPUFREQ_TARGET_KHZ=$4
}

run_test_cpufreq_attestation_validate()
{
    [ "$#" -eq 4 ] || die test_cpufreq_attestation_validate_usage
    configure_cpufreq_test_contract "$@"
    initialize_cpufreq_attestation
}

run_test_cpufreq_attestation_generation()
{
    [ "$#" -eq 4 ] || die test_cpufreq_attestation_generation_usage
    configure_cpufreq_test_contract "$@"
    initialize_cpufreq_attestation
    cpufreq_generation_snapshot
}

run_test_cpufreq_mode_requirement()
{
    local mode=$1
    shift
    [ "$#" -eq 4 ] || die test_cpufreq_mode_requirement_usage
    configure_cpufreq_test_contract "$@"
    require_cpufreq_attestation_for_mode "$mode"
    if [ "$mode" = --plan ]; then
        printf 'CPUFREQ_MODE_BYPASS mode=--plan\n'
    fi
}

run_test_cpufreq_attestation_cell()
{
    local sysroot=$1 lockroot=$2 pool=$3 target=$4 meta=$5 pre=$6
    local post=$7 summary=$8 command=$9 rc cpu_window=$5.cpu-window
    [ "$#" -eq 9 ] || die test_cpufreq_attestation_cell_usage
    configure_cpufreq_test_contract "$sysroot" "$lockroot" "$pool" "$target"
    [ -x "$command" ] || die test_cpufreq_command_not_executable
    for output in "$meta" "$cpu_window" "$pre" "$post" "$summary"; do
        [ -n "$output" ] && [[ $output == /* ]] &&
            [ ! -e "$output" ] && [ ! -L "$output" ] ||
            die "unsafe_test_cpufreq_output=$output"
    done
    (umask 077; set -o noclobber; : >"$meta") ||
        die test_cpufreq_meta_create_failed
    initialize_cpufreq_attestation
    if [ "${EXITOS_SCALE_TEST_PAUSE_AFTER_CPUFREQ_INIT:-0}" = 1 ]; then
        echo CPUFREQ_TEST_INIT_READY
        kill -STOP "$$"
    fi
    cpufreq_cell_pre_gate test-fake none "$cpu_window" "$pre" "$meta"
    if (exec_bound_objects none - - "$command"); then
        rc=0
    else
        rc=$?
    fi
    # A failed command may already have issued writes.  Preserve the post-cell
    # profile and boundary observation before interpreting its return code.
    cpufreq_cell_post_gate "$post"
    [ "$rc" -eq 0 ] || die "test_cell_command_rc=$rc"
    (umask 077; set -o noclobber; : >"$summary") ||
        die test_cpufreq_summary_create_failed
}

run_test_cell_evidence()
{
    local cell=$1 proc_root=$2 sys_root=$3 cpu_root=$4 controller_root=$5
    local base=$6 controller=$7 pci=$8 char_dev_t=$9 map=${10}
    local command=${11} rc summary
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 11 ] || die test_cell_evidence_usage
    for root in "$cell" "$proc_root" "$sys_root" "$cpu_root" "$controller_root"; do
        [ -n "$root" ] && [[ $root == /tmp/* ]] ||
            die "test_cell_evidence_root_unsafe=$root"
    done
    [ -x "$command" ] || die test_cell_evidence_command_not_executable
    CELL_EVIDENCE_PROC_ROOT=$proc_root
    CELL_EVIDENCE_SYS_ROOT=$sys_root
    CELL_EVIDENCE_CPU_ROOT=$cpu_root
    CELL_EVIDENCE_CONTROLLER_ROOT=$controller_root
    CELL_EVIDENCE_BASE=$base
    CELL_EVIDENCE_CTRL=$controller
    CELL_EVIDENCE_PCI=$pci
    CELL_EVIDENCE_CHAR_DEV_T=$char_dev_t
    CELL_EVIDENCE_ACTIVE=0
    initialize_cell_evidence "$cell" "$map"
    CELL_IO_PID=
    CELL_IO_PGID=
    CELL_SENTINEL_PID=
    trap 'signal_cleanup_exit 129' HUP
    trap 'signal_cleanup_exit 130' INT
    trap 'signal_cleanup_exit 143' TERM
    "$command" &
    CELL_IO_PID=$!
    if wait "$CELL_IO_PID"; then
        rc=0
    else
        rc=$?
    fi
    record_cell_fio_rc "$rc" || die cell_evidence_rc_record_failed
    CELL_IO_PID=
    capture_cell_evidence_post || die cell_evidence_post_failed
    [ "$rc" -eq 0 ] || die "test_cell_command_rc=$rc"
    summary='CELL_OK arm=uringpoll mode=thread jobs=1 rep=1 variant=ss pair_us=1.000000'
    persist_cell_summary "$summary"
    case "${EVIDENCE_SUMMARY_MODE:-}" in
        '') ;;
        empty)
            chmod 0600 "$cell/cell-summary.txt" || die test_summary_chmod_failed
            : >"$cell/cell-summary.txt"
            chmod 0400 "$cell/cell-summary.txt" || die test_summary_chmod_failed
            ;;
        mismatch)
            chmod 0600 "$cell/cell-summary.txt" || die test_summary_chmod_failed
            printf '%s\n' 'CELL_OK mutated=yes' >"$cell/cell-summary.txt"
            chmod 0400 "$cell/cell-summary.txt" || die test_summary_chmod_failed
            ;;
        replace)
            mv "$cell/cell-summary.txt" "$cell/cell-summary.old" ||
                die test_summary_replace_failed
            printf '%s\n' "$summary" >"$cell/cell-summary.txt"
            chmod 0400 "$cell/cell-summary.txt" || die test_summary_chmod_failed
            ;;
        *) die test_summary_mode_invalid ;;
    esac
    seal_cell_evidence_success
    if [ -n "${EVIDENCE_GLOBAL_SUMMARY:-}" ]; then
        append_global_summary "$EVIDENCE_GLOBAL_SUMMARY" "$summary"
    fi
    trap - HUP INT TERM
}

run_test_cell_evidence_lifecycle()
{
    local root=$1 proc_root=$2 sys_root=$3 cpu_root=$4 controller_root=$5
    local base=$6 controller=$7 pci=$8 char_dev_t=$9 map=${10}
    local command=${11} cells=${12} before after cell summary
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    [ "$#" -eq 12 ] || die test_cell_evidence_lifecycle_usage
    [[ $root == /tmp/* ]] && [[ $cells =~ ^[1-9][0-9]*$ ]] ||
        die test_cell_evidence_lifecycle_unsafe
    CELL_EVIDENCE_PROC_ROOT=$proc_root
    CELL_EVIDENCE_SYS_ROOT=$sys_root
    CELL_EVIDENCE_CPU_ROOT=$cpu_root
    CELL_EVIDENCE_CONTROLLER_ROOT=$controller_root
    CELL_EVIDENCE_BASE=$base
    CELL_EVIDENCE_CTRL=$controller
    CELL_EVIDENCE_PCI=$pci
    CELL_EVIDENCE_CHAR_DEV_T=$char_dev_t
    CELL_EVIDENCE_ACTIVE=0
    CELL_EVIDENCE_DIR_FD=
    CELL_EVIDENCE_FD_PATH=
    CELL_EVIDENCE_IDENTITIES=
    mkdir -m 0700 "$root" || die test_lifecycle_root_create_failed
    before=$(find "/proc/$$/fd" -mindepth 1 -maxdepth 1 | wc -l)
    summary='CELL_OK arm=uringpoll mode=thread jobs=1 rep=1 variant=ss pair_us=1.000000'
    for ((cell = 1; cell <= cells; ++cell)); do
        mkdir -m 0700 "$root/cell-$cell" || die test_lifecycle_cell_create_failed
        initialize_cell_evidence "$root/cell-$cell" "$map"
        "$command" || die test_lifecycle_command_failed
        record_cell_fio_rc 0 || die test_lifecycle_rc_failed
        capture_cell_evidence_post || die test_lifecycle_post_failed
        persist_cell_summary "$summary"
        seal_cell_evidence_success
    done
    after=$(find "/proc/$$/fd" -mindepth 1 -maxdepth 1 | wc -l)
    printf 'CELL_EVIDENCE_LIFECYCLE_OK cells=%s fd_delta=%s active=%s dir_fd=%s fd_path=%s identities=%s\n' \
        "$cells" "$((after - before))" "$CELL_EVIDENCE_ACTIVE" \
        "${CELL_EVIDENCE_DIR_FD:-none}" "${CELL_EVIDENCE_FD_PATH:-none}" \
        "${CELL_EVIDENCE_IDENTITIES:-none}"
}

run_cell()
{
    local arm=$1 mode=$2 jobs=$3 rep=$4 variant=$5 thread map cell rc
    local so_sha so_identity SO summary
    local fallback backend_requested group_clean=0
    local worker_exit_stats_env=()
    validate_cell_shape "$arm" "$mode" "$jobs" "$variant" ||
        die "unsupported_cell_shape=$arm:$mode:$jobs:$variant"
    case "$arm" in
        base)
            SO=-
            so_sha=-
            so_identity=-
            backend_requested=base
            fallback=not-applicable
            ;;
        uringpoll)
            SO=${VARIANT_PATH[$variant]}
            so_sha=${VARIANT_SHA[$variant]}
            so_identity=${VARIANT_IDENTITY[$variant]}
            backend_requested=uringpoll
            fallback=forbidden
            ;;
    esac
    assert_scale_variants
    identity_gate
    verify_workset pre-cell "$CAMPAIGN_GENERATION_SHA" \
        "$((FILE_MIB * 1024 * 1024))" "$WORKSET_MANIFEST_IDENTITY"
    map=$(cpu_map "$jobs")
    case "$mode" in thread) thread=1;; process) thread=0;; *) die "bad_mode=$mode";; esac
    if [ "$mode" = process ] && [ "$arm" != base ]; then
        worker_exit_stats_env=(EXITOS_STATS_ON_WORKER_EXIT=1)
    else
        worker_exit_stats_env=(-u EXITOS_STATS_ON_WORKER_EXIT)
    fi
    cell="$RESULT_DIR/cell-block${rep}-${mode}-${jobs}-${arm}-${variant}"
    mkdir "$cell"
    printf 'arm=%s\nmode=%s\njobs=%s\nrep=%s\nvariant=%s\ncpus=%s\nso=%s\nso_sha256=%s\nso_identity=%s\nbackend_requested=%s\nfallback=%s\n' \
        "$arm" "$mode" "$jobs" "$rep" "$variant" "$map" "$SO" \
        "$so_sha" "$so_identity" "$backend_requested" "$fallback" \
        > "$cell/meta.txt"
    CELL_EVIDENCE_PROC_ROOT=/proc
    CELL_EVIDENCE_SYS_ROOT=/sys
    CELL_EVIDENCE_CPU_ROOT=/sys/devices/system/cpu
    CELL_EVIDENCE_CONTROLLER_ROOT=/sys/class/nvme/$CTRL
    CELL_EVIDENCE_BASE=$BASE
    CELL_EVIDENCE_CTRL=$CTRL
    CELL_EVIDENCE_PCI=$PCI
    CELL_EVIDENCE_CHAR_DEV_T=$(cat -- "/sys/class/nvme/$CTRL/dev") ||
        die controller_dev_t_read_failed
    initialize_cell_evidence "$cell" "$map"
    cpufreq_cell_pre_gate production "$map" \
        "$cell/prestart-cpu-window.txt" "$cell/cpufreq-pre.txt" \
        "$cell/meta.txt"

    common=(fio --name=wal '--filename_format=/proc/self/fd/10$jobnum'
        --rw=write --bs=4k --size="${FILE_MIB}m" --time_based=1
        --runtime="$RUNTIME" --direct=1 --ioengine=psync --fdatasync=1
        --numjobs="$jobs" --thread="$thread" --group_reporting=1
        --overwrite=1 --create_on_open=0 --fallocate=none
        --cpus_allowed="$map" --cpus_allowed_policy=split
        --output-format=json --output="$cell/fio.json")

    start_cell_noise_sentinel "$cell/noise-sentinel.txt" \
        /proc /sys/devices/system/cpu "$map"
    set +e
    if [ "$arm" = base ]; then
        start_bound_io "$map" "$cell/stdout.txt" "$cell/stderr.txt" \
            sealed - - \
            env -u EXITOS_VERIFY_IDENTITY -u EXITOS_STRICT -u EXITOS_STAGE \
            -u EXITOS_WINDOW_IN_PART -u EXITOS_FILES -u EXITOS_DEV \
            -u EXITOS_EXPECT_SERIAL -u EXITOS_IOPATH -u EXITOS_STATS \
            -u EXITOS_STATS_ON_WORKER_EXIT -u LD_PRELOAD "${common[@]}"
    else
        start_bound_io "$map" "$cell/stdout.txt" "$cell/stderr.txt" \
            sealed "$SO" "$so_identity" \
            env -u EXITOS_VERIFY_IDENTITY -u EXITOS_STRICT -u EXITOS_STAGE \
            -u EXITOS_WINDOW_IN_PART \
            "${worker_exit_stats_env[@]}" \
            EXITOS_FILES=/proc/self/fd/100:/proc/self/fd/101:/proc/self/fd/102:/proc/self/fd/103:/proc/self/fd/104:/proc/self/fd/105:/proc/self/fd/106:/proc/self/fd/107 \
            EXITOS_DEV="$PART" EXITOS_EXPECT_SERIAL="$WWID" \
            EXITOS_IOPATH=uringpoll \
            EXITOS_STATS="$cell/stats-%p.txt" LD_PRELOAD=/proc/self/fd/120 \
            "${common[@]}"
    fi
    wait "$CELL_IO_PID"
    rc=$?
    record_cell_fio_rc "$rc" || {
        set -e
        die "cell_evidence_rc_record_failed cell=$cell"
    }
    finish_bound_io_group_after_wait || group_clean=$?
    set -e
    capture_cell_evidence_post || die "cell_evidence_post_failed cell=$cell"
    opener_gate
    [ "$group_clean" -eq 0 ] || die "bound_io_group_not_empty cell=$cell"
    finish_cell_noise_sentinel
    cpufreq_cell_post_gate "$cell/cpufreq-post.txt"
    verify_workset post-cell "$CAMPAIGN_GENERATION_SHA" \
        "$((FILE_MIB * 1024 * 1024))" "$WORKSET_MANIFEST_IDENTITY"
    assert_scale_variants
    resolve_identity
    assert_campaign_generation post-cell
    verify_cell_monitor "$cell"
    [ "$rc" -eq 0 ] || die "fio_rc=$rc cell=$cell"
    summary=$(summarize_cell "$cell" "$arm" "$mode" "$jobs" "$rep" "$variant") ||
        die "cell_summary_failed cell=$cell"
    persist_cell_summary "$summary"
    seal_cell_evidence_success
    append_global_summary "$RESULT_DIR/summary.txt" "$summary"
    printf '%s\n' "$summary"
    sleep 2
}

factorial_order()
{
    case "$1" in
        1) printf '%s' 'thread:1:base:base thread:8:uringpoll:gg process:8:uringpoll:ss thread:1:uringpoll:ss thread:8:uringpoll:sg process:8:base:base thread:1:uringpoll:gg thread:8:base:base thread:8:uringpoll:ss thread:8:uringpoll:gs' ;;
        2) printf '%s' 'thread:8:uringpoll:gs thread:8:uringpoll:ss thread:8:base:base thread:1:uringpoll:gg process:8:base:base thread:8:uringpoll:sg thread:1:uringpoll:ss process:8:uringpoll:ss thread:8:uringpoll:gg thread:1:base:base' ;;
        3) printf '%s' 'thread:8:base:base thread:1:uringpoll:ss thread:8:uringpoll:gs process:8:base:base thread:8:uringpoll:gg thread:1:base:base process:8:uringpoll:ss thread:8:uringpoll:ss thread:1:uringpoll:gg thread:8:uringpoll:sg' ;;
        4) printf '%s' 'thread:8:uringpoll:sg thread:1:uringpoll:gg thread:8:uringpoll:ss process:8:uringpoll:ss thread:1:base:base thread:8:uringpoll:gg process:8:base:base thread:8:uringpoll:gs thread:1:uringpoll:ss thread:8:base:base' ;;
        *) die "bad_factorial_block=$1" ;;
    esac
}

run_test_factorial_order()
{
    local block=$1 order
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    order=$(factorial_order "$block")
    echo "FACTORIAL_ORDER block=$block cells=${order// /,}"
}

endpoint_order()
{
    case "$1" in
        1|3) printf '%s' 'gg ss' ;;
        2|4) printf '%s' 'ss gg' ;;
        *) die "bad_endpoint_block=$1" ;;
    esac
}

run_test_endpoint_order()
{
    local block=$1 order
    [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
    order=$(endpoint_order "$block")
    echo "ENDPOINT_ORDER block=$block variants=${order// /,}"
}

run_endpoint_pilot()
{
    local block variant order i
    for i in $(seq 0 $((MAX_JOBS - 1))); do
        [ -f "$WORK_FD_PATH/w$i.log" ] || die "run_requires_prepare_file=$i"
    done
    for block in 1 2 3 4; do
        order=$(endpoint_order "$block")
        for variant in $order; do
            run_cell uringpoll thread 8 "$block" "$variant"
        done
    done
    summarize_endpoint "$RESULT_DIR/summary.txt" |
        tee "$RESULT_DIR/endpoint-result.txt"
}

run_factorial_pilot()
{
    local block spec order mode jobs arm variant i
    for i in $(seq 0 $((MAX_JOBS - 1))); do
        [ -f "$WORK_FD_PATH/w$i.log" ] || die "run_requires_prepare_file=$i"
    done
    for block in 1 2 3 4; do
        order=$(factorial_order "$block")
        for spec in $order; do
            IFS=: read -r mode jobs arm variant <<<"$spec"
            run_cell "$arm" "$mode" "$jobs" "$block" "$variant"
        done
    done
    summarize_factorial "$RESULT_DIR/summary.txt" |
        tee "$RESULT_DIR/factorial-result.txt"
}

usage()
{
    echo "usage: $0 --plan | --pilot-endpoints | --pilot-factorial" >&2
    exit 2
}

case "${1:-}" in
    --test-result-root-required)
        shift
        run_test_result_root_required "$@"
        exit 0
        ;;
    --test-result-root-policy)
        shift
        run_test_result_root_policy "$@"
        exit 0
        ;;
    --test-workfile-init)
        [ "$#" -eq 2 ] || die test_workfile_init_usage
        run_test_workfile_init "$2"
        exit 0
        ;;
    --test-workset-seal)
        [ "$#" -eq 4 ] || die test_workset_seal_usage
        run_test_workset_seal "$2" "$3" "$4"
        exit 0
        ;;
    --test-workset-verify)
        [ "$#" -eq 5 ] || die test_workset_verify_usage
        run_test_workset_verify "$2" "$3" "$4" "$5"
        exit 0
        ;;
    --test-generation-freeze)
        [ "$#" -eq 2 ] || die test_generation_freeze_usage
        run_test_generation_freeze "$2"
        exit 0
        ;;
    --test-opener-gate)
        [ "$#" -eq 6 ] || die test_opener_gate_usage
        run_test_opener_gate "$2" "$3" "$4" "$5" "$6"
        exit 0
        ;;
    --test-post-opener-gate)
        shift
        run_test_post_opener_gate "$@"
        exit 0
        ;;
    --test-generic-char)
        [ "$#" -eq 6 ] || die test_generic_char_usage
        run_test_generic_char "$2" "$3" "$4" "$5" "$6"
        exit 0
        ;;
    --test-generic-char-repeat)
        [ "$#" -eq 7 ] || die test_generic_char_repeat_usage
        run_test_generic_char_repeat "$2" "$3" "$4" "$5" "$6" "$7"
        exit 0
        ;;
    --test-variants)
        [ "$#" -eq 2 ] || die test_variants_usage
        [ "${EXITOS_SCALE_TEST_MODE:-0}" = 1 ] || die test_mode_not_enabled
        validate_scale_variants "$2"
        exit 0
        ;;
    --test-endpoint-order)
        [ "$#" -eq 2 ] || die test_endpoint_order_usage
        run_test_endpoint_order "$2"
        exit 0
        ;;
    --test-endpoint-summary)
        [ "$#" -eq 2 ] || die test_endpoint_summary_usage
        run_test_endpoint_summary "$2"
        exit 0
        ;;
    --test-factorial-order)
        [ "$#" -eq 2 ] || die test_factorial_order_usage
        run_test_factorial_order "$2"
        exit 0
        ;;
    --test-factorial-summary)
        [ "$#" -eq 2 ] || die test_factorial_summary_usage
        run_test_factorial_summary "$2"
        exit 0
        ;;
    --test-cell-shape)
        shift
        run_test_cell_shape "$@"
        exit 0
        ;;
    --test-cell-summary)
        shift
        run_test_cell_summary "$@"
        exit 0
        ;;
    --test-post-cell-generation)
        [ "$#" -eq 3 ] || die test_post_cell_generation_usage
        run_test_post_cell_generation "$2" "$3"
        exit 0
        ;;
    --test-required-generation-read)
        [ "$#" -eq 2 ] || die test_required_generation_read_usage
        run_test_required_generation_read "$2"
        exit 0
        ;;
    --test-campaign-lock)
        [ "$#" -eq 2 ] || die test_campaign_lock_usage
        run_test_campaign_lock "$2"
        exit 0
        ;;
    --test-bound-workfiles)
        [ "$#" -eq 3 ] || die test_bound_workfiles_usage
        run_test_bound_workfiles "$2" "$3"
        exit 0
        ;;
    --test-bound-artifact)
        [ "$#" -eq 4 ] || die test_bound_artifact_usage
        run_test_bound_artifact "$2" "$3" "$4"
        exit 0
        ;;
    --test-bound-parent-death)
        [ "$#" -eq 2 ] || die test_bound_parent_death_usage
        run_test_bound_parent_death "$2"
        exit 0
        ;;
    --test-bound-group-exit)
        [ "$#" -eq 2 ] || die test_bound_group_exit_usage
        run_test_bound_group_exit "$2"
        exit 0
        ;;
    --test-production-plan)
        [ "$#" -eq 2 ] || die test_production_plan_usage
        run_test_production_plan "$2"
        exit 0
        ;;
    --test-priority-launch)
        [ "$#" -eq 4 ] || die test_priority_launch_usage
        run_test_priority_launch "$2" "$3" "$4"
        exit 0
        ;;
    --test-noise-monitor)
        [ "$#" -eq 9 ] || die test_noise_monitor_usage
        run_test_noise_monitor "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9"
        exit 0
        ;;
    --test-noise-defaults)
        [ "$#" -eq 1 ] || die test_noise_defaults_usage
        run_test_noise_defaults
        exit 0
        ;;
    --test-cell-noise)
        [ "$#" -eq 8 ] || die test_cell_noise_usage
        run_test_cell_noise "$2" "$3" "$4" "$5" "$6" "$7" "$8"
        exit 0
        ;;
    --test-cpufreq-attestation-validate)
        shift
        run_test_cpufreq_attestation_validate "$@"
        exit 0
        ;;
    --test-cpufreq-attestation-generation)
        shift
        run_test_cpufreq_attestation_generation "$@"
        exit 0
        ;;
    --test-cpufreq-mode-requirement)
        shift
        run_test_cpufreq_mode_requirement "$@"
        exit 0
        ;;
    --test-cpufreq-attestation-cell)
        shift
        run_test_cpufreq_attestation_cell "$@"
        exit 0
        ;;
    --test-cell-evidence)
        shift
        run_test_cell_evidence "$@"
        exit 0
        ;;
    --test-cell-evidence-lifecycle)
        shift
        run_test_cell_evidence_lifecycle "$@"
        exit 0
        ;;
esac

[ "$#" -eq 1 ] || usage
case "$1" in
    --plan|--pilot-endpoints|--pilot-factorial|--prepare|--pilot-thread|--pilot-process|--pilot-all|--pilot-endpoints-thread|--pilot-endpoints-process|--pilot-endpoints-all) ;;
    *) usage ;;
esac
require_result_root_for_mode "$1"
command -v fio >/dev/null || die fio_missing
command -v findmnt >/dev/null || die findmnt_missing
command -v fuser >/dev/null || die fuser_missing
NVME_BIN=$(command -v nvme) || die nvme_cli_missing
NVME_BIN=$(readlink -f -- "$NVME_BIN") || die nvme_cli_unresolvable
[ -x "$NVME_BIN" ] || die nvme_cli_not_executable
[ -n "$VARIANTS_DIR" ] || die EXITOS_SCALE_VARIANTS_DIR_missing
umask 077
validate_scale_variants "$VARIANTS_DIR"
require_cpufreq_attestation_for_mode "$1"

if [ "$1" = --plan ]; then
    identity_gate
    plan_map=$(cpu_map "$MAX_JOBS")
    cpu_observation_stdout "$plan_map"
    emit_production_plan "$plan_map" "$VARIANT_MANIFEST_SHA"
    exit 0
fi

prepare_result_root_and_lock
RESULT_DIR=$(mktemp -d "$RESULT_ROOT/pilot.XXXXXX")
trap 'campaign_exit $?' EXIT
trap 'signal_cleanup_exit 129' HUP
trap 'signal_cleanup_exit 130' INT
trap 'signal_cleanup_exit 143' TERM
freeze_campaign_generation

case "$1" in
    --pilot-endpoints)
        create_owned_workdir; prepare_files; quiet_window; run_endpoint_pilot ;;
    --pilot-factorial)
        create_owned_workdir; prepare_files; quiet_window; run_factorial_pilot ;;
    --prepare|--pilot-thread|--pilot-process|--pilot-all|--pilot-endpoints-thread|--pilot-endpoints-process|--pilot-endpoints-all)
        die split_campaign_disabled_use_integrated_endpoint_command ;;
    *) usage ;;
esac
