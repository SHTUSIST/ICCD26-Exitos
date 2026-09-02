#!/bin/bash
# Public one-thread real-QD comparison.  Device binding/restoration belongs to
# the outer device transaction; this runner owns only files and result evidence.
set -Eeuo pipefail
umask 077

self_dir=$(cd "$(dirname "$0")" && pwd -P)
mode=plan
mount_dir=
result_dir=
cpus_csv=
active_so=
transformer=
fio_bin=
device=
expect_identity=
manifest_mode=self
qd_list_text=1,2,4,8
whole=${EXITOS_TRANSACTION_WHOLE-}
generic=${EXITOS_TRANSACTION_GENERIC-}
test_mode=${EXITOS_QD_EXAMPLE_TEST_MODE:-0}

readonly bs_bytes=4096
readonly operations=2097152
readonly data_bytes=8589934592
readonly guard_bytes=4096
readonly file_bytes=8589942784
readonly cooldown_seconds=10
readonly reps=4

usage()
{
    cat <<'EOF'
Usage: tests/uring-passthrough-qd/run.sh [--plan | --run] OPTIONS

Required transaction-compatible options:
  --mount ABS          mounted ext4 test partition
  --result-dir ABS     new evidence directory outside the measured mount
  --cpus CSV           one or more CPU IDs; this one-thread example uses first
  --active-so ABS      recorded active-bpftime DSO provenance
  --transformer ABS    recorded bpftime transformer provenance
  --fio PATH           recorded transaction/fio provenance (not the workload)
  --device ABS         exact mounted partition passed to qd_bench
  --expect-identity ID exact namespace WWID
  --manifest-mode MODE self (default) or outer

Optional:
  --qd-list CSV        unique static subset of 1,2,4,8 (default: 1,2,4,8)

EXITOS_TRANSACTION_WHOLE and EXITOS_TRANSACTION_GENERIC must be exported by the
outer transaction.  Production always executes the qd_bench next to this
script.  There is no backend fallback and no adaptive QD selection.
EOF
}

die()
{
    printf 'URING_QD_EXAMPLE_REFUSE %s\n' "$*" >&2
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
        --qd-list) need_value "$@"; qd_list_text=$2; shift 2 ;;
        --threads|--bs|--operations|--repeats|--cooldown-seconds|--backend)
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
[ -n "$fio_bin" ] || die fio_missing
[ -n "$expect_identity" ] || die expect_identity_missing
[[ $expect_identity != *$'\n'* ]] || die identity_contains_newline
case "$manifest_mode" in self|outer) ;; *) die "manifest_mode_invalid=$manifest_mode" ;; esac
case "$test_mode" in 0|1) ;; *) die "test_mode_invalid=$test_mode" ;; esac
[[ $cpus_csv =~ ^[0-9]+(,[0-9]+)*$ ]] || die "cpus_invalid=$cpus_csv"
IFS=, read -r -a cpus <<<"$cpus_csv"
declare -A seen_cpu=()
for cpu_item in "${cpus[@]}"; do
    [ -z "${seen_cpu[$cpu_item]+x}" ] || die "cpu_duplicate=$cpu_item"
    seen_cpu[$cpu_item]=1
done
cpu=${cpus[0]}

[[ $qd_list_text =~ ^[0-9]+(,[0-9]+)*$ ]] || die "qd_list_invalid=$qd_list_text"
IFS=, read -r -a selected_qds <<<"$qd_list_text"
declare -A selected=()
for qd in "${selected_qds[@]}"; do
    case "$qd" in 1|2|4|8) ;; *) die "qd_not_static_supported=$qd" ;; esac
    [ -z "${selected[$qd]+x}" ] || die "qd_duplicate=$qd"
    selected[$qd]=1
done
[ "${#selected_qds[@]}" -gt 0 ] || die qd_list_empty

if [ "$mode" = plan ]; then
    cat <<EOF
PLAN_OK mode=plan threads=1 bs=$bs_bytes operations=$operations bytes=$data_bytes
PLAN_QD qd_list=$qd_list_text adaptive=forbidden
PLAN_MATRIX reps=4 qd_orders=1,2,4,8/8,4,2,1/2,1,8,4/4,8,1,2 arm_orders=ext4-passthrough/passthrough-ext4 arm_rep_map=ext4-passthrough,passthrough-ext4,passthrough-ext4,ext4-passthrough final_cell=passthrough cooldown_seconds=$cooldown_seconds
PLAN_GEOMETRY same_region=true same_pattern=0x5a same_commands=$operations same_batch_qd=true file_bytes=$file_bytes guards=4096+4096 prepare=actual-zero-write+sync untimed=true
PLAN_BACKENDS ext4=IORING_OP_WRITE+iopoll passthrough=IORING_OP_URING_CMD+iopoll fallback=forbidden
PLAN_CORRECTNESS readback=once-final-complete files=1 data=0x5a guards=0 checksum_per_cell=forbidden
PLAN_TARGET mount=$mount_dir partition=$device whole=$whole generic=$generic expect_identity=$expect_identity cpu=$cpu
PLAN_PROVENANCE active_so=$active_so transformer=$transformer fio=$fio_bin qd_bench=$self_dir/qd_bench qd_bench_freshness=make-q
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
for path in "$active_so" "$transformer"; do
    [ -r "$path" ] || die "input_unreadable=$path"
done
if [[ $fio_bin == */* ]]; then
    absolute fio "$fio_bin"
    fio_resolved=$(readlink -f -- "$fio_bin") || die "fio_not_found=$fio_bin"
else
    fio_resolved=$(command -v -- "$fio_bin") || die "fio_not_found=$fio_bin"
fi
[ -x "$fio_resolved" ] || die "fio_not_executable=$fio_resolved"

qd_bench=$self_dir/qd_bench
if [ "$test_mode" -eq 1 ] && [ -n "${EXITOS_QD_BENCH_BIN-}" ]; then
    qd_bench=$EXITOS_QD_BENCH_BIN
elif [ -n "${EXITOS_QD_BENCH_BIN-}" ]; then
    die qd_bench_override_test_only
fi
absolute qd_bench "$qd_bench"
[ -x "$qd_bench" ] || die "qd_bench_not_executable=$qd_bench"
if [ "$test_mode" -eq 0 ]; then
    repo_root=$(readlink -f -- "$self_dir/../..") || die repo_root_canonicalize_failed
    set +e
    make -q -C "$repo_root" tests/uring-passthrough-qd/qd_bench \
        >/dev/null 2>&1
    make_q_rc=$?
    set -e
    case "$make_q_rc" in
        0) ;;
        1) die qd_bench_stale_against_makefile_dependencies ;;
        *) die "qd_bench_make_q_failed=rc:$make_q_rc" ;;
    esac
fi

if [ "$test_mode" -eq 1 ]; then
    [ -f "$device" ] && [ -f "$whole" ] && [ -f "$generic" ] ||
        die test_fixture_device_missing
    device_real=$device
    whole_real=$whole
    generic_real=$generic
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
    generic_real=$(readlink -f -- "$generic") || die generic_canonicalize_failed
    mount_real=$(readlink -f -- "$mount_source") || die mount_source_canonicalize_failed
    [ "$device_real" = "$mount_real" ] || die device_not_mounted_partition
    whole_base=${whole_real##*/}
    [ -r "/sys/block/$whole_base/wwid" ] || die whole_wwid_missing
    [ "$(cat "/sys/block/$whole_base/wwid")" = "$expect_identity" ] ||
        die namespace_identity_mismatch
    [ "$(cat "/sys/block/$whole_base/queue/io_poll")" = 1 ] || die io_poll_disabled
fi

mkdir -- "$result_dir"
mkdir -- "$result_dir/cells"
work_root=$mount_dir/.exitos-uring-passthrough-qd-$$
[ ! -e "$work_root" ] || die "work_root_exists=$work_root"
mkdir -m 0700 -- "$work_root"
work_id=$(stat -Lc '%d:%i:%u:%a' "$work_root")
work_owned()
{
    [ -d "$work_root" ] && [ "$(dirname -- "$work_root")" = "$mount_dir" ] &&
        [ "$(stat -Lc '%d:%i:%u:%a' "$work_root" 2>/dev/null)" = "$work_id" ]
}
cleanup()
{
    local rc=$?
    trap - EXIT
    set +e
    if work_owned; then rm -rf -- "$work_root"; fi
    exit "$rc"
}
trap cleanup EXIT

benchmark_file=$work_root/benchmark.dat
ext4_file=$benchmark_file
passthrough_file=$benchmark_file
prepare_file()
{
    local target=$1
    if [ "$test_mode" -eq 1 ]; then
        # Device-free tests retain the exact sparse geometry; hardware runs use
        # the actual-write branch below and never inherit this override.
        truncate -s "$file_bytes" -- "$target"
    else
        dd if=/dev/zero of="$target" bs=8M count=1024 oflag=direct status=none
        dd if=/dev/zero of="$target" bs=4K count=2 seek=2097152 oflag=direct \
            conv=notrunc,fdatasync status=none
    fi
    [ "$(stat -Lc %s "$target")" -eq "$file_bytes" ] ||
        die "prepared_size_mismatch=$target"
}
prepare_file "$benchmark_file"
if [ "$test_mode" -eq 0 ]; then sync -f -- "$mount_dir"; fi
printf 'PREPARE_OK files=1 file_bytes=%s mode=%s actual_zero_write=%s synced=%s\n' \
    "$file_bytes" "$([ "$test_mode" -eq 1 ] && printf test-sparse || printf production)" \
    "$([ "$test_mode" -eq 1 ] && printf test-only-no || printf yes)" \
    "$([ "$test_mode" -eq 1 ] && printf test-only-no || printf yes)" \
    >"$result_dir/prepare.txt"

{
    printf 'threads=1\nbs=%s\noperations=%s\ndata_bytes=%s\n' \
        "$bs_bytes" "$operations" "$data_bytes"
    printf 'file_bytes=%s\nguard_bytes=%s\npattern=0x5a\n' \
        "$file_bytes" "$guard_bytes"
    printf 'qd_list=%s\nrepetitions=%s\ncooldown_seconds=%s\ncpu=%s\n' \
        "$qd_list_text" "$reps" "$cooldown_seconds" "$cpu"
    printf 'partition=%s\nwhole=%s\ngeneric=%s\nidentity=%s\n' \
        "$device_real" "$whole_real" "$generic_real" "$expect_identity"
    printf 'active_so=%s\ntransformer=%s\nfio=%s\nqd_bench=%s\n' \
        "$active_so" "$transformer" "$fio_resolved" "$qd_bench"
} >"$result_dir/config.txt"
printf 'rep\tposition\tqd\tarm\tiops\telapsed_ns\tcell\n' >"$result_dir/results.tsv"

run_program()
{
    if [ "$test_mode" -eq 1 ]; then
        "$@"
    else
        timeout --signal=TERM --kill-after=10 7200 taskset -c "$cpu" "$@"
    fi
}

run_cell()
{
    local rep=$1 position=$2 qd=$3 arm=$4
    local target cell rc iops elapsed
    case "$arm" in
        ext4) target=$ext4_file ;;
        passthrough) target=$passthrough_file ;;
        *) die "internal_arm_invalid=$arm" ;;
    esac
    cell=$result_dir/cells/rep${rep}-pos$(printf '%02d' "$position")-qd${qd}-${arm}
    mkdir -m 0700 -- "$cell"
    sleep "$cooldown_seconds"
    argv=("$qd_bench" --arm "$arm" --file "$target" \
        --partition "$device_real" --char-device "$generic_real" \
        --expect-identity "$expect_identity" \
        --qd "$qd" --operations "$operations" --output "$cell/result.json")
    {
        printf '%q' "${argv[0]}"
        printf ' %q' "${argv[@]:1}"
        printf '\n'
    } >"$cell/argv.txt"
    printf 'rep=%s\nposition=%s\nqd=%s\narm=%s\nthreads=1\nbs=4096\noperations=%s\n' \
        "$rep" "$position" "$qd" "$arm" "$operations" >"$cell/config.txt"
    set +e
    run_program env -u LD_PRELOAD -u LD_AUDIT "${argv[@]}" \
        >"$cell/stdout.txt" 2>"$cell/stderr.txt"
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$cell/rc.txt"
    [ "$rc" -eq 0 ] || die "qd_bench_failed=cell:${cell##*/},rc:$rc"
    if ! python3 "$self_dir/summarize.py" --validate-cell \
        "$cell/result.json" "$arm" "$qd" "$operations" "$target" \
        "$device_real" "$generic_real" "$expect_identity" \
        >"$cell/validation.txt" \
        2>"$cell/validation.err"; then
        cat "$cell/validation.err" >&2
        die "cell_json_invalid=${cell##*/}"
    fi
    read -r iops elapsed < <(python3 - "$cell/result.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="ascii"))
print(data["iops"], data["elapsed_ns"])
PY
    )
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$rep" "$position" "$qd" "$arm" "$iops" "$elapsed" "${cell##*/}" \
        >>"$result_dir/results.tsv"
    printf 'CELL_OK rep=%s position=%s qd=%s arm=%s iops=%s max_submitted_batch=%s\n' \
        "$rep" "$position" "$qd" "$arm" "$iops" "$qd"
}

qd_orders=('1 2 4 8' '8 4 2 1' '2 1 8 4' '4 8 1 2')
arm_orders=('ext4 passthrough' 'passthrough ext4')
arm_order_indexes=(0 1 1 0)
for rep in 0 1 2 3; do
    position=0
    for qd in ${qd_orders[$rep]}; do
        [ -n "${selected[$qd]+x}" ] || continue
        for arm in ${arm_orders[${arm_order_indexes[$rep]}]}; do
            run_cell "$rep" "$position" "$qd" "$arm"
            position=$((position + 1))
        done
    done
done

python3 "$self_dir/summarize.py" "$result_dir/results.tsv" \
    "$result_dir/summary.json" >"$result_dir/summary.txt" || die summary_invalid
cat "$result_dir/summary.txt"

final_readback()
{
    if [ -n "${EXITOS_QD_TEST_READBACK_LOG-}" ]; then
        printf 'readback-pass\n' >>"$EXITOS_QD_TEST_READBACK_LOG"
    fi
    python3 - "$test_mode" "$operations" "$benchmark_file" <<'PY'
import os
import sys

test_mode = int(sys.argv[1])
operations = int(sys.argv[2])
paths = sys.argv[3:]
bs = 4096
data_bytes = operations * bs
file_bytes = data_bytes + 2 * bs
zero = b"\0" * bs
pattern = b"Z" * bs

for path in paths:
    with open(path, "rb", buffering=0) as stream:
        if os.fstat(stream.fileno()).st_size != file_bytes:
            raise SystemExit(f"readback size mismatch: {path}")
        if stream.read(bs) != zero:
            raise SystemExit(f"front guard mismatch: {path}")
        if test_mode:
            if stream.read(bs) != pattern:
                raise SystemExit(f"first data block mismatch: {path}")
            stream.seek(bs + data_bytes - bs)
            if stream.read(bs) != pattern:
                raise SystemExit(f"last data block mismatch: {path}")
        else:
            remaining = data_bytes
            chunk_bytes = 4 * 1024 * 1024
            expected_chunk = b"Z" * chunk_bytes
            while remaining:
                want = min(remaining, chunk_bytes)
                actual = stream.read(want)
                if actual != expected_chunk[:want]:
                    raise SystemExit(f"data mismatch: {path}, remaining={remaining}")
                remaining -= want
        stream.seek(bs + data_bytes)
        if stream.read(bs) != zero:
            raise SystemExit(f"tail guard mismatch: {path}")
        if stream.read(1):
            raise SystemExit(f"unexpected bytes after tail guard: {path}")
print(
    f"FINAL_READBACK_OK files={len(paths)} "
    f"mode={'test-samples' if test_mode else 'complete'} "
    f"data_bytes_per_file={data_bytes} expected_data=0x5a guards=0"
)
PY
}

final_readback >"$result_dir/final-readback.txt" || die final_readback_failed
cat "$result_dir/final-readback.txt"

if [ "$manifest_mode" = self ]; then
    (
        cd "$result_dir"
        find . -type f ! -name MANIFEST.sha256 -print0 | sort -z |
            xargs -0 sha256sum >MANIFEST.sha256
    )
    [ -s "$result_dir/MANIFEST.sha256" ] || die final_manifest_empty
    printf 'MANIFEST_OK mode=self count=1 path=%s\n' "$result_dir/MANIFEST.sha256"
else
    [ ! -e "$result_dir/MANIFEST.sha256" ] || die outer_manifest_unexpected
    printf 'MANIFEST_DEFERRED owner=outer\n'
fi
printf 'CAMPAIGN_OK threads=1 bs=4096 operations=%s qd_list=%s cells=%s readback=once-final\n' \
    "$operations" "$qd_list_text" "$((reps * ${#selected_qds[@]} * 2))"
