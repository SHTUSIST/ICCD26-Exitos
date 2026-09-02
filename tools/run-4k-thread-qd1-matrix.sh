#!/bin/bash
# Fixed 4 KiB/QD1 thread-scaling diagnostic.  This script assumes that the
# caller already owns and mounted the explicitly named test partition; PCI
# binding and restoration belong to the outer NVMe transaction.
set -Eeuo pipefail
umask 077

mode=plan
mount_dir=
result_dir=
cpus_csv=
active_so=
transformer=
fio_bin=${FIO_BIN:-fio}
device=
expect_identity=
file_mib=1024
logical_write_gib=32
cooldown_seconds=10
cell_timeout_seconds=3600
manifest_mode=self
script_dir=$(cd "$(dirname "$0")" && pwd -P)
validator=$script_dir/validate-4k-thread-qd1-evidence.py

usage()
{
    cat <<'EOF'
Usage: run-4k-thread-qd1-matrix.sh [--plan | --run] OPTIONS

Required:
  --mount ABS          mounted ext4 test partition
  --result-dir ABS     new evidence directory outside the measured mount
  --cpus CSV           at least eight distinct CPU IDs
  --active-so ABS      active-bpftime frontend DSO
  --transformer ABS    patched bpftime text-transformer DSO
  --device ABS         exact mounted partition (EXITOS_DEV)
  --expect-identity ID exact namespace WWID (EXITOS_EXPECT_SERIAL)

Options:
  --file-mib N         prepared file size per worker in MiB (default: 1024)
  --logical-write-gib N equal aggregate write volume per cell (default: 32)
  --cooldown-seconds N quiet delay after setup, before timed I/O (default: 10)
  --fio PATH           fio executable
  --manifest-mode MODE self (default) or outer

The matrix is deliberately immutable: 4 KiB, psync, QD1 per worker and 4T/8T.
B and C use the same active-bpftime frontend and donor preparation; B sets
EXITOS_FASTPATH=0 and C sets EXITOS_FASTPATH=1 with uringpoll.  Any backend or
thread override is rejected instead of silently changing the diagnostic.
EOF
}

die()
{
    printf 'THREAD_QD1_REFUSE %s\n' "$*" >&2
    exit 3
}

need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "missing_value=$1"; }
absolute() { [[ $2 == /* ]] || die "$1_not_absolute=$2"; }
positive() { [[ $1 =~ ^[1-9][0-9]*$ ]]; }
nonnegative() { [[ $1 =~ ^[0-9]+$ ]]; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --plan) mode=plan; shift ;;
        --run) mode=run; shift ;;
        --mount) need_value "$@"; mount_dir=$2; shift 2 ;;
        --result-dir) need_value "$@"; result_dir=$2; shift 2 ;;
        --cpus) need_value "$@"; cpus_csv=$2; shift 2 ;;
        --active-so) need_value "$@"; active_so=$2; shift 2 ;;
        --transformer) need_value "$@"; transformer=$2; shift 2 ;;
        --fio) need_value "$@"; fio_bin=$2; shift 2 ;;
        --device) need_value "$@"; device=$2; shift 2 ;;
        --expect-identity) need_value "$@"; expect_identity=$2; shift 2 ;;
        --file-mib) need_value "$@"; file_mib=$2; shift 2 ;;
        --logical-write-gib) need_value "$@"; logical_write_gib=$2; shift 2 ;;
        --cooldown-seconds) need_value "$@"; cooldown_seconds=$2; shift 2 ;;
        --manifest-mode) need_value "$@"; manifest_mode=$2; shift 2 ;;
        --backend|--threads|--bs|--iodepth)
            die "immutable_matrix_option=$1"
            ;;
        *) die "unknown_option=$1" ;;
    esac
done

for pair in "mount:$mount_dir" "result_dir:$result_dir" \
            "active_so:$active_so" \
            "transformer:$transformer" "device:$device"; do
    key=${pair%%:*}
    value=${pair#*:}
    [ -n "$value" ] || die "${key}_missing"
    absolute "$key" "$value"
done
[ -n "$expect_identity" ] || die expect_identity_missing
[ -n "$cpus_csv" ] || die cpus_missing
[ -d "$mount_dir" ] || die "mount_not_directory=$mount_dir"
mount_dir=$(readlink -f -- "$mount_dir") || die mount_canonicalize_failed
[ "$mount_dir" != / ] || die system_root_mount_forbidden
canonical_result=$(readlink -m -- "$result_dir") || die result_canonicalize_failed
[ "$canonical_result" = "$result_dir" ] || die "result_not_canonical=$result_dir"
[ -d "$(dirname -- "$result_dir")" ] || die "result_parent_missing=$result_dir"
[ ! -e "$result_dir" ] || die "result_exists=$result_dir"
case "$result_dir" in "$mount_dir"|"$mount_dir"/*) die result_inside_measured_mount ;; esac
[[ $cpus_csv =~ ^[0-9]+(,[0-9]+)*$ ]] || die "cpus_invalid=$cpus_csv"
IFS=, read -r -a cpus <<<"$cpus_csv"
[ "${#cpus[@]}" -ge 8 ] || die "cpus_need_eight=count:${#cpus[@]}"
declare -A seen_cpu=()
for cpu in "${cpus[@]}"; do
    [ -z "${seen_cpu[$cpu]+x}" ] || die "cpu_duplicate=$cpu"
    seen_cpu[$cpu]=1
done
positive "$file_mib" || die "file_mib_invalid=$file_mib"
positive "$logical_write_gib" || die "logical_write_gib_invalid=$logical_write_gib"
nonnegative "$cooldown_seconds" || die "cooldown_invalid=$cooldown_seconds"
[ "$cooldown_seconds" -le 60 ] || die "cooldown_too_large=$cooldown_seconds,max:60"
case "$manifest_mode" in self|outer) ;; *) die "manifest_mode_invalid=$manifest_mode" ;; esac
[[ $expect_identity != *$'\n'* ]] || die identity_contains_newline

if [[ $fio_bin == */* ]]; then
    absolute fio "$fio_bin"
    fio_resolved=$(readlink -f -- "$fio_bin") || die "fio_not_found=$fio_bin"
else
    fio_resolved=$(command -v -- "$fio_bin") || die "fio_not_found=$fio_bin"
fi

cpu4=$(IFS=,; printf '%s' "${cpus[*]:0:4}")
cpu8=$(IFS=,; printf '%s' "${cpus[*]:0:8}")

if [ "$mode" = plan ]; then
    cat <<EOF
PLAN_OK mode=plan configs=threads4:4,threads8:8 bs=4k per_worker_qd=1 ioengine=psync
PLAN_IO direct=1 fallocate=native(default) overwrite=0(default) create_on_open=0(default) fdatasync=1
PLAN_BACKEND backend=uringpoll opcode=IORING_OP_URING_CMD bypass_block_layer=1 fallback=forbidden
PLAN_ARMS arms=B:active-bpftime-prepared-ext4,C:active-bpftime-uringpoll
PLAN_DELTA B:EXITOS_FASTPATH=0 C:EXITOS_FASTPATH=1 frontend=identical preparation=identical
PLAN_ORDER blocks=4B-4C-8B-8C,8C-8B-4C-4B,4C-4B-8C-8B,8B-8C-4B-4C,8B-8C-4B-4C,4C-4B-8C-8B,8C-8B-4C-4B,4B-4C-8B-8C cooldown_seconds=$cooldown_seconds
PLAN_TIMING prepare_timing=serialized-setup_files->Nth-PREPARE_READY->ARM->selected-close/forget->SIGSTOP->cooldown->timed-pre-snapshot->SIGCONT->timed-io
PLAN_CPU cpus4=$cpu4 cpus8=$cpu8 scheduler=SCHED_OTHER nice=-20 scheduler-policy-pilots=4T+8T parent=verified-per-cell
PLAN_WORKLOAD file_mib_per_worker=$file_mib logical_write_gib_per_cell=$logical_write_gib fixed_bytes=1
PLAN_SYNC fdatasync=1 sync_ios=write_ios-jobs
PLAN_GATE per-config=C/B:-5pct+7of8 thread-scaling=(C8/B8)/(C4/B4):-5pct+7of8
PLAN_TARGET mount=$mount_dir device=$device expect_identity=$expect_identity
PLAN_CORRECTNESS correctness=one-untimed-complete-readback checksum_in_timed_path=forbidden
PLAN_ARTIFACT result_dir=$result_dir manifest_mode=$manifest_mode
PLAN_FIO $fio_resolved --rw=write --bs=4k --size=${file_mib}m --io_size=\<equal-per-job-bytes\> --direct=1 --ioengine=psync --iodepth=1 --fdatasync=1 --create_serialize=1 --numjobs=\<threads\> --thread=1
EOF
    exit 0
fi

case "$result_dir" in
    /root/exitos-artifacts/?*) ;;
    *) die "result_root_forbidden=$result_dir" ;;
esac

for path in "$active_so" "$transformer" "$validator"; do
    [ -r "$path" ] || die "input_unreadable=$path"
done
[ -x "$validator" ] || die "validator_not_executable=$validator"
[ -x "$fio_resolved" ] || die "fio_not_executable=$fio_resolved"
mountpoint -q -- "$mount_dir" || die "not_mounted=$mount_dir"
mount_record=$(findmnt -rn -T "$mount_dir" -o SOURCE,FSTYPE) ||
    die mount_source_unresolved
read -r mount_source mount_fstype mount_extra <<<"$mount_record"
[ -n "$mount_source" ] && [ "$mount_fstype" = ext4 ] &&
    [ -z "${mount_extra-}" ] || die "mount_not_exact_ext4=$mount_record"
[ -b "$device" ] || die "device_not_block=$device"
device_real=$(readlink -f -- "$device") || die device_canonicalize_failed
mount_real=$(readlink -f -- "$mount_source") || die mount_source_canonicalize_failed
[ "$device_real" = "$mount_real" ] ||
    die "device_not_mounted_partition=device:$device_real,mount:$mount_real"
part_base=${device_real##*/}
[ -r "/sys/class/block/$part_base/partition" ] || die "device_not_partition=$device_real"
whole_base=$(lsblk -ndo PKNAME "$device_real") || die parent_device_unresolved
[ -n "$whole_base" ] && [ -b "/dev/$whole_base" ] || die parent_device_missing
[ "$(cat "/sys/block/$whole_base/wwid")" = "$expect_identity" ] ||
    die namespace_identity_mismatch
[ "$(cat "/sys/block/$whole_base/queue/io_poll")" = 1 ] || die io_poll_disabled
[ "$(cat /sys/module/nvme/parameters/poll_queues)" -gt 0 ] || die poll_queues_disabled

mkdir -- "$result_dir"
mkdir -- "$result_dir/cells"
work_root=$mount_dir/.exitos-thread-q1-$$
[ ! -e "$work_root" ] || die "work_root_exists=$work_root"
mkdir -m 0700 -- "$work_root"
work_root_id=$(stat -Lc '%d:%i:%u:%a' "$work_root")
last_work=
current_session_pid=
current_fio_pid=

work_root_is_owned()
{
    [ -n "$work_root" ] && [ -d "$work_root" ] &&
    [ "$(dirname -- "$work_root")" = "$mount_dir" ] &&
    [ "$(stat -Lc '%d:%i:%u:%a' "$work_root" 2>/dev/null)" = "$work_root_id" ]
}

terminate_current_session()
{
    local attempt

    [ -n "$current_session_pid" ] || return 0
    if kill -0 -- "-$current_session_pid" 2>/dev/null; then
        kill -TERM -- "-$current_session_pid" 2>/dev/null || true
        kill -CONT -- "-$current_session_pid" 2>/dev/null || true
        for attempt in $(seq 1 50); do
            kill -0 -- "-$current_session_pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 -- "-$current_session_pid" 2>/dev/null; then
            kill -KILL -- "-$current_session_pid" 2>/dev/null || true
        fi
    fi
    wait "$current_session_pid" 2>/dev/null || true
    current_session_pid=
    current_fio_pid=
}

cleanup()
{
    local rc=$1
    trap - EXIT
    trap '' INT TERM HUP
    set +e
    terminate_current_session
    if work_root_is_owned; then rm -rf -- "$work_root"; fi
    exit "$rc"
}
trap 'cleanup $?' EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM
trap 'cleanup 129' HUP

clean_unsets=()
while IFS= read -r variable; do
    case "$variable" in
        EXITOS_*|LD_PRELOAD|LD_AUDIT) clean_unsets+=(-u "$variable") ;;
    esac
done < <(compgen -e)

fio_version=$(env "${clean_unsets[@]}" "$fio_resolved" --version)
fallocate_help=$(env "${clean_unsets[@]}" "$fio_resolved" --cmdhelp=fallocate)
overwrite_help=$(env "${clean_unsets[@]}" "$fio_resolved" --cmdhelp=overwrite)
create_help=$(env "${clean_unsets[@]}" "$fio_resolved" --cmdhelp=create_on_open)
[ "$fio_version" = fio-3.41 ] || die "fio_version_not_3.41=$fio_version"
grep -Eq '^[[:space:]]*default:[[:space:]]*native[[:space:]]*$' \
    <<<"$fallocate_help" || die fallocate_default_not_native
grep -Eq '^[[:space:]]*default:[[:space:]]*0[[:space:]]*$' \
    <<<"$overwrite_help" || die overwrite_default_not_zero
grep -Eq '^[[:space:]]*default:[[:space:]]*0[[:space:]]*$' \
    <<<"$create_help" || die create_on_open_default_not_zero
{
    printf 'fio_version=%s\n' "$fio_version"
    printf 'fallocate_expected=native\n%s\n' "$fallocate_help"
    printf 'overwrite_expected=0\n%s\n' "$overwrite_help"
    printf 'create_on_open_expected=0\n%s\n' "$create_help"
} >"$result_dir/fio-defaults.txt"

{
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
    printf 'mount=%s\nmount_source=%s\npartition=%s\nwhole=%s\n' \
        "$mount_dir" "$mount_source" "$device_real" "/dev/$whole_base"
    printf 'namespace_identity=%s\nio_poll=%s\npoll_queues=%s\n' \
        "$expect_identity" "$(cat "/sys/block/$whole_base/queue/io_poll")" \
        "$(cat /sys/module/nvme/parameters/poll_queues)"
    printf 'configs=threads4:4,threads8:8\nbs=4096\nper_worker_qd=1\n'
    printf 'frontend=active-bpftime\nbackend=uringpoll\n'
    printf 'file_mib_per_worker=%s\nlogical_write_gib_per_cell=%s\n' \
        "$file_mib" "$logical_write_gib"
    printf 'cooldown_seconds=%s\ncpus4=%s\ncpus8=%s\n' \
        "$cooldown_seconds" "$cpu4" "$cpu8"
    printf '%s\n' 'blocks=4B-4C-8B-8C,8C-8B-4C-4B,4C-4B-8C-8B,8B-8C-4B-4C,8B-8C-4B-4C,4C-4B-8C-8B,8C-8B-4C-4B,4B-4C-8B-8C'
} >"$result_dir/run-meta.txt"

env "${clean_unsets[@]}" nvme id-ctrl -o json "/dev/$whole_base" \
    >"$result_dir/controller-id.json"
warning_temp_kelvin=$(jq -er \
    '.wctemp | select(type == "number" and . > 273)' \
    "$result_dir/controller-id.json") || die controller_warning_temperature_invalid
printf 'warning_temp_kelvin=%s\n' "$warning_temp_kelvin" \
    >>"$result_dir/run-meta.txt"

snapshot_device()
{
    local prefix=$1 current_mount current_source current_fstype current_devt
    local current_extra
    local current_source_real queue
    local -a mq_queues

    current_mount=$(findmnt -rn -T "$mount_dir" -o SOURCE,FSTYPE,MAJ:MIN) ||
        die "snapshot_mount_missing=$prefix"
    read -r current_source current_fstype current_devt current_extra \
        <<<"$current_mount"
    [ -n "$current_source" ] && [ "$current_fstype" = ext4 ] &&
        [ -n "$current_devt" ] && [ -z "${current_extra-}" ] ||
        die "snapshot_mount_invalid=$prefix:$current_mount"
    current_source_real=$(readlink -f -- "$current_source") ||
        die "snapshot_mount_source_unresolved=$prefix"
    shopt -s nullglob
    mq_queues=(/sys/block/"$whole_base"/mq/*)
    shopt -u nullglob
    [ "${#mq_queues[@]}" -gt 0 ] || die "snapshot_mq_missing=$prefix"
    {
        printf 'time_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"
        printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
        printf 'partition_devt=%s\n' "$(cat "/sys/class/block/$part_base/dev")"
        printf 'whole_devt=%s\n' "$(cat "/sys/class/block/$whole_base/dev")"
        printf 'wwid=%s\n' "$(cat "/sys/block/$whole_base/wwid")"
        printf 'diskseq=%s\n' "$(cat "/sys/class/block/$whole_base/diskseq")"
        printf 'io_poll=%s\n' "$(cat "/sys/block/$whole_base/queue/io_poll")"
        printf 'poll_queues=%s\n' "$(cat /sys/module/nvme/parameters/poll_queues)"
        printf 'write_cache=%s\n' "$(cat "/sys/block/$whole_base/queue/write_cache")"
        printf 'mount_source=%s\nmount_fstype=%s\nmount_devt=%s\n' \
            "$current_source_real" "$current_fstype" "$current_devt"
        printf 'mq_count=%s\n' "${#mq_queues[@]}"
        for queue in "${mq_queues[@]}"; do
            printf 'mq.%s.cpu_list=' "${queue##*/}"
            cat "$queue/cpu_list"
        done
        printf 'stat='; cat "/sys/class/block/$whole_base/stat"
    } >"${prefix}-device.txt"
    env "${clean_unsets[@]}" nvme smart-log -o json "/dev/$whole_base" \
        >"${prefix}-smart.json"
    jq -e '
      has("temperature") and (.temperature | type == "number") and
      has("critical_warning") and (.critical_warning | type == "number") and
      has("media_errors") and (.media_errors | type == "number") and
      has("warning_temp_time") and (.warning_temp_time | type == "number") and
      has("critical_comp_time") and (.critical_comp_time | type == "number") and
      has("thm_temp1_total_time") and (.thm_temp1_total_time | type == "number") and
      has("thm_temp2_total_time") and (.thm_temp2_total_time | type == "number") and
      has("thm_temp1_trans_count") and (.thm_temp1_trans_count | type == "number") and
      has("thm_temp2_trans_count") and (.thm_temp2_trans_count | type == "number")
    ' "${prefix}-smart.json" >/dev/null || die "smart_schema_invalid=$prefix"
    grep -Ev '^(time_utc|stat)=' "${prefix}-device.txt" \
        >"${prefix}-device-stable.txt"
    jq -r '[.critical_warning,.media_errors,.warning_temp_time,
            .critical_comp_time,.thm_temp1_total_time,.thm_temp2_total_time,
            .thm_temp1_trans_count,.thm_temp2_trans_count] | @tsv' \
        "${prefix}-smart.json" \
        >"${prefix}-smart-stable.tsv"
}

verify_snapshot_against_entry()
{
    local prefix=$1

    cmp -s "$result_dir/campaign-entry-device-stable.txt" \
        "${prefix}-device-stable.txt" || die "device_generation_drift=$prefix"
    cmp -s "$result_dir/campaign-entry-smart-stable.tsv" \
        "${prefix}-smart-stable.tsv" || die "smart_counter_drift=$prefix"
}

run_pinned()
{
    local attest=$1 allowed=$2 run_cwd=$3
    shift 3
    exec setsid env EXITOS_RUNNER_ATTEST_PATH="$attest" \
      EXITOS_RUNNER_WORKING_DIR="$run_cwd" \
      chrt --other 0 nice -n -20 taskset -c "$allowed" \
        bash -c '
            set -e
            policy=$(chrt -p $$)
            nice_value=$(ps -o ni= -p $$ | tr -d "[:space:]")
            affinity=$(taskset -pc $$)
            {
                printf "%s\n" "$policy"
                printf "nice=%s\n" "$nice_value"
                printf "%s\n" "$affinity"
            } >"$EXITOS_RUNNER_ATTEST_PATH"
            grep -q "SCHED_OTHER" <<<"$policy"
            test "$nice_value" = -20
            cd -- "$EXITOS_RUNNER_WORKING_DIR"
            unset EXITOS_RUNNER_ATTEST_PATH EXITOS_RUNNER_WORKING_DIR
            exec "$@"
        ' bash "$@"
}

find_fio_in_session()
{
    local candidate exe

    while IFS= read -r candidate; do
        [[ $candidate =~ ^[1-9][0-9]*$ ]] || continue
        exe=$(readlink -f -- "/proc/$candidate/exe" 2>/dev/null) || continue
        [ "$exe" = "$fio_resolved" ] && printf '%s\n' "$candidate"
    done < <(pgrep -s "$1" 2>/dev/null || true)
}

task_state()
{
    awk '{ line=$0; sub(/^.*\) /, "", line); split(line, field, " "); print field[1] }' \
        "/proc/$1/stat"
}

task_nice()
{
    awk '{ line=$0; sub(/^.*\) /, "", line); split(line, field, " "); print field[17] }' \
        "$1"
}

attest_session_leader()
{
    local pid=$1 evidence=$2 deadline=$((SECONDS + 3)) sid pgid

    while [ "$SECONDS" -lt "$deadline" ]; do
        sid=$(ps -o sid= -p "$pid" 2>/dev/null | tr -d '[:space:]')
        pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d '[:space:]')
        if [ "$sid" = "$pid" ] && [ "$pgid" = "$pid" ]; then
            printf 'pid=%s\nsid=%s\npgid=%s\n' "$pid" "$sid" "$pgid" \
                >"$evidence"
            return 0
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.02
    done
    die "session_leader_not_attested=$pid,sid:${sid-},pgid:${pgid-}"
}

wait_for_prepare_stop()
{
    local cell=$1 deadline=$((SECONDS + 120)) marker_count state early_rc
    local -a fio_pids

    while [ "$SECONDS" -lt "$deadline" ]; do
        if ! kill -0 "$current_session_pid" 2>/dev/null; then
            set +e
            wait "$current_session_pid"
            early_rc=$?
            set -e
            current_session_pid=
            die "fio_exited_before_prepare_stop=rc:$early_rc"
        fi
        marker_count=$(grep -c '^PREPARE_STOP_READY v=1 frontend=bpftime ' \
            "$cell/stderr.txt" 2>/dev/null || true)
        [ "$marker_count" -le 1 ] || die "prepare_stop_marker_duplicate=$cell"
        mapfile -t fio_pids < <(find_fio_in_session "$current_session_pid")
        [ "${#fio_pids[@]}" -le 1 ] || die "multiple_fio_processes=$cell"
        if [ "$marker_count" -eq 1 ] && [ "${#fio_pids[@]}" -eq 1 ]; then
            state=$(task_state "${fio_pids[0]}") || state=
            if [[ $state == T || $state == t ]]; then
                current_fio_pid=${fio_pids[0]}
                printf 'session_pid=%s\nfio_pid=%s\nstate=%s\nready_utc=%s\n' \
                    "$current_session_pid" "$current_fio_pid" "$state" \
                    "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)" \
                    >"$cell/prepare-stop-attestation.txt"
                [ "$(grep -c '^PREPARE_STOP_RELEASED ' "$cell/stderr.txt" || true)" -eq 0 ] ||
                    die "prepare_released_before_controller=$cell"
                return 0
            fi
        fi
        sleep 0.05
    done
    die "prepare_stop_timeout=$cell"
}

revalidate_stopped_fio()
{
    local cell=$1 state sid pgid exe marker_count released_count
    local -a fio_pids

    [ -n "$current_session_pid" ] && [ -n "$current_fio_pid" ] ||
        die "release_identity_missing=$cell"
    kill -0 -- "-$current_session_pid" 2>/dev/null ||
        die "release_session_missing=$cell"
    mapfile -t fio_pids < <(find_fio_in_session "$current_session_pid")
    [ "${#fio_pids[@]}" -eq 1 ] && [ "${fio_pids[0]}" = "$current_fio_pid" ] ||
        die "release_fio_identity_changed=$cell,count:${#fio_pids[@]}"
    sid=$(ps -o sid= -p "$current_fio_pid" 2>/dev/null | tr -d '[:space:]')
    pgid=$(ps -o pgid= -p "$current_fio_pid" 2>/dev/null | tr -d '[:space:]')
    [ "$sid" = "$current_session_pid" ] && [ "$pgid" = "$current_session_pid" ] ||
        die "release_session_identity_changed=$cell,sid:$sid,pgid:$pgid"
    exe=$(readlink -f -- "/proc/$current_fio_pid/exe" 2>/dev/null) ||
        die "release_fio_exe_unreadable=$cell"
    [ "$exe" = "$fio_resolved" ] || die "release_fio_exe_changed=$cell:$exe"
    state=$(task_state "$current_fio_pid" 2>/dev/null) ||
        die "release_fio_state_unreadable=$cell"
    [[ $state == T || $state == t ]] || die "release_fio_not_stopped=$cell:$state"
    marker_count=$(grep -c '^PREPARE_STOP_READY v=1 frontend=bpftime ' \
        "$cell/stderr.txt" 2>/dev/null || true)
    released_count=$(grep -c '^PREPARE_STOP_RELEASED ' \
        "$cell/stderr.txt" 2>/dev/null || true)
    [ "$marker_count" -eq 1 ] && [ "$released_count" -eq 0 ] ||
        die "release_marker_state_invalid=$cell,ready:$marker_count,released:$released_count"
    printf 'session_pid=%s\nfio_pid=%s\nsid=%s\npgid=%s\nexe=%s\nstate=%s\n' \
        "$current_session_pid" "$current_fio_pid" "$sid" "$pgid" "$exe" "$state" \
        >"$cell/prepare-release-attestation.txt"
}

wait_for_target_quiet()
{
    local cell=$1 attempt previous current previous_stat current_stat
    local sync_started sync_finished

    snapshot_device "$cell/prep-complete"
    verify_snapshot_against_entry "$cell/prep-complete"
    sync_started=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
    env "${clean_unsets[@]}" sync -f -- "$mount_dir" ||
        die "prepare_syncfs_failed=$cell"
    sync_finished=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
    printf 'started_utc=%s\nfinished_utc=%s\nmount=%s\nmethod=syncfs\n' \
        "$sync_started" "$sync_finished" "$mount_dir" \
        >"$cell/pre-timed-syncfs.txt"
    snapshot_device "$cell/post-syncfs"
    verify_snapshot_against_entry "$cell/post-syncfs"
    if [ "$cooldown_seconds" -gt 0 ]; then sleep "$cooldown_seconds"; fi
    previous=$cell/quiet-0
    snapshot_device "$previous"
    verify_snapshot_against_entry "$previous"
    for attempt in $(seq 1 10); do
        sleep 2
        current=$cell/quiet-$attempt
        snapshot_device "$current"
        verify_snapshot_against_entry "$current"
        previous_stat=$(sed -n 's/^stat=//p' "$previous-device.txt")
        current_stat=$(sed -n 's/^stat=//p' "$current-device.txt")
        if [ "$previous_stat" = "$current_stat" ]; then
            cp -- "$current-device.txt" "$cell/timed-pre-device.txt"
            cp -- "$current-device-stable.txt" "$cell/timed-pre-device-stable.txt"
            cp -- "$current-smart.json" "$cell/timed-pre-smart.json"
            cp -- "$current-smart-stable.tsv" "$cell/timed-pre-smart-stable.tsv"
            {
                printf 'minimum_cooldown_seconds=%s\n' "$cooldown_seconds"
                printf 'quiet_interval_seconds=2\nquiet_attempt=%s\n' "$attempt"
                printf 'previous=%s\ncurrent=%s\nstat=%s\n' \
                    "${previous##*/}" "${current##*/}" "$current_stat"
            } >"$cell/quiet-gate.txt"
            return 0
        fi
        previous=$current
    done
    die "target_not_quiet_after_prepare=$cell"
}

attest_fio_tasks()
{
    local cell=$1 jobs=$2 allowed=$3 deadline=$((SECONDS + 10))
    local tid policy nice_value affinity comm singleton_count cpu sample_ok
    local scheduler_snapshot_converged=0
    local scheduler_snapshot_tmp=$cell/.scheduler-workers.$$.tmp
    local last_observation=none
    local -a tids=() expected_cpus
    declare -A singleton_seen=()

    IFS=, read -r -a expected_cpus <<<"$allowed"
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ -d "/proc/$current_fio_pid/task" ] || { sleep 0.02; continue; }
        mapfile -t tids < <(find "/proc/$current_fio_pid/task" -mindepth 1 \
            -maxdepth 1 -type d -printf '%f\n' | sort -n)
        if [ "${#tids[@]}" -lt $((jobs + 1)) ]; then
            last_observation="tasks:${#tids[@]},need:$((jobs + 1))"
            sleep 0.02
            continue
        fi

        sample_ok=1
        singleton_count=0
        singleton_seen=()
        : >"$scheduler_snapshot_tmp"
        for tid in "${tids[@]}"; do
            if ! policy=$(chrt -p "$tid" 2>/dev/null) ||
               ! grep -q 'SCHED_OTHER' <<<"$policy"; then
                sample_ok=0
                break
            fi
            if ! nice_value=$(task_nice "/proc/$current_fio_pid/task/$tid/stat" 2>/dev/null) ||
               [ "$nice_value" != -20 ]; then
                sample_ok=0
                break
            fi
            if ! affinity=$(taskset -pc "$tid" 2>/dev/null | sed 's/^.*: //') ||
               ! comm=$(cat "/proc/$current_fio_pid/task/$tid/comm" 2>/dev/null); then
                sample_ok=0
                break
            fi
            printf 'tid=%s policy=SCHED_OTHER nice=%s affinity=%s comm=%s\n' \
                "$tid" "$nice_value" "$affinity" "$comm" \
                >>"$scheduler_snapshot_tmp"
            if [[ $affinity =~ ^[0-9]+$ ]]; then
                singleton_seen[$affinity]=$(( ${singleton_seen[$affinity]:-0} + 1 ))
                singleton_count=$((singleton_count + 1))
            fi
        done
        last_observation="tasks:${#tids[@]},singletons:$singleton_count,sample_ok:$sample_ok"
        if [ "$sample_ok" -eq 1 ] && [ "$singleton_count" -eq "$jobs" ]; then
            for cpu in "${expected_cpus[@]}"; do
                if [ "${singleton_seen[$cpu]:-0}" -ne 1 ]; then
                    sample_ok=0
                    break
                fi
            done
        else
            sample_ok=0
        fi
        if [ "$sample_ok" -eq 1 ]; then
            mv -- "$scheduler_snapshot_tmp" "$cell/scheduler-workers.txt"
            scheduler_snapshot_converged=1
            break
        fi
        sleep 0.02
    done
    rm -f -- "$scheduler_snapshot_tmp"
    [ "$scheduler_snapshot_converged" -eq 1 ] ||
        die "fio_worker_scheduler_not_converged=$cell,$last_observation"
}

run_scheduler_pilot()
{
    local jobs=$1 allowed=$2 pilot=$result_dir/scheduler-pilot-$1
    local deadline=$((SECONDS + 10)) pilot_rc
    local -a fio_pids

    mkdir -m 0700 -- "$pilot"
    run_pinned "$pilot/scheduler-parent.txt" "$allowed" "$pilot" \
        timeout --signal=TERM --kill-after=5 30 env "${clean_unsets[@]}" \
        "$fio_resolved" --name=scheduler-pilot --ioengine=null --rw=write \
        --bs=4k --size=1g --time_based=1 --runtime=10 \
        "--numjobs=$jobs" --thread=1 "--cpus_allowed=$allowed" \
        --cpus_allowed_policy=split --output-format=json \
        "--output=$pilot/fio.json" --eta=never \
        >"$pilot/stdout.txt" 2>"$pilot/stderr.txt" &
    current_session_pid=$!
    attest_session_leader "$current_session_pid" "$pilot/session.txt"
    while [ "$SECONDS" -lt "$deadline" ]; do
        mapfile -t fio_pids < <(find_fio_in_session "$current_session_pid")
        if [ "${#fio_pids[@]}" -eq 1 ]; then
            current_fio_pid=${fio_pids[0]}
            break
        fi
        kill -0 "$current_session_pid" 2>/dev/null || break
        sleep 0.02
    done
    [ -n "$current_fio_pid" ] || die "scheduler_pilot_fio_not_observed=$jobs"
    attest_fio_tasks "$pilot" "$jobs" "$allowed"
    set +e
    wait "$current_session_pid"
    pilot_rc=$?
    set -e
    current_session_pid=
    current_fio_pid=
    printf '%s\n' "$pilot_rc" >"$pilot/rc.txt"
    [ "$pilot_rc" -eq 0 ] || die "scheduler_pilot_failed=$jobs:$pilot_rc"
    jq -e --argjson jobs "$jobs" \
        '(.jobs | length) == $jobs and all(.jobs[]; .error == 0)' \
        "$pilot/fio.json" >/dev/null || die "scheduler_pilot_job_error=$jobs"
}

run_scheduler_pilot 4 "$cpu4"
run_scheduler_pilot 8 "$cpu8"

snapshot_device "$result_dir/campaign-entry"
[ "$(jq -r '.critical_warning' "$result_dir/campaign-entry-smart.json")" = 0 ] ||
    die campaign_entry_critical_warning
[ "$(sed -n 's/^write_cache=//p' "$result_dir/campaign-entry-device.txt")" = \
  'write through' ] || die campaign_entry_write_cache_not_through

printf 'block\tposition\tconfig\tjobs\tarm\tiops\tpair_us\twrite_ios\tsync_ios\twrite_lat_us\tsync_lat_us\tfast_write\tfast_sync\tpass\tdeclined\trefresh\thook_entries\thook_claimed\ttimed_pre_temp_c\ttimed_post_temp_c\n' \
    >"$result_dir/results.tsv"

run_cell()
{
    local block=$1 position=$2 jobs=$3 arm=$4 config allowed cell_name cell
    local cell_work donor_dir selector filename_format per_job_mib fio_rc
    local iops write_ios sync_ios pair_us write_lat_us sync_lat_us stats_path line
    local hook_entries hook_claimed expected_ios pre_temp post_temp fastpath
    local pre_temp_raw post_temp_raw
    local -a common stats stats_paths targets

    case "$jobs" in
        4) config=threads4; allowed=$cpu4 ;;
        8) config=threads8; allowed=$cpu8 ;;
        *) die "internal_jobs_invalid=$jobs" ;;
    esac
    case "$arm" in B|C) ;; *) die "internal_arm_invalid=$arm" ;; esac
    per_job_mib=$((logical_write_gib * 1024 / jobs))
    expected_ios=$((logical_write_gib * 1024 * 1024 * 1024 / 4096))
    cell_name=block${block}-pos${position}-${config}-${arm}
    cell=$result_dir/cells/$cell_name
    cell_work=$work_root/$cell_name
    donor_dir=$cell_work/donors
    selector=$cell_work/job.
    filename_format=$cell_work/job.\$jobnum.dat
    mkdir -m 0700 -- "$cell" "$cell_work" "$donor_dir"
    [ "$(stat -c %a "$donor_dir")" = 700 ] || die "donor_mode=$donor_dir"
    snapshot_device "$cell/cell-entry"
    verify_snapshot_against_entry "$cell/cell-entry"
    fastpath=$([ "$arm" = B ] && printf 0 || printf 1)

    common=("$fio_resolved" --name=thread-q1
        "--filename_format=$filename_format" --rw=write --bs=4k
        "--size=${file_mib}m" "--io_size=${per_job_mib}m" --direct=1
        --ioengine=psync --iodepth=1 --fdatasync=1 --create_serialize=1 \
        "--numjobs=$jobs"
        --thread=1 --buffer_pattern=0x5a
        --scramble_buffers=0 "--cpus_allowed=$allowed"
        --cpus_allowed_policy=split --output-format=json
        "--output=$cell/fio.json" --eta=never)
    {
        printf '%q' "${common[0]}"
        printf ' %q' "${common[@]:1}"
        printf '\n'
    } >"$cell/argv.txt"
    {
        printf 'frontend=active-bpftime\nbackend=uringpoll\n'
        printf 'prepare=donor-fallocate\ndonor_files=%s\n' "$jobs"
        printf 'fastpath=%s\nselector=%s\n' \
            "$fastpath" "$selector"
        printf 'prepare_ready_stop=1\nprepare_expected=%s\n' "$jobs"
        printf 'prepare_stop_point=after-selected-close\n'
        printf 'block=%s\nposition=%s\nconfig=%s\narm=%s\n' \
            "$block" "$position" "$config" "$arm"
    } >"$cell/environment.txt"
    run_pinned "$cell/scheduler.txt" "$allowed" "$cell" \
        timeout --signal=TERM --kill-after=10 "$cell_timeout_seconds" \
        env "${clean_unsets[@]}" \
        LD_PRELOAD="$active_so" EXITOS_BPFTIME_SO="$transformer" \
        EXITOS_FILES="$selector" EXITOS_PREPARE=donor-fallocate \
        EXITOS_DONOR_DIR="$donor_dir" EXITOS_DONOR_FILES="$jobs" \
        EXITOS_PREPARE_READY_STOP=1 EXITOS_PREPARE_EXPECTED="$jobs" \
        EXITOS_FASTPATH="$fastpath" \
        EXITOS_DEV="$device_real" EXITOS_EXPECT_SERIAL="$expect_identity" \
        EXITOS_IOPATH=uringpoll EXITOS_STAGE=0 EXITOS_STRICT=0 \
        EXITOS_VERIFY_IDENTITY=0 EXITOS_VERBOSE=1 \
        EXITOS_STATS="$cell/stats-%p.txt" "${common[@]}" \
        >"$cell/stdout.txt" 2>"$cell/stderr.txt" &
    current_session_pid=$!
    attest_session_leader "$current_session_pid" "$cell/session.txt"
    wait_for_prepare_stop "$cell"
    wait_for_target_quiet "$cell"
    revalidate_stopped_fio "$cell"
    printf 'released_utc=%s\nfio_pid=%s\nsignal=SIGCONT\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)" "$current_fio_pid" \
        >"$cell/prepare-release.txt"
    kill -CONT "$current_fio_pid" || die "prepare_sigcont_failed=$cell"
    set +e
    wait "$current_session_pid"
    fio_rc=$?
    set -e
    current_session_pid=
    current_fio_pid=
    printf '%s\n' "$fio_rc" >"$cell/rc.txt"
    [ "$fio_rc" -eq 0 ] || die "fio_failed=cell:$cell_name,rc:$fio_rc"
    snapshot_device "$cell/timed-post"
    verify_snapshot_against_entry "$cell/timed-post"
    iops=$(jq -r '[.jobs[].write.iops] | add' "$cell/fio.json")
    write_ios=$(jq -r '[.jobs[].write.total_ios] | add' "$cell/fio.json")
    sync_ios=$(jq -r '[.jobs[] | (.sync.lat_ns.N // 0)] | add' "$cell/fio.json")
    write_lat_us=$(jq -r '
      ([.jobs[].write.total_ios] | add) as $n |
      ([.jobs[] | .write.total_ios * .write.lat_ns.mean] | add) / $n / 1000
    ' "$cell/fio.json")
    sync_lat_us=$(jq -r '
      ([.jobs[] | (.sync.lat_ns.N // 0)] | add) as $n |
      ([.jobs[] | (.sync.lat_ns.N // 0) * (.sync.lat_ns.mean // 0)] | add) / $n / 1000
    ' "$cell/fio.json")
    [ "$write_ios" -eq "$expected_ios" ] ||
        die "write_count=cell:$cell_name,got:$write_ios,expected:$expected_ios"
    [ "$sync_ios" -eq $((expected_ios - jobs)) ] ||
        die "sync_count=cell:$cell_name,got:$sync_ios,expected:$((expected_ios - jobs))"
    pair_us=$(awk -v j="$jobs" -v i="$iops" 'BEGIN { printf "%.9f", j*1000000/i }')

    mapfile -t stats_paths < <(find "$cell" -maxdepth 1 -type f -name 'stats-*.txt')
    [ "${#stats_paths[@]}" -eq 1 ] ||
        die "stats_count=cell:$cell_name,count:${#stats_paths[@]}"
    stats_path=${stats_paths[0]}
    mapfile -t stats <"$stats_path"
    "$validator" validate-cell --fio-json "$cell/fio.json" \
        --stderr "$cell/stderr.txt" --stats "$stats_path" --arm "$arm" \
        --jobs "$jobs" --file-bytes "$((file_mib * 1024 * 1024))" \
        --expected-write-ios "$expected_ios" --frontend bpftime \
        >"$cell/evidence-validation.json"
    line=$(grep -E 'hook entered [0-9]+ times, claimed [0-9]+' "$cell/stderr.txt" | tail -1)
    hook_entries=$(sed -n 's/.*hook entered \([0-9][0-9]*\) times.*/\1/p' <<<"$line")
    hook_claimed=$(sed -n 's/.*claimed \([0-9][0-9]*\).*/\1/p' <<<"$line")
    [ "$hook_entries" -gt 0 ] && [ "$hook_claimed" -gt 0 ] ||
        die "hook_counter_zero=cell:$cell_name"

    mapfile -d '' targets < <(
        find "$cell_work" -maxdepth 1 -type f -name 'job.*.dat' -print0 | sort -z
    )
    [ "${#targets[@]}" -eq "$jobs" ] ||
        die "target_count=cell:$cell_name,count:${#targets[@]}"
    printf '%s\n' "${targets[@]}" >"$cell/targets.txt"
    pre_temp_raw=$(jq -r '.temperature' "$cell/timed-pre-smart.json")
    post_temp_raw=$(jq -r '.temperature' "$cell/timed-post-smart.json")
    [ "$pre_temp_raw" -ge 273 ] && [ "$post_temp_raw" -ge 273 ] ||
        die "temperature_missing_or_non_kelvin=cell:$cell_name"
    [ "$pre_temp_raw" -lt "$warning_temp_kelvin" ] &&
        [ "$post_temp_raw" -lt "$warning_temp_kelvin" ] ||
        die "timed_temperature_at_or_above_warning=cell:$cell_name,threshold:$warning_temp_kelvin,pre:$pre_temp_raw,post:$post_temp_raw"
    pre_temp=$((pre_temp_raw - 273))
    post_temp=$((post_temp_raw - 273))
    [ "$pre_temp" -le 100 ] && [ "$post_temp" -le 100 ] ||
        die "temperature_out_of_range=cell:$cell_name"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$block" "$position" "$config" "$jobs" "$arm" "$iops" "$pair_us" \
        "$write_ios" "$sync_ios" "$write_lat_us" "$sync_lat_us" \
        "${stats[0]}" "${stats[1]}" "${stats[2]}" "${stats[3]}" \
        "${stats[4]}" "$hook_entries" "$hook_claimed" "$pre_temp" "$post_temp" \
        >>"$result_dir/results.tsv"
    printf 'CELL_OK block=%s position=%s config=%s jobs=%s arm=%s iops=%s pair_us=%s\n' \
        "$block" "$position" "$config" "$jobs" "$arm" "$iops" "$pair_us"

    if [ "$block" -eq 7 ] && [ "$position" -eq 3 ] &&
       [ "$jobs" -eq 8 ] && [ "$arm" = C ]; then
        last_work=$cell_work
    else
        [ "$(dirname -- "$cell_work")" = "$work_root" ] || die cleanup_scope
        rm -rf -- "$cell_work"
    fi
}

blocks=(
    '4B 4C 8B 8C'
    '8C 8B 4C 4B'
    '4C 4B 8C 8B'
    '8B 8C 4B 4C'
    '8B 8C 4B 4C'
    '4C 4B 8C 8B'
    '8C 8B 4C 4B'
    '4B 4C 8B 8C'
)
for block in "${!blocks[@]}"; do
    position=0
    for token in ${blocks[$block]}; do
        jobs=${token%?}
        arm=${token: -1}
        run_cell "$block" "$position" "$jobs" "$arm"
        position=$((position + 1))
    done
done

[ -n "$last_work" ] && [ -d "$last_work" ] || die final_readback_target_missing
mapfile -d '' final_targets < <(
    find "$last_work" -maxdepth 1 -type f -name 'job.*.dat' -print0 | sort -z
)
[ "${#final_targets[@]}" -eq 8 ] || die final_readback_target_count
env "${clean_unsets[@]}" python3 - "$((file_mib * 1024 * 1024))" \
    "${final_targets[@]}" \
    >"$result_dir/final-readback.txt" <<'PY'
import pathlib, sys
file_bytes = int(sys.argv[1])
paths = [pathlib.Path(item) for item in sys.argv[2:]]
chunk_bytes = 4 * 1024 * 1024
expected = bytes([0x5a]) * chunk_bytes
total = 0
for path in paths:
    if path.stat().st_size != file_bytes:
        raise SystemExit(f"size mismatch: {path}")
    with path.open("rb", buffering=0) as source:
        remain = file_bytes
        while remain:
            chunk = source.read(min(chunk_bytes, remain))
            if not chunk or chunk != expected[:len(chunk)]:
                raise SystemExit(f"readback mismatch: {path} offset={file_bytes-remain}")
            remain -= len(chunk)
            total += len(chunk)
print(f"READBACK_OK files={len(paths)} bytes={total} expected_byte=0x5a bs=4096")
PY
snapshot_device "$result_dir/campaign-final"
verify_snapshot_against_entry "$result_dir/campaign-final"
[ "$(dirname -- "$last_work")" = "$work_root" ] || die final_cleanup_scope
rm -rf -- "$last_work"
rmdir -- "$work_root" || die work_root_not_empty
work_root=

"$validator" summarize --tsv "$result_dir/results.tsv" \
    --output "$result_dir/summary.json" >"$result_dir/summary-stdout.json"

printf 'MATRIX_WORK_OK\n'
if [ "$manifest_mode" = self ]; then
    (
        cd "$result_dir"
        find . -type f ! -name final-manifest.sha256 -print0 | sort -z |
            xargs -0 sha256sum >final-manifest.sha256
    )
fi
printf 'RUN_OK result=%s\n' "$result_dir"
