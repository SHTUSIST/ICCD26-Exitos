#!/bin/bash
# Minimal 4 KiB/QD1 control: active bpftime, LD_PRELOAD, and native NVMe
# io_uring_cmd polling.  PCI binding and device restoration belong to the
# outer transaction that supplies EXITOS_TRANSACTION_WHOLE/GENERIC.
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
manifest_mode=self
whole=${EXITOS_TRANSACTION_WHOLE-}
generic=${EXITOS_TRANSACTION_GENERIC-}
test_mode=${EXITOS_FRONTEND_NATIVE_TEST_MODE:-0}

readonly file_mib=512
readonly file_bytes=536870912
readonly raw_bytes=4294967296
readonly bs_bytes=4096
readonly ios_per_job=$((file_bytes / bs_bytes))
readonly cooldown_seconds=10

usage()
{
    cat <<'EOF'
Usage: run-4k-thread-frontend-native-control.sh [--plan | --run] OPTIONS

Required:
  --mount ABS          mounted ext4 test partition
  --result-dir ABS     new evidence directory outside the measured mount
  --cpus CSV           at least eight distinct CPU IDs
  --active-so ABS      active-bpftime frontend DSO
  --transformer ABS    patched bpftime text-transformer DSO
  --fio PATH           fio 3.41 executable
  --device ABS         exact mounted ext4 partition
  --expect-identity ID exact namespace WWID
  --manifest-mode MODE self or outer

The outer transaction must export the dynamically resolved namespace block and
generic character devices as EXITOS_TRANSACTION_WHOLE and
EXITOS_TRANSACTION_GENERIC.  The workload is immutable: 4 KiB, QD1 per
worker, one process (--thread=1), 4/8 workers, two reversed repetitions, and
active-bpftime/preload/native-io_uring_cmd arms.
EOF
}

die()
{
    printf 'FRONTEND_NATIVE_CONTROL_REFUSE %s\n' "$*" >&2
    exit 3
}

need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "missing_value=$1"; }
absolute() { [[ $2 == /* ]] || die "$1_not_absolute=$2"; }

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
        --manifest-mode) need_value "$@"; manifest_mode=$2; shift 2 ;;
        --threads|--bs|--iodepth|--backend|--repeats|--cooldown-seconds|--file-mib)
            die "immutable_workload_option=$1"
            ;;
        *) die "unknown_option=$1" ;;
    esac
done

for pair in "mount:$mount_dir" "result_dir:$result_dir" \
            "active_so:$active_so" "transformer:$transformer" \
            "device:$device" "whole:$whole" "generic:$generic"; do
    key=${pair%%:*}
    value=${pair#*:}
    [ -n "$value" ] || die "${key}_missing"
    absolute "$key" "$value"
done
[ -n "$expect_identity" ] || die expect_identity_missing
[[ $expect_identity != *$'\n'* ]] || die identity_contains_newline
[[ $cpus_csv =~ ^[0-9]+(,[0-9]+)*$ ]] || die "cpus_invalid=$cpus_csv"
IFS=, read -r -a cpus <<<"$cpus_csv"
[ "${#cpus[@]}" -ge 8 ] || die "cpus_need_eight=count:${#cpus[@]}"
declare -A cpu_seen=()
for cpu in "${cpus[@]:0:8}"; do
    [ -z "${cpu_seen[$cpu]+x}" ] || die "cpu_duplicate=$cpu"
    cpu_seen[$cpu]=1
done
case "$manifest_mode" in self|outer) ;; *) die "manifest_mode_invalid=$manifest_mode" ;; esac
case "$test_mode" in 0|1) ;; *) die "test_mode_invalid=$test_mode" ;; esac

cpu4=$(IFS=,; printf '%s' "${cpus[*]:0:4}")
cpu8=$(IFS=,; printf '%s' "${cpus[*]:0:8}")
preload_so=$(dirname -- "$active_so")/libexitos_preload.so

if [ "$mode" = plan ]; then
    cat <<EOF
PLAN_OK mode=plan configs=threads4:4,threads8:8 bs=4k per_worker_qd=1
PLAN_PROCESS process_model=single-process thread=1
PLAN_ARMS arms=active-bpftime,preload,native-io_uring_cmd
PLAN_ORDER reps=2 rep0=active-bpftime-preload-native rep1=native-preload-active-bpftime cooldown_seconds=$cooldown_seconds
PLAN_FILES files=8 file_mib=$file_mib initialize=0x5a preparation=untimed-once
PLAN_FRONTENDS frontends=active-bpftime+preload fastpath=1 iopath=uringpoll fallback=forbidden exact_takeover=required
PLAN_NATIVE ioengine=io_uring_cmd cmd_type=nvme hipri=1 qd=1 raw_window=partition-end+0 bytes=$raw_bytes whole=$whole generic=$generic
PLAN_CPU cpus4=$cpu4 cpus8=$cpu8
PLAN_CORRECTNESS readback=once-final ext4_bytes=$raw_bytes raw_bytes=$raw_bytes expected_byte=0x5a checksum_per_cell=forbidden
PLAN_TARGET mount=$mount_dir partition=$device expect_identity=$expect_identity
PLAN_ARTIFACT result_dir=$result_dir manifest_mode=$manifest_mode inner_manifest=$([ "$manifest_mode" = outer ] && printf absent || printf final-only)
EOF
    exit 0
fi

[ "$EUID" -eq 0 ] || die run_requires_root
[ -d "$mount_dir" ] || die "mount_not_directory=$mount_dir"
mount_dir=$(readlink -f -- "$mount_dir") || die mount_canonicalize_failed
[ "$mount_dir" != / ] || die system_root_mount_forbidden
[ -d "$(dirname -- "$result_dir")" ] || die "result_parent_missing=$result_dir"
[ ! -e "$result_dir" ] || die "result_exists=$result_dir"
case "$result_dir" in "$mount_dir"|"$mount_dir"/*) die result_inside_measured_mount ;; esac

for path in "$active_so" "$preload_so" "$transformer"; do
    [ -r "$path" ] || die "input_unreadable=$path"
done
if [[ $fio_bin == */* ]]; then
    absolute fio "$fio_bin"
    fio_resolved=$(readlink -f -- "$fio_bin") || die "fio_not_found=$fio_bin"
else
    fio_resolved=$(command -v -- "$fio_bin") || die "fio_not_found=$fio_bin"
fi
[ -x "$fio_resolved" ] || die "fio_not_executable=$fio_resolved"

if [ "$test_mode" -eq 1 ]; then
    [ -f "$device" ] && [ -f "$whole" ] && [ -f "$generic" ] ||
        die test_fixture_device_missing
    [[ ${EXITOS_TEST_PART_START_SECTORS-} =~ ^[0-9]+$ ]] ||
        die test_part_start_missing
    [[ ${EXITOS_TEST_PART_SIZE_SECTORS-} =~ ^[1-9][0-9]*$ ]] ||
        die test_part_size_missing
    [[ ${EXITOS_TEST_WHOLE_BYTES-} =~ ^[1-9][0-9]*$ ]] ||
        die test_whole_bytes_missing
    part_start_sectors=$EXITOS_TEST_PART_START_SECTORS
    part_size_sectors=$EXITOS_TEST_PART_SIZE_SECTORS
    whole_size_bytes=$EXITOS_TEST_WHOLE_BYTES
    device_real=$device
else
    mountpoint -q -- "$mount_dir" || die "not_mounted=$mount_dir"
    mount_record=$(findmnt -rn -T "$mount_dir" -o SOURCE,FSTYPE) ||
        die mount_source_unresolved
    read -r mount_source mount_fstype mount_extra <<<"$mount_record"
    [ "$mount_fstype" = ext4 ] && [ -z "${mount_extra-}" ] ||
        die "mount_not_exact_ext4=$mount_record"
    [ -b "$device" ] || die "device_not_block=$device"
    [ -b "$whole" ] || die "whole_not_block=$whole"
    [ -c "$generic" ] || die "generic_not_char=$generic"
    device_real=$(readlink -f -- "$device") || die device_canonicalize_failed
    whole_real=$(readlink -f -- "$whole") || die whole_canonicalize_failed
    mount_real=$(readlink -f -- "$mount_source") || die mount_source_canonicalize_failed
    [ "$device_real" = "$mount_real" ] || die device_not_mounted_partition
    part_base=${device_real##*/}
    whole_base=${whole_real##*/}
    [ -r "/sys/class/block/$part_base/partition" ] || die device_not_partition
    [ "$(lsblk -ndo PKNAME "$device_real")" = "$whole_base" ] ||
        die partition_parent_not_transaction_whole
    [ "$(cat "/sys/block/$whole_base/wwid")" = "$expect_identity" ] ||
        die namespace_identity_mismatch
    [ "$(cat "/sys/block/$whole_base/queue/io_poll")" = 1 ] || die io_poll_disabled
    [ "$(cat "/sys/block/$whole_base/queue/write_cache")" = 'write through' ] ||
        die write_cache_not_through
    part_start_sectors=$(cat "/sys/class/block/$part_base/start")
    part_size_sectors=$(cat "/sys/class/block/$part_base/size")
    whole_size_bytes=$(blockdev --getsize64 "$whole_real")
fi

[[ $part_start_sectors =~ ^[0-9]+$ ]] || die partition_start_invalid
[[ $part_size_sectors =~ ^[1-9][0-9]*$ ]] || die partition_size_invalid
[[ $whole_size_bytes =~ ^[1-9][0-9]*$ ]] || die whole_size_invalid
native_offset=$(((part_start_sectors + part_size_sectors) * 512))
[ $((native_offset % bs_bytes)) -eq 0 ] || die native_offset_not_4k_aligned
[ $((native_offset + raw_bytes)) -le "$whole_size_bytes" ] ||
    die native_window_exceeds_namespace

clean_unsets=()
while IFS= read -r variable; do
    case "$variable" in
        EXITOS_*|LD_PRELOAD|LD_AUDIT) clean_unsets+=(-u "$variable") ;;
    esac
done < <(compgen -e)

[ "$(env "${clean_unsets[@]}" "$fio_resolved" --version)" = fio-3.41 ] ||
    die fio_version_not_3_41
env "${clean_unsets[@]}" "$fio_resolved" --enghelp=io_uring_cmd |
    grep -q '^hipri' || die fio_io_uring_cmd_hipri_missing

mkdir -- "$result_dir"
mkdir -- "$result_dir/cells"
work_root=$mount_dir/.exitos-frontend-native-control-$$
[ ! -e "$work_root" ] || die "work_root_exists=$work_root"
mkdir -m 0700 -- "$work_root"
work_root_id=$(stat -Lc '%d:%i:%u:%a' "$work_root")

work_root_owned()
{
    [ -d "$work_root" ] && [ "$(dirname -- "$work_root")" = "$mount_dir" ] &&
        [ "$(stat -Lc '%d:%i:%u:%a' "$work_root" 2>/dev/null)" = "$work_root_id" ]
}

cleanup()
{
    local rc=$?
    trap - EXIT
    set +e
    if work_root_owned; then rm -rf -- "$work_root"; fi
    exit "$rc"
}
trap cleanup EXIT

run_with_policy()
{
    local allowed=$1
    shift
    if [ "$test_mode" -eq 1 ]; then
        "$@"
    else
        timeout --signal=TERM --kill-after=10 3600 \
            chrt --other 0 nice -n -20 taskset -c "$allowed" "$@"
    fi
}

files_dir=$work_root/files
mkdir -m 0700 -- "$files_dir"
filename_format=$files_dir/w\$jobnum.dat
prepare_argv=("$fio_resolved" --name=frontend-native-prepare
    "--filename_format=$filename_format" --rw=write --bs=1m
    "--size=${file_mib}m" "--io_size=${file_mib}m" --direct=1
    --ioengine=psync --iodepth=1 --numjobs=8 --thread=1
    --create_serialize=1 --fallocate=none --overwrite=1
    --buffer_pattern=0x5a --scramble_buffers=0
    "--cpus_allowed=$cpu8" --cpus_allowed_policy=split --eta=never)
env "${clean_unsets[@]}" "${prepare_argv[@]}" \
    >"$result_dir/prepare.stdout" 2>"$result_dir/prepare.stderr" ||
    die ext4_preinitialize_failed
for job in $(seq 0 7); do
    target=$files_dir/w$job.dat
    [ -f "$target" ] || die "prepared_file_missing=$target"
    if [ "$test_mode" -eq 0 ]; then
        [ "$(stat -Lc %s "$target")" -eq "$file_bytes" ] ||
            die "prepared_file_size=$target"
    fi
done
if [ "$test_mode" -eq 0 ]; then sync -f -- "$mount_dir"; fi

{
    printf 'whole=%s\ngeneric=%s\npartition=%s\nidentity=%s\n' \
        "$whole" "$generic" "$device_real" "$expect_identity"
    printf 'partition_start_sectors=%s\npartition_size_sectors=%s\n' \
        "$part_start_sectors" "$part_size_sectors"
    printf 'native_offset=%s\nnative_bytes=%s\n' "$native_offset" "$raw_bytes"
    printf 'bs=4096\nfile_bytes=%s\nfiles=8\npattern=0x5a\n' "$file_bytes"
} >"$result_dir/config.txt"

printf 'rep\tconfig\tjobs\tthread\tarm\tiops\tpair_us\twrite_ios\tsync_ios\tfast_write\tfast_sync\tpass\tdeclined\trefresh\thook_entries\thook_claimed\n' \
    >"$result_dir/results.tsv"

run_cell()
{
    local rep=$1 config=$2 jobs=$3 order=$4 arm=$5 allowed cell rc
    local iops write_ios sync_ios pair_us expected_write expected_sync
    local stats_path hook_line hook_entries=0 hook_claimed=0
    local -a fio_argv launch_env stats_paths values sums=(0 0 0 0 0)

    case "$jobs" in
        4) allowed=$cpu4 ;;
        8) allowed=$cpu8 ;;
        *) die "internal_jobs_invalid=$jobs" ;;
    esac
    cell=$result_dir/cells/rep${rep}-${config}-ord${order}-${arm}
    mkdir -m 0700 -- "$cell"
    sleep "$cooldown_seconds"

    if [ "$arm" = native ]; then
        fio_argv=("$fio_resolved" --name=frontend-native-native
            "--filename=$generic" --ioengine=io_uring_cmd --cmd_type=nvme
            --hipri=1 --fixedbufs=0 --registerfiles=0 --writefua=0
            --rw=write --bs=4k "--size=${file_mib}m" "--io_size=${file_mib}m"
            "--offset=$native_offset" "--offset_increment=${file_mib}m"
            --iodepth=1 "--numjobs=$jobs" --thread=1
            --buffer_pattern=0x5a --scramble_buffers=0
            "--cpus_allowed=$allowed" --cpus_allowed_policy=split
            --output-format=json "--output=$cell/fio.json" --eta=never)
        launch_env=(env "${clean_unsets[@]}")
    else
        fio_argv=("$fio_resolved" --name=frontend-native-files
            "--filename_format=$filename_format" --rw=write --bs=4k
            "--size=${file_mib}m" "--io_size=${file_mib}m" --direct=1
            --ioengine=psync --iodepth=1 --fdatasync=1
            "--numjobs=$jobs" --thread=1 --create_serialize=1
            --fallocate=none --overwrite=1 --create_on_open=0
            --buffer_pattern=0x5a --scramble_buffers=0
            "--cpus_allowed=$allowed" --cpus_allowed_policy=split
            --output-format=json "--output=$cell/fio.json" --eta=never)
        launch_env=(env "${clean_unsets[@]}")
        if [ "$arm" = active-bpftime ]; then
            launch_env+=(LD_PRELOAD="$active_so" EXITOS_BPFTIME_SO="$transformer")
        elif [ "$arm" = preload ]; then
            launch_env+=(LD_PRELOAD="$preload_so")
        else
            die "internal_arm_invalid=$arm"
        fi
        launch_env+=(EXITOS_FILES="$files_dir/w" EXITOS_FASTPATH=1
            EXITOS_DEV="$device_real" EXITOS_EXPECT_SERIAL="$expect_identity"
            EXITOS_IOPATH=uringpoll EXITOS_STAGE=0 EXITOS_STRICT=0
            EXITOS_VERIFY_IDENTITY=0 EXITOS_VERBOSE=1
            EXITOS_STATS="$cell/stats-%p.txt" EXITOS_STATS_ON_WORKER_EXIT=1)
    fi

    {
        printf '%q' "${launch_env[0]}"
        printf ' %q' "${launch_env[@]:1}" "${fio_argv[@]}"
        printf '\n'
    } >"$cell/argv.txt"
    printf 'rep=%s\nconfig=%s\njobs=%s\nthread=1\narm=%s\ncooldown_seconds=%s\n' \
        "$rep" "$config" "$jobs" "$arm" "$cooldown_seconds" \
        >"$cell/config.txt"

    set +e
    run_with_policy "$allowed" "${launch_env[@]}" "${fio_argv[@]}" \
        >"$cell/stdout.txt" 2>"$cell/stderr.txt"
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$cell/rc.txt"
    [ "$rc" -eq 0 ] || die "fio_failed=cell:${cell##*/},rc:$rc"
    jq -e --argjson jobs "$jobs" \
        '(.jobs | length) == $jobs and all(.jobs[]; .error == 0)' \
        "$cell/fio.json" >/dev/null || die "fio_json_invalid=$cell"

    iops=$(jq -r '[.jobs[].write.iops] | add' "$cell/fio.json")
    write_ios=$(jq -r '[.jobs[].write.total_ios] | add' "$cell/fio.json")
    sync_ios=$(jq -r '[.jobs[] | (.sync.lat_ns.N // 0)] | add' "$cell/fio.json")
    expected_write=$((ios_per_job * jobs))
    if [ "$arm" = native ]; then expected_sync=0; else
        expected_sync=$(((ios_per_job - 1) * jobs))
    fi
    [ "$write_ios" -eq "$expected_write" ] ||
        die "write_count_mismatch=cell:${cell##*/},actual:$write_ios,expected:$expected_write"
    [ "$sync_ios" -eq "$expected_sync" ] ||
        die "sync_count_mismatch=cell:${cell##*/},actual:$sync_ios,expected:$expected_sync"
    pair_us=$(awk -v jobs="$jobs" -v iops="$iops" \
        'BEGIN { printf "%.9f", jobs * 1000000.0 / iops }')

    if [ "$arm" != native ]; then
        mapfile -t stats_paths < <(find "$cell" -maxdepth 1 -type f -name 'stats-*.txt' | sort)
        [ "${#stats_paths[@]}" -ge 1 ] || die "stats_missing=$cell"
        for stats_path in "${stats_paths[@]}"; do
            mapfile -t values <"$stats_path"
            [ "${#values[@]}" -eq 5 ] || die "stats_shape=$stats_path"
            for index in 0 1 2 3 4; do
                [[ ${values[$index]} =~ ^[0-9]+$ ]] || die "stats_value=$stats_path"
                sums[$index]=$((sums[$index] + values[$index]))
            done
        done
        [ "${sums[0]}" -eq "$write_ios" ] &&
            [ "${sums[1]}" -eq "$sync_ios" ] &&
            [ "${sums[2]}" -eq 0 ] && [ "${sums[3]}" -eq 0 ] &&
            [ "${sums[4]}" -eq 0 ] ||
            die "takeover_count_mismatch=cell:${cell##*/},fast_write:${sums[0]},write:$write_ios,fast_sync:${sums[1]},sync:$sync_ios,pass:${sums[2]},declined:${sums[3]},refresh:${sums[4]}"
        [ "$(grep -c -- '-> FAST PATH' "$cell/stderr.txt" || true)" -eq "$jobs" ] ||
            die "fast_path_registration_count=cell:${cell##*/}"
        if [ "$arm" = active-bpftime ]; then
            hook_line=$(grep -E 'hook entered [0-9]+ times, claimed [0-9]+' \
                "$cell/stderr.txt" | tail -1)
            hook_entries=$(sed -n 's/.*hook entered \([0-9][0-9]*\) times.*/\1/p' \
                <<<"$hook_line")
            hook_claimed=$(sed -n 's/.*claimed \([0-9][0-9]*\).*/\1/p' \
                <<<"$hook_line")
            [ "$hook_entries" -gt 0 ] && [ "$hook_claimed" -gt 0 ] ||
                die "active_hook_count_zero=$cell"
        fi
    fi

    printf '%s\t%s\t%s\t1\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$rep" "$config" "$jobs" "$arm" "$iops" "$pair_us" \
        "$write_ios" "$sync_ios" "${sums[0]}" "${sums[1]}" \
        "${sums[2]}" "${sums[3]}" "${sums[4]}" "$hook_entries" "$hook_claimed" \
        >>"$result_dir/results.tsv"
    printf 'CELL_OK rep=%s config=%s jobs=%s thread=1 arm=%s iops=%s pair_us=%s\n' \
        "$rep" "$config" "$jobs" "$arm" "$iops" "$pair_us"
}

orders=('active-bpftime preload native' 'native preload active-bpftime')
for rep in 0 1; do
    for jobs in 4 8; do
        config=threads$jobs
        order=0
        for arm in ${orders[$rep]}; do
            run_cell "$rep" "$config" "$jobs" "$order" "$arm"
            order=$((order + 1))
        done
    done
done

python3 - "$result_dir/results.tsv" "$result_dir/summary.json" \
    >"$result_dir/summary.txt" <<'PY'
import csv, json, statistics, sys

rows = list(csv.DictReader(open(sys.argv[1], encoding="ascii"), delimiter="\t"))
by = {}
for row in rows:
    by.setdefault((row["config"], row["arm"]), []).append(float(row["pair_us"]))
if len(rows) != 12 or any(len(values) != 2 for values in by.values()):
    raise SystemExit("incomplete matrix")
pair_us = {f"{config}:{arm}": values for (config, arm), values in by.items()}
medians = {key: statistics.median(values) for key, values in pair_us.items()}
summary = {"pair_us": pair_us, "median_pair_us": medians}
with open(sys.argv[2], "w", encoding="ascii") as stream:
    json.dump(summary, stream, indent=2, sort_keys=True)
for key in sorted(medians):
    print(f"{key} median_pair_us={medians[key]:.9f} samples={pair_us[key]}")
PY

mapfile -t final_files < <(find "$files_dir" -maxdepth 1 -type f -name 'w*.dat' | sort -V)
[ "${#final_files[@]}" -eq 8 ] || die final_ext4_file_count
if [ "$test_mode" -eq 1 ]; then
    python3 - "$native_offset" "$file_bytes" "$whole" "${final_files[@]}" \
        >"$result_dir/final-readback.txt" <<'PY'
import os, pathlib, sys

offset, increment = int(sys.argv[1]), int(sys.argv[2])
whole = sys.argv[3]
files = [pathlib.Path(item) for item in sys.argv[4:]]
expected = b"Z" * 4096
for path in files:
    if path.read_bytes()[:4096] != expected:
        raise SystemExit(f"ext4 test sample mismatch: {path}")
print(f"EXT4_READBACK_TEST_SAMPLE_OK files={len(files)} sample_bytes=4096 expected_byte=0x5a")
fd = os.open(whole, os.O_RDONLY)
try:
    for job in range(8):
        if os.pread(fd, 4096, offset + job * increment) != expected:
            raise SystemExit(f"raw test sample mismatch: job={job}")
finally:
    os.close(fd)
print("RAW_READBACK_TEST_SAMPLE_OK jobs=8 sample_bytes_per_job=4096 expected_byte=0x5a")
PY
else
    python3 - "$file_bytes" "$native_offset" "$raw_bytes" "$whole" \
        "${final_files[@]}" >"$result_dir/final-readback.txt" <<'PY'
import os, pathlib, sys

file_bytes, raw_offset, raw_bytes = map(int, sys.argv[1:4])
whole = sys.argv[4]
files = [pathlib.Path(item) for item in sys.argv[5:]]
chunk_bytes = 4 * 1024 * 1024
expected = bytes([0x5a]) * chunk_bytes

ext4_total = 0
for path in files:
    if path.stat().st_size != file_bytes:
        raise SystemExit(f"ext4 size mismatch: {path}")
    with path.open("rb", buffering=0) as source:
        remain = file_bytes
        while remain:
            data = source.read(min(remain, chunk_bytes))
            if not data or data != expected[:len(data)]:
                raise SystemExit(f"ext4 readback mismatch: {path} offset={file_bytes-remain}")
            remain -= len(data)
            ext4_total += len(data)
print(f"EXT4_READBACK_OK files={len(files)} bytes={ext4_total} expected_byte=0x5a")

fd = os.open(whole, os.O_RDONLY)
try:
    done = 0
    while done < raw_bytes:
        data = os.pread(fd, min(chunk_bytes, raw_bytes - done), raw_offset + done)
        if not data or data != expected[:len(data)]:
            raise SystemExit(f"raw readback mismatch: offset={raw_offset+done}")
        done += len(data)
finally:
    os.close(fd)
print(f"RAW_READBACK_OK offset={raw_offset} bytes={done} expected_byte=0x5a")
PY
fi

if work_root_owned; then rm -rf -- "$work_root"; else die cleanup_scope; fi
work_root=
if [ "$manifest_mode" = self ]; then
    (
        cd "$result_dir"
        find . -type f ! -name final-manifest.sha256 -print0 | sort -z |
            xargs -0 sha256sum >final-manifest.sha256
    )
else
    printf 'MANIFEST_DEFERRED owner=outer\n'
fi
printf 'RUN_OK result=%s cells=12 readback=complete\n' "$result_dir"
