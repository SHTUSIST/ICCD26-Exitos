#!/bin/bash
# Finite fio comparison: plain allocation and Exitos. Planning is read-only;
# --run owns the writes.
set -Eeuo pipefail
umask 077

SELF_DIR=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(cd "$SELF_DIR/../.." && pwd -P)
SUMMARIZER=$SELF_DIR/summarize.py

mode=plan
mount_dir=
result_dir=
threads_csv=1,2,4,8
cpus_csv=
file_mib=64
bs=4k
repeats=2
frontend=bpftime
preload_so=$REPO/libexitos_preload.so
active_so=$REPO/libexitos_bpftime_active.so
transformer=${EXITOS_BPFTIME_SO:-}
fio_bin=${FIO_BIN:-fio}
device=
expect_identity=
manifest_mode=self
only=all
worker_mode=thread
work_root=

die() { printf 'FIO_BPFTIME_E2E_REFUSE %s\n' "$*" >&2; exit 3; }
is_uint() { [[ $1 =~ ^[1-9][0-9]*$ ]]; }
require_abs() { [[ $2 == /* && $2 != *$'\n'* ]] || die "$1_not_absolute=$2"; }

usage()
{
    cat <<'EOF'
Usage: example/fio-bpftime-e2e/run.sh [--plan | --run] OPTIONS

Required: --mount ABS --result-dir ABS --cpus CSV
Defaults: --frontend bpftime --threads 1,2,4,8 --repeats 2 --bs 4k --file-mib 64
          --worker-mode thread

Frontend controls:
  --frontend NAME   bpftime (default) or preload (explicit comparison control)
  --active-so ABS   active Exitos loader DSO
  --transformer ABS patched bpftime transformer DSO
  --preload-so ABS  symbol-interposition DSO

Workload controls:
  --threads CSV     subset of 1,2,4,8; each value is one fio worker set, and
                    therefore one queue depth: the engine is synchronous with
                    application iodepth 1, so N workers is N writes in flight
  --worker-mode M   thread (default) or process. thread runs one fio process
                    with N job threads; process runs N independent fio
                    processes, one job each, so the workers share no address
                    space and no Exitos process state.
  --only NAME       all, plain, or exitos
  --fio PATH        fio 3.41 executable
  --device ABS      mounted partition used by raw-write authorization
  --expect-identity ID  namespace identity paired with --device
  --manifest-mode   self or outer (outer transaction owns the final manifest)
  --plan            inspect without creating files (default)
  --run             create fresh files and run the finite matrix

The common fio command deliberately omits allocation overrides. With fio 3.41
this preserves native extent allocation with overwrite=0 and create_on_open=0.
The plain arm has no Exitos environment. The default matrix also requires the
active bpftime arm; setup failure never falls back.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --plan) mode=plan; shift ;;
        --run) mode=run; shift ;;
        --mount|--result-dir|--threads|--cpus|--file-mib|--bs|--repeats|--frontend|--preload-so|--active-so|--transformer|--fio|--device|--expect-identity|--manifest-mode|--only|--worker-mode)
            [ "$#" -ge 2 ] || die "missing_value=$1"
            key=${1#--}; value=$2
            case "$key" in
                mount) mount_dir=$value ;; result-dir) result_dir=$value ;;
                threads) threads_csv=$value ;; cpus) cpus_csv=$value ;;
                file-mib) file_mib=$value ;; bs) bs=$value ;;
                repeats) repeats=$value ;; frontend) frontend=$value ;;
                preload-so) preload_so=$value ;; active-so) active_so=$value ;;
                transformer) transformer=$value ;; fio) fio_bin=$value ;;
                device) device=$value ;; expect-identity) expect_identity=$value ;;
                manifest-mode) manifest_mode=$value ;; only) only=$value ;;
                worker-mode) worker_mode=$value ;;
            esac
            shift 2
            ;;
        *) die "unknown_option=$1" ;;
    esac
done

case "$worker_mode" in
    thread|process) ;;
    *) die "worker_mode_invalid=$worker_mode" ;;
esac
[ -n "$mount_dir" ] || die mount_missing
[ -n "$result_dir" ] || die result_dir_missing
[ -n "$cpus_csv" ] || die cpus_missing
require_abs mount "$mount_dir"
require_abs result_dir "$result_dir"
[ -d "$mount_dir" ] || die "mount_not_directory=$mount_dir"
mount_dir=$(readlink -f -- "$mount_dir") || die mount_canonicalize_failed
mountpoint -q -- "$mount_dir" || die "not_a_mountpoint=$mount_dir"

canonical_result=$(readlink -m -- "$result_dir") || die result_dir_canonicalize_failed
[ "$canonical_result" = "$result_dir" ] || die "result_dir_not_canonical=$result_dir"
result_parent=$(dirname -- "$result_dir")
[ -d "$result_parent" ] || die result_parent_missing
[ ! -e "$result_dir" ] || die result_dir_exists
case "$result_dir/" in "$mount_dir/"*) die result_inside_mount ;; esac

is_uint "$file_mib" || die file_mib_invalid
is_uint "$repeats" || die repeats_invalid
[[ $bs =~ ^[1-9][0-9]*([kKmM])?$ ]] || die bs_invalid
case "$frontend" in bpftime|preload) ;; *) die frontend_invalid ;; esac
case "$manifest_mode" in self|outer) ;; *) die manifest_mode_invalid ;; esac
case "$only" in all|plain|exitos) ;; *) die only_invalid ;; esac
needs_exitos=1
[ "$only" = plain ] && needs_exitos=0
if [ -n "$device" ] || [ -n "$expect_identity" ]; then
    [ -n "$device" ] && [ -n "$expect_identity" ] || die device_identity_pair_required
    require_abs device "$device"
fi
# The Exitos arm cannot register a file without EXITOS_DEV, and --device is the
# only channel that reaches it: the launch below strips every inherited EXITOS_*
# name, so an exported EXITOS_DEV never arrives. raw_write_allowed() in
# src/intercept.c waves through only a loop device; on anything else an unset
# EXITOS_DEV fails registration with -EPERM, no file is ever taken over, and the
# run dies much later at the marker count with a message that names the wrong
# thing. Refuse here instead, where the message can name the missing flag.
if [ "$needs_exitos" -eq 1 ] && [ -z "$device" ]; then
    die device_required_for_exitos_arm
fi
# Process mode publishes its counters from an _exit interposer, which lives only
# in src/preload.c. libexitos_bpftime_active.so is built from the common sources
# plus the two bpftime files and does NOT carry that interposer, so a bpftime
# process worker -- fio ends a --thread=0 job with _exit(), which skips DSO
# destructors -- would write no counter file at all and every cell would be
# refused for a takeover that did happen. Refuse the combination instead.
if [ "$worker_mode" = process ] && [ "$frontend" = bpftime ] && [ "$needs_exitos" -eq 1 ]; then
    die process_mode_requires_preload_frontend
fi

IFS=, read -r -a thread_values <<<"$threads_csv"
[ "${#thread_values[@]}" -gt 0 ] || die threads_empty
declare -A seen_threads=()
max_threads=0
for jobs in "${thread_values[@]}"; do
    case "$jobs" in 1|2|4|8) ;; *) die "threads_invalid=$threads_csv" ;; esac
    [ -z "${seen_threads[$jobs]+x}" ] || die threads_duplicate
    seen_threads[$jobs]=1
    [ "$jobs" -gt "$max_threads" ] && max_threads=$jobs
done
[[ $cpus_csv =~ ^[0-9]+(,[0-9]+)*$ ]] || die cpus_invalid
IFS=, read -r -a cpu_values <<<"$cpus_csv"
declare -A seen_cpus=()
for cpu in "${cpu_values[@]}"; do
    [ -z "${seen_cpus[$cpu]+x}" ] || die cpus_duplicate
    seen_cpus[$cpu]=1
done
[ "${#cpu_values[@]}" -ge "$max_threads" ] || die cpus_insufficient

case "${bs,,}" in
    *k) bs_bytes=$((${bs%?} * 1024)) ;;
    *m) bs_bytes=$((${bs%?} * 1024 * 1024)) ;;
    *) bs_bytes=$bs ;;
esac
file_bytes=$((file_mib * 1024 * 1024))
[ "$file_bytes" -gt 0 ] && [ "$bs_bytes" -gt 0 ] || die geometry_overflow
[ $((file_bytes % bs_bytes)) -eq 0 ] || die geometry_not_aligned
ios_per_job=$((file_bytes / bs_bytes))
[ "$ios_per_job" -gt 1 ] || die geometry_needs_two_ios

if [[ $fio_bin == */* ]]; then
    require_abs fio "$fio_bin"
    fio_resolved=$(readlink -f -- "$fio_bin") || die fio_not_found
else
    fio_resolved=$(command -v -- "$fio_bin") || die fio_not_found
fi
[ -x "$fio_resolved" ] || die fio_not_executable
[ -r "$SUMMARIZER" ] || die summarizer_unreadable

mount_record=$(findmnt -rn -T "$mount_dir" -o SOURCE,FSTYPE 2>/dev/null) || die mount_source_unresolved
read -r mount_source mount_fstype mount_extra <<<"$mount_record"
[ -n "$mount_source" ] && [ "$mount_fstype" = ext4 ] && [ -z "${mount_extra:-}" ] || die mount_not_ext4
if [[ $mount_source == /dev/* ]] && [ -b "$mount_source" ] && [ -n "$device" ]; then
    [ "$(readlink -f -- "$device")" = "$(readlink -f -- "$mount_source")" ] || die device_not_mounted_partition
fi

# The shortcut submits an NVMe write command on an io_uring ring set up with
# IORING_SETUP_IOPOLL: the filesystem and the ordinary block layer are both out
# of the path and completion is polled rather than waited on. This needs the
# nvme driver to have poll queues, which the device transaction sets up.
#
# At 4 KiB the scatter-gather question does not arise: one block fits a single
# physically contiguous transfer, so no multi-segment descriptor list is built
# and nothing has to be staged into a contiguous bounce buffer first.
exitos_iopath=uringpoll

# Poll queues are settled when the nvme driver probes, so a run cannot switch
# them on for itself. Without them iopath_open_preferred() refuses the open and
# registration fails with -EACCES -- a message that names neither the driver nor
# the parameter. Check here so the refusal says what is actually wrong.
# Gated on --device naming a real block node: that is the only case where the
# driver setting can be read and is the only case where it matters. A fixture
# that points --device at an ordinary path is exercising the shell, not a driver.
if [ "$needs_exitos" -eq 1 ] && [ -b "$device" ]; then
    poll_param=/sys/module/nvme/parameters/poll_queues
    [ -r "$poll_param" ] || die poll_queues_unreadable
    poll_queues=$(cat "$poll_param") || die poll_queues_unreadable
    [ "$poll_queues" -gt 0 ] 2>/dev/null || die "poll_queues_disabled=$poll_queues"
fi

unset_list=()
while IFS= read -r variable; do
    case "$variable" in
        EXITOS_*|LD_PRELOAD|LD_AUDIT) unset_list+=(-u "$variable") ;;
    esac
done < <(compgen -e)

if [ "$worker_mode" = thread ]; then
    normalized_fio=(
        --name=fio-bpftime-e2e
        '--filename_format=<cell-work>/job.$jobnum.dat'
        --rw=write "--bs=$bs" "--size=${file_mib}m" --direct=1
        --ioengine=psync --iodepth=1 --fdatasync=1
        '--numjobs=<workers>' --thread=1 --output-format=json
        --buffer_pattern=0x5a --scramble_buffers=0
        '--cpus_allowed=<first-N-cpus>' --cpus_allowed_policy=split
        '--output=<cell-result>/fio.json'
    )
else
    normalized_fio=(
        --name=fio-bpftime-e2e
        '--filename=<cell-work>/job.<w>.dat'
        --rw=write "--bs=$bs" "--size=${file_mib}m" --direct=1
        --ioengine=psync --iodepth=1 --fdatasync=1
        --numjobs=1 --thread=0 --output-format=json
        --buffer_pattern=0x5a --scramble_buffers=0
        '--cpus_allowed=<cpu-of-worker-w>' --cpus_allowed_policy=split
        '--output=<cell-result>/fio.<w>.json'
    )
fi

if [ "$mode" = plan ]; then
    printf 'PLAN_OK mode=plan frontend=%s mount=%s mount_source=%s result_dir=%s threads=%s worker_mode=%s cpus=%s file_mib=%s bs=%s repeats=%s only=%s manifest_mode=%s\n' \
        "$frontend" "$mount_dir" "$mount_source" "$result_dir" "$threads_csv" "$worker_mode" "$cpus_csv" "$file_mib" "$bs" "$repeats" "$only" "$manifest_mode"
    echo 'PLAN_ARMS A=plain-fio B=exitos'
    if [ "$worker_mode" = thread ]; then
        echo 'PLAN_SEMANTICS process=one fio_threads=workers application_iodepth=1 allocation_overrides=omitted'
    else
        echo 'PLAN_SEMANTICS processes=workers fio_threads=one-per-process application_iodepth=1 allocation_overrides=omitted'
    fi
    echo 'PLAN_TAKEOVER B=EXITOS_FASTPATH=1 timed_writes_and_syncs=shortcut'
    echo 'PLAN_DEFAULTS fio=3.41 fallocate=native overwrite=0 create_on_open=0'
    echo 'PLAN_MODE B=exitos configured=true'
    printf 'PLAN_FIO %s' "$fio_resolved"; printf ' %q' "${normalized_fio[@]}"; printf '\n'
    echo 'PLAN_EXECUTE rerun_with=--run default_frontend=bpftime'
    exit 0
fi

if [ "$needs_exitos" -eq 1 ] && [ "$frontend" = bpftime ]; then
    require_abs active_so "$active_so"
    require_abs transformer "$transformer"
    [ -r "$active_so" ] || die active_so_unreadable
    [ -r "$transformer" ] || die transformer_unreadable
elif [ "$needs_exitos" -eq 1 ]; then
    require_abs preload_so "$preload_so"
    [ -r "$preload_so" ] || die preload_so_unreadable
fi

fio_version=$(env "${unset_list[@]}" "$fio_resolved" --version) || die fio_version_probe_failed
fio_version=${fio_version//$'\r'/}
[ "$fio_version" = fio-3.41 ] || die "fio_version_not_exact=$fio_version"
check_default()
{
    local option=$1 expected=$2 help
    help=$(env "${unset_list[@]}" "$fio_resolved" "--cmdhelp=$option") || die "fio_cmdhelp_failed=$option"
    printf '%s\n' "$help" | grep -Eq "^[[:space:]]*default:[[:space:]]*$expected[[:space:]]*$" || die "fio_default_mismatch=$option"
    printf '%s\n' "$help"
}
fallocate_help=$(check_default fallocate native)
overwrite_help=$(check_default overwrite 0)
create_help=$(check_default create_on_open 0)

old_umask=$(umask); umask 077
mkdir -m 0700 -- "$result_dir"
mkdir -m 0700 -- "$result_dir/cells"
work_root=$(mktemp -d -p "$mount_dir" .exitos-fio-bpftime.XXXXXXXX) || die work_root_create_failed
umask "$old_umask"
cleanup()
{
    if [ -n "$work_root" ] && [ -d "$work_root" ]; then
        rm -rf -- "$work_root"
    fi
}
trap cleanup EXIT

printf 'fio_version=%s\nfallocate_expected=native\n%s\noverwrite_expected=0\n%s\ncreate_on_open_expected=0\n%s\n' \
    "$fio_version" "$fallocate_help" "$overwrite_help" "$create_help" >"$result_dir/fio-defaults.txt"

python3 - "$result_dir/run.json" "$mount_dir" "$mount_source" "$work_root" "$threads_csv" "$cpus_csv" "$file_mib" "$file_bytes" "$bs" "$bs_bytes" "$repeats" "$frontend" "$exitos_iopath" "$fio_resolved" "$fio_version" "$device" "$expect_identity" "$manifest_mode" "$only" <<'PY'
import json, pathlib, sys
(path, mount, source, work, threads, cpus, mib, size, bs, bs_bytes,
 repeats, frontend, iopath, fio, version, device, identity, manifest, only) = sys.argv[1:]
payload = {
    "schema": "exitos-fio-bpftime-e2e-run-v1",
    "semantics": "one fio process with synchronous worker threads, application iodepth=1",
    "arms": {"A": "plain-fio", "B": "exitos"},
    "orders": [["A", "B"], ["B", "A"]],
    "mount": mount, "mount_source": source, "work_root": work,
    "result_dir": str(pathlib.Path(path).parent),
    "threads": [int(v) for v in threads.split(",")],
    "cpus": [int(v) for v in cpus.split(",")], "file_mib": int(mib),
    "file_bytes": int(size), "bs": bs, "bs_bytes": int(bs_bytes),
    "repeats": int(repeats), "frontend": frontend, "iopath": iopath,
    "fio": fio, "fio_version": version, "device": device or None,
    "expect_identity": identity or None, "manifest_mode": manifest, "only": only,
    "fio_defaults": {"fallocate": "native", "overwrite": "0", "create_on_open": "0"},
}
pathlib.Path(path).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

case "$only" in
    all) selected=(A B) ;;
    plain) selected=(A) ;;
    exitos) selected=(B) ;;
esac

built_arm_env=()
# One worker's Exitos environment. Thread mode calls this once for the whole
# fio process with the full pool width; process mode calls it once per worker
# with a private donor directory and a width of one, because the donor pool is
# created lazily on the first intercepted fallocate and its files are opened
# O_EXCL -- N processes sharing one directory would collide on donor-0000.dat
# and every worker but the first would be refused.
build_arm_env()
{
    local cell=$1 selector=$2 dir=$3 width=$4
    # The timed data and sync calls go through the selected backend.
    # EXITOS_FASTPATH=0 would keep the donor-fallocate preparation and then send
    # every timed call back through ext4, which measures the preparation alone
    # and never exercises the shortcut. This arm is the shortcut, so it is 1.
    built_arm_env=(
        "EXITOS_FILES=$selector" EXITOS_PREPARE=donor-fallocate
        "EXITOS_DONOR_DIR=$dir" "EXITOS_DONOR_FILES=$width"
        EXITOS_FASTPATH=1 "EXITOS_IOPATH=$exitos_iopath"
        "EXITOS_STATS=$cell/stats-%p.txt" EXITOS_VERBOSE=1
    )
    # fio runs a --thread=0 job in a forked child that ends with _exit(), which
    # skips DSO destructors -- and the destructor is the only place the counters
    # are written. src/preload.c interposes _exit for exactly this case and arms
    # it on this variable. Without it a process-mode cell publishes only the
    # parent's five zeros and the validator refuses a takeover that did happen.
    if [ "$worker_mode" = process ]; then
        built_arm_env+=(EXITOS_STATS_ON_WORKER_EXIT=1)
    fi
    if [ -n "$device" ]; then
        built_arm_env+=("EXITOS_DEV=$device" "EXITOS_EXPECT_SERIAL=$expect_identity")
    fi
    if [ "$frontend" = bpftime ]; then
        built_arm_env+=("LD_PRELOAD=$active_so" "EXITOS_BPFTIME_SO=$transformer")
    else
        built_arm_env+=("LD_PRELOAD=$preload_so")
    fi
}

run_cell()
{
    local arm=$1 jobs=$2 rep=$3 pos=$4 order=$5
    local arm_name cell cell_work donor_dir selector filename_format cell_cpus
    local fio_rc
    local -a common arm_env targets
    case "$arm" in
        A) arm_name=plain-fio ;;
        B) arm_name=exitos ;;
        *) die internal_arm_invalid ;;
    esac
    local mode_tag=T
    [ "$worker_mode" = process ] && mode_tag=P
    cell="${result_dir}/cells/t${jobs}${mode_tag}-r${rep}-o${pos}-${arm}"
    cell_work="$work_root/t${jobs}${mode_tag}-r${rep}-o${pos}-${arm}"
    donor_dir="$cell_work/donors"
    selector="$cell_work/job."
    filename_format="$cell_work/job.\$jobnum.dat"
    cell_cpus=${cpu_values[0]}
    local i=1
    while [ "$i" -lt "$jobs" ]; do
        cell_cpus=$cell_cpus,${cpu_values[$i]}
        i=$((i + 1))
    done
    mkdir -m 0700 -- "$cell" "$cell_work" "$donor_dir"
    if [ "$worker_mode" = thread ]; then
        common=(
            --name=fio-bpftime-e2e "--filename_format=$filename_format" --rw=write
            "--bs=$bs" "--size=${file_mib}m" --direct=1 --ioengine=psync
            --iodepth=1 --fdatasync=1 "--numjobs=$jobs" --thread=1
            --output-format=json --buffer_pattern=0x5a --scramble_buffers=0
            "--cpus_allowed=$cell_cpus" --cpus_allowed_policy=split
            "--output=$cell/fio.json" --eta=never
        )
    else
        # The recorded argv is one worker's. <w> is the worker index, which
        # varies per process; everything else is identical across the N.
        common=(
            --name=fio-bpftime-e2e "--filename=$cell_work/job.<w>.dat" --rw=write
            "--bs=$bs" "--size=${file_mib}m" --direct=1 --ioengine=psync
            --iodepth=1 --fdatasync=1 --numjobs=1 --thread=0
            --output-format=json --buffer_pattern=0x5a --scramble_buffers=0
            "--cpus_allowed=<cpu-of-worker-w>" --cpus_allowed_policy=split
            "--output=$cell/fio.<w>.json" --eta=never
        )
    fi
    python3 - "$cell/meta.json" "$arm" "$arm_name" "$frontend" "$exitos_iopath" "$jobs" "$rep" "$pos" "$order" "$file_mib" "$file_bytes" "$bs" "$bs_bytes" "$selector" "$device" "$expect_identity" "$worker_mode" "${common[@]}" <<'PY'
import json, pathlib, sys
(path, arm, arm_name, frontend, iopath, jobs, rep, pos, order, mib,
 size, bs, bs_bytes, selector, device, identity, worker_mode, *argv) = sys.argv[1:]
payload = {"schema": "exitos-fio-bpftime-e2e-cell-v1", "arm": arm,
    "arm_name": arm_name, "frontend": frontend, "iopath": iopath,
    "threads": int(jobs), "worker_mode": worker_mode,
    "rep": int(rep), "position": int(pos),
    "order": order.split(), "file_mib": int(mib), "file_bytes": int(size),
    "bs": bs, "bs_bytes": int(bs_bytes), "selector": selector,
    "device": device or None, "expect_identity": identity or None,
    "fio_argv": argv}
pathlib.Path(path).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
    printf '%q' "${common[0]}" >"$cell/command.txt"
    printf ' %q' "${common[@]:1}" >>"$cell/command.txt"
    printf '\n' >>"$cell/command.txt"

    arm_env=()
    if [ "$arm" != A ]; then
        arm_env=()
        build_arm_env "$cell" "$selector" "$donor_dir" "$jobs"
        arm_env=("${built_arm_env[@]}")
    fi
    if [ "$worker_mode" = thread ]; then
        set +e
        env "${unset_list[@]}" "${arm_env[@]}" "$fio_resolved" "${common[@]}" \
            >"$cell/stdout.txt" 2>"$cell/stderr.txt"
        fio_rc=$?
        set -e
        printf '%s\n' "$fio_rc" >"$cell/fio-exit-status.txt"
        [ "$fio_rc" -eq 0 ] || die "fio_cell_failed=$(basename -- "$cell"):rc:$fio_rc"
    else
        # Process mode: N independent fio processes, one job each, started
        # together and pinned one per CPU. Each writes its own file, so the
        # workers share no address space and no Exitos process state. A single
        # fio with --thread=0 cannot be used: fio forks after start, so every
        # child would inherit the same EXITOS_DONOR_DIR.
        local w=0 pid rc
        local -a pids=() worker_env=()
        for ((w = 0; w < jobs; w++)); do
            mkdir -m 0700 -- "$cell_work/donors-$w"
            worker_env=()
            if [ "$arm" != A ]; then
                build_arm_env "$cell" "$cell_work/job.$w." \
                    "$cell_work/donors-$w" 1
                worker_env=("${built_arm_env[@]}")
            fi
            env "${unset_list[@]}" "${worker_env[@]}" "$fio_resolved" \
                --name=fio-bpftime-e2e "--filename=$cell_work/job.$w.dat" \
                --rw=write "--bs=$bs" "--size=${file_mib}m" --direct=1 \
                --ioengine=psync --iodepth=1 --fdatasync=1 --numjobs=1 \
                --thread=0 --output-format=json --buffer_pattern=0x5a \
                --scramble_buffers=0 "--cpus_allowed=${cpu_values[$w]}" \
                --cpus_allowed_policy=split "--output=$cell/fio.$w.json" \
                --eta=never \
                >"$cell/stdout.$w.txt" 2>"$cell/stderr.$w.txt" &
            pids+=($!)
        done
        fio_rc=0
        for w in "${!pids[@]}"; do
            pid=${pids[$w]}
            set +e
            wait "$pid"
            rc=$?
            set -e
            printf '%s\n' "$rc" >"$cell/fio-exit-status.$w.txt"
            [ "$rc" -eq 0 ] || fio_rc=$rc
        done
        cat "$cell"/stdout.*.txt >"$cell/stdout.txt"
        cat "$cell"/stderr.*.txt >"$cell/stderr.txt"
        printf '%s\n' "$fio_rc" >"$cell/fio-exit-status.txt"
        [ "$fio_rc" -eq 0 ] || die "fio_cell_failed=$(basename -- "$cell"):rc:$fio_rc"
        # One jobs array, so the same validator reads both modes unchanged.
        python3 - "$cell/fio.json" "$cell" "$jobs" <<'PY_MERGE'
import json, pathlib, sys
out, cell, jobs = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
merged = []
for index in range(jobs):
    payload = json.loads((cell / f"fio.{index}.json").read_text(encoding="utf-8"))
    entries = payload.get("jobs")
    if not isinstance(entries, list) or len(entries) != 1:
        raise SystemExit(f"process worker {index} did not report exactly one job")
    merged.extend(entries)
out.write_text(json.dumps({"jobs": merged}, indent=2, sort_keys=True) + "\n",
               encoding="utf-8")
PY_MERGE
    fi

    if [ "$arm" = A ]; then
        printf 'PREPARE_NOT_APPLICABLE arm=A\n' >"$cell/prepare.txt"
        printf 'PREPARE_OUTCOME_NOT_APPLICABLE arm=A\n' >"$cell/prepare-outcome.txt"
    else
        grep -F 'PREPARE_READY' "$cell/stderr.txt" >"$cell/prepare.txt" || true
        grep -F 'PREPARE_OUTCOME ' "$cell/stderr.txt" >"$cell/prepare-outcome.txt" || true
    fi
    mapfile -d '' targets < <(find "$cell_work" -maxdepth 1 -type f -name 'job.*.dat' -print0 | sort -z)
    [ "${#targets[@]}" -eq "$jobs" ] || die "target_count_mismatch=$(basename -- "$cell")"
    printf '%s\n' "${targets[@]}" >"$cell/targets.txt"
    python3 "$SUMMARIZER" --check-pattern --expected-byte 0x5a --file-bytes "$file_bytes" "${targets[@]}" >"$cell/readback.txt"
    python3 "$SUMMARIZER" --check-cell "$cell" >"$cell/check.txt"
    rm -rf -- "$cell_work"
}

rep=1
while [ "$rep" -le "$repeats" ]; do
    if [ "$only" = all ]; then
        if [ $((rep % 2)) -eq 1 ]; then order=(A B); else order=(B A); fi
    else
        order=("${selected[@]}")
    fi
    pos=1
    for arm in "${order[@]}"; do
        for jobs in "${thread_values[@]}"; do
            run_cell "$arm" "$jobs" "$rep" "$pos" "${order[*]}"
        done
        pos=$((pos + 1))
    done
    rep=$((rep + 1))
done

python3 "$SUMMARIZER" --result-dir "$result_dir" >"$result_dir/summary.txt"
if [ "$manifest_mode" = self ]; then
    manifest=$result_dir/final-manifest.sha256
    (
        cd "$result_dir"
        find . -type f ! -path './final-manifest.sha256' -print0 |
            LC_ALL=C sort -z | xargs -0 -r sha256sum --
    ) >"$manifest"
    [ -s "$manifest" ] || die manifest_empty
    (cd "$result_dir" && sha256sum -c final-manifest.sha256 >/dev/null) || die manifest_check_failed
else
    echo 'MANIFEST_DEFERRED owner=outer'
fi
echo "RUN_OK result_dir=$result_dir arms=$only threads=$threads_csv repeats=$repeats frontend=$frontend manifest_mode=$manifest_mode"
