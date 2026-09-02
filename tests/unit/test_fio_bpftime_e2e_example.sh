#!/bin/bash
# Device-free contract for the public fio -> active-bpftime example.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
EXAMPLE=$ROOT/example/fio-bpftime-e2e
RUNNER=$EXAMPLE/run.sh
TMP=$(mktemp -d /tmp/exitos-fio-bpftime-example.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT

n=0
fail=0
ok()  { n=$((n + 1)); printf 'ok %d - %s\n' "$n" "$*"; }
nok() { n=$((n + 1)); fail=$((fail + 1)); printf 'not ok %d - %s\n' "$n" "$*" >&2; }
has() {
    if grep -Fq -- "$2" "$1"; then ok "$3"; else nok "$3 (missing: $2)"; fi
}

if [ -x "$RUNNER" ] && bash -n "$RUNNER"; then
    ok "fio E2E runner exists, is executable, and parses"
else
    nok "fio E2E runner exists, is executable, and parses"
fi

if [ ! -x "$RUNNER" ]; then
    printf '1..%d  (%d failed)\n' "$n" "$fail"
    exit 1
fi

has "$RUNNER" 'frontend=bpftime' "default frontend is active bpftime"
has "$RUNNER" 'plain-fio' "plan includes the plain fio arm"
has "$RUNNER" 'arm_name=exitos' "plan includes the Exitos arm"
has "$RUNNER" '--ioengine=psync' "fio uses the synchronous engine"
has "$RUNNER" '--iodepth=1' "fio application depth is one"
has "$RUNNER" '--thread=1' "one process uses fio thread jobs"
has "$RUNNER" 'threads_csv=1,2,4,8' "default workers are 1, 2, 4 and 8"
has "$RUNNER" 'bs=4k' "the workload writes 4 KiB"
# The worker count is the queue depth here. The engine is synchronous at
# application iodepth 1, and interception covers write/pwrite/writev/fdatasync
# but not io_uring_enter, so an async engine would leave the Exitos arm with
# nothing to take over. N workers is the only way to get N writes in flight.
has "$RUNNER" 'exitos_iopath=uringpoll' "the Exitos arm submits on a polled ring"
has "$RUNNER" 'EXITOS_PREPARE=donor-fallocate' "Exitos arm carries its configured setup"
has "$RUNNER" 'EXITOS_BPFTIME_SO' "active bpftime receives the transformer"
has "$RUNNER" 'EXITOS_FASTPATH=1' "Exitos arm takes the shortcut for timed calls"
has "$RUNNER" 'worker-mode' "runner offers thread and process worker modes"
has "$RUNNER" 'donors-' "process mode gives each worker its own donor directory"
# Only code counts: the comment above the setting explains what 0 would mean.
if grep -Eq '^[^#]*EXITOS_FASTPATH=0' "$RUNNER"; then
    nok "no code path leaves the Exitos arm writing through ext4"
else
    ok "no code path leaves the Exitos arm writing through ext4"
fi

for forbidden in 'bpftime-prepared-ext4' 'bpftime-ext4-iouring-write' 'ring_backend' 'order=(A B C)' 'selected=(A B C)'; do
    if grep -Fq -- "$forbidden" "$RUNNER"; then
        nok "runner removes third-arm trace: $forbidden"
    else
        ok "runner removes third-arm trace: $forbidden"
    fi
done
if [ -e "$ROOT/example/ext4-iouring-write" ]; then
    nok "third-arm wrapper directory is removed"
else
    ok "third-arm wrapper directory is removed"
fi

for forbidden in '--fallocate=' '--overwrite=' '--create_on_open='; do
    if grep -Fq -- "$forbidden" "$RUNNER"; then
        nok "normalized fio argv omits $forbidden"
    else
        ok "normalized fio argv omits $forbidden"
    fi
done

mkdir -p "$TMP/bin" "$TMP/mount" "$TMP/results"
: >"$TMP/active.so"
: >"$TMP/transformer.so"
: >"$TMP/preload.so"
FAKE_DEV=$TMP/dev-fake
: >"$FAKE_DEV"
DEV_ARGS=(--device "$FAKE_DEV" --expect-identity fake-wwid)
cat >"$TMP/bin/mountpoint" <<'EOF'
#!/bin/sh
exit 0
EOF
cat >"$TMP/bin/findmnt" <<'EOF'
#!/bin/sh
printf '/dev/loop-fio ext4\n'
EOF
cat >"$TMP/bin/fio" <<'EOF'
#!/bin/bash
set -euo pipefail
log=${FAKE_FIO_LOG:?}
if [ "$1" = --version ]; then printf 'fio-3.41\n'; exit 0; fi
if [[ $1 == --cmdhelp=* ]]; then
    case ${1#--cmdhelp=} in
        fallocate) printf 'default: native\n' ;;
        overwrite|create_on_open) printf 'default: 0\n' ;;
    esac
    exit 0
fi
printf 'env=%s\n' "$(env | tr '\n' ';')" >>"$log"
printf 'argv=' >>"$log"
printf '%q ' "$@" >>"$log"
printf '\n' >>"$log"
output= filename= jobs=1 size=1m bs=8k
for arg in "$@"; do
    case "$arg" in
        --output=*) output=${arg#--output=} ;;
        --filename_format=*) filename=${arg#--filename_format=} ;;
        --filename=*) filename=${arg#--filename=} ;;
        --numjobs=*) jobs=${arg#--numjobs=} ;;
        --size=*) size=${arg#--size=} ;;
        --bs=*) bs=${arg#--bs=} ;;
    esac
done
bytes=$((1 * 1024 * 1024))
case "$size" in *m) bytes=$((${size%m} * 1024 * 1024));; esac
bs_bytes=8192
case "$bs" in *k) bs_bytes=$((${bs%k} * 1024));; esac
ios=$((bytes / bs_bytes))
mkdir -p "$(dirname "$output")"
python3 - "$filename" "$jobs" "$bytes" <<'PY'
import pathlib, sys
pattern = bytes([0x5a])
fmt, jobs, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
for job in range(1, jobs + 1):
    path = pathlib.Path(fmt.replace('$jobnum', str(job)))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as target:
        target.write(pattern * size)
PY
frontend=bpftime
if [ -n "${EXITOS_FILES-}" ]; then
    [ -z "${EXITOS_BPFTIME_SO-}" ] && frontend=ld_preload
    for ((i=0; i<jobs; i++)); do
        printf 'PREPARE_READY frontend=%s mode=0 off=0 len=%s prepared=%s chunks=1 fiemap=safe unsafe_flags=0\n' "$frontend" "$bytes" "$bytes" >&2
        printf 'PREPARE_OUTCOME v=1 frontend=%s fd=3 mode=0 off=0 len=%s outcome=prepared stage=complete rc=0 return_rc=0 return_errno=0 rdwr_alias=1 prepared=%s chunks=1\n' "$frontend" "$bytes" "$bytes" >&2
    done
    if [ -n "${EXITOS_STATS-}" ]; then
        stats=${EXITOS_STATS//%p/$$}
        sync_ios=$((ios - 1))
        total_ios=$((ios * jobs))
        total_sync=$((sync_ios * jobs))
        if [ "${EXITOS_FASTPATH-0}" = 1 ]; then
            printf '%s\n%s\n0\n0\n%s\n' "$total_ios" "$total_sync" "$jobs" >"$stats"
        else
            printf '0\n0\n0\n0\n%s\n' "$jobs" >"$stats"
        fi
    fi
fi
jobs_json=
for ((i=0; i<jobs; i++)); do
    [ -n "$jobs_json" ] && jobs_json+=,
    sync_ios=$((ios - 1))
    jobs_json+="{\"error\":0,\"write\":{\"total_ios\":$ios,\"iops\":1000,\"lat_ns\":{\"mean\":1000}},\"sync\":{\"lat_ns\":{\"N\":$sync_ios,\"mean\":1000}}}"
done
printf '{"jobs":[%s]}\n' "$jobs_json" >"$output"
EOF
chmod +x "$TMP/bin"/*

plan=$TMP/plan.txt
if PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
    --mount "$TMP/mount" --result-dir "$TMP/results/plan" --cpus 0,1,2,3 \
    "${DEV_ARGS[@]}" \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --threads 1,4 --repeats 1 >"$plan" 2>"$TMP/plan.err"; then
    ok "plan mode accepts the transaction-compatible interface"
else
    nok "plan mode accepts the transaction-compatible interface"
fi
has "$plan" 'frontend=bpftime' "plan keeps bpftime as the default"
has "$plan" 'plain-fio' "plan prints the plain arm"
has "$plan" 'exitos' "plan prints the Exitos arm"
if grep -Eq 'bpftime-ext4|prepared-ext4|arm_orders=.*C|PLAN_ARMS .*C' "$plan"; then
    nok "plan contains only A and B arms"
else
    ok "plan contains only A and B arms"
fi
if [ ! -e "$TMP/results/plan" ]; then ok "plan mode creates no result directory"; else nok "plan mode creates no result directory"; fi

log=$TMP/fio.log
result=$TMP/results/run
if PATH="$TMP/bin:$PATH" FAKE_FIO_LOG="$log" "$RUNNER" --run \
    --mount "$TMP/mount" --result-dir "$result" --cpus 0,1,2,3 \
    "${DEV_ARGS[@]}" \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --threads 1,4 --repeats 1 --file-mib 1 \
    --frontend bpftime >"$TMP/run.out" 2>"$TMP/run.err"; then
    ok "device-free fake E2E matrix completes"
else
    nok "device-free fake E2E matrix completes"
    sed -n '1,160p' "$TMP/run.err" >&2
fi

if [ "$(grep -c '^argv=' "$log" 2>/dev/null || true)" -eq 4 ]; then
    ok "two worker counts invoke both arms"
else
    nok "two worker counts invoke both arms"
fi
if awk '/^env=/{ plain=($0 !~ /EXITOS_FILES=/); exitos=($0 ~ /EXITOS_FILES=/); if (plain && $0 ~ /LD_PRELOAD=/) exit 1; if (exitos && $0 !~ /EXITOS_PREPARE=donor-fallocate/) exit 1; n++ } END{ exit !(n==4) }' "$log"; then
    ok "plain arm is unarmed and Exitos arm carries its configuration"
else
    nok "plain arm is unarmed and Exitos arm carries its configuration"
fi
if [ "$(find "$result/cells" -name readback.txt -type f 2>/dev/null | wc -l)" -eq 4 ] &&
   [ -s "$result/final-manifest.sha256" ]; then
    ok "each cell has one complete readback and one final manifest"
else
    nok "each cell has one complete readback and one final manifest"
fi
if python3 - "$result/cells/t1T-r1-o1-A/meta.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    assert json.load(source)["order"] == ["A", "B"]
PY
then
    ok "cell metadata records the arm order as tokens"
else
    nok "cell metadata records the arm order as tokens"
fi
if ! grep -R -E 'sha(1|224|256|384|512)sum|openssl dgst' "$result/cells" >/dev/null 2>&1; then
    ok "cell commands contain no per-transfer digest"
else
    nok "cell commands contain no per-transfer digest"
fi

proc_log=$TMP/fio-proc.log
proc_result=$TMP/results/proc
if PATH="$TMP/bin:$PATH" FAKE_FIO_LOG="$proc_log" "$RUNNER" --run \
    --mount "$TMP/mount" --result-dir "$proc_result" --cpus 0,1,2,3 \
    "${DEV_ARGS[@]}" \
    --preload-so "$TMP/preload.so" \
    --fio "$TMP/bin/fio" --threads 1,4 --repeats 1 --file-mib 1 \
    --worker-mode process --frontend preload \
    >"$TMP/proc.out" 2>"$TMP/proc.err"; then
    ok "process worker mode completes the same finite matrix"
else
    nok "process worker mode completes the same finite matrix"
    sed -n '1,160p' "$TMP/proc.err" >&2
fi

# One fio process per worker: 1+4 workers per arm, two arms, one repetition.
if [ "$(grep -c '^argv=' "$proc_log" 2>/dev/null || true)" -eq 10 ]; then
    ok "process mode starts one fio process per worker"
else
    nok "process mode starts one fio process per worker"
fi
if grep -q -- '--thread=0 ' "$proc_log" && ! grep -q -- '--thread=1 ' "$proc_log"; then
    ok "process mode asks fio for processes, not threads"
else
    nok "process mode asks fio for processes, not threads"
fi
if [ "$(grep -c 'EXITOS_DONOR_FILES=1;' "$proc_log" 2>/dev/null || true)" -ge 5 ] &&
   [ "$(grep -o 'EXITOS_DONOR_DIR=[^;]*donors-[0-9]*' "$proc_log" | sort -u | wc -l)" -ge 5 ]; then
    ok "each process worker gets a private donor directory of width one"
else
    nok "each process worker gets a private donor directory of width one"
fi
if [ -d "$proc_result/cells/t4P-r1-o1-A" ] && [ -d "$proc_result/cells/t4T-r1-o1-A" ] 2>/dev/null; then
    nok "process cells are named apart from thread cells"
elif [ -d "$proc_result/cells/t4P-r1-o1-A" ]; then
    ok "process cells are named apart from thread cells"
else
    nok "process cells are named apart from thread cells"
fi
if python3 - "$proc_result/cells/t4P-r1-o2-B/meta.json" <<'PY_META'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    assert json.load(source)["worker_mode"] == "process"
PY_META
then
    ok "cell metadata records the worker mode"
else
    nok "cell metadata records the worker mode"
fi

SUB=$ROOT/example/fio-bpftime-e2e/multi-process/run.sh
if [ -x "$SUB" ] && bash -n "$SUB"; then
    ok "the process-sweep entry exists and parses"
else
    nok "the process-sweep entry exists and parses"
fi
has "$SUB" '--worker-mode process' "the process-sweep entry fixes the worker mode"
has "$SUB" 'exec "$PARENT"' "the process-sweep entry runs the parent matrix, not a copy"
if PATH="$TMP/bin:$PATH" "$SUB" --worker-mode thread --mount "$TMP/mount" \
     --result-dir "$TMP/results/sub" --cpus 0,1 >/dev/null 2>&1; then
    nok "the process-sweep entry refuses an overriding --worker-mode"
else
    ok "the process-sweep entry refuses an overriding --worker-mode"
fi
if [ -f "$ROOT/example/fio-bpftime-e2e/multi-process/README.md" ]; then
    ok "the process-sweep entry documents itself"
else
    nok "the process-sweep entry documents itself"
fi
# Results live under the operator's --result-dir, never inside the repository.
if find "$ROOT/example" -type d -name results | grep -q .; then
    nok "the example ships no result directories"
else
    ok "the example ships no result directories"
fi
if [ -e "$ROOT/example/fio-no-poll" ]; then
    nok "the retired plain-only example is gone"
else
    ok "the retired plain-only example is gone"
fi
if [ -e "$ROOT/example/uring-passthrough-qd" ]; then
    nok "the queue-depth example now lives under tests/"
elif [ -x "$ROOT/tests/uring-passthrough-qd/run.sh" ]; then
    ok "the queue-depth example now lives under tests/"
else
    nok "the queue-depth example now lives under tests/"
fi

# --- the Exitos arm cannot register a file without EXITOS_DEV, and --device is
# --- the only channel that reaches it, so the runner has to refuse early.
err=$TMP/nodev.err
if PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
    --mount "$TMP/mount" --result-dir "$TMP/results/nodev" --cpus 0,1 \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --threads 1,2 >/dev/null 2>"$err"; then
    nok "the Exitos arm is refused without --device"
else
    has "$err" 'device_required_for_exitos_arm' "the Exitos arm is refused without --device"
fi
# --- the plain-only arm needs no device, so it must still be accepted.
if PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
    --mount "$TMP/mount" --result-dir "$TMP/results/plainonly" --cpus 0,1 \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --threads 1,2 --only plain >/dev/null 2>"$TMP/plainonly.err"; then
    ok "the plain-only arm still runs without --device"
else
    nok "the plain-only arm still runs without --device"
    sed -n '1,20p' "$TMP/plainonly.err" >&2
fi
# --- fio ends a --thread=0 job with _exit(), which skips DSO destructors; only
# --- the preload frontend interposes _exit to publish the counters.
err=$TMP/procbpf.err
if PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
    --mount "$TMP/mount" --result-dir "$TMP/results/procbpf" --cpus 0,1 \
    "${DEV_ARGS[@]}" \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --threads 1,2 --worker-mode process --frontend bpftime \
    >/dev/null 2>"$err"; then
    nok "process mode refuses the instruction-rewriting frontend"
else
    has "$err" 'process_mode_requires_preload_frontend' "process mode refuses the instruction-rewriting frontend"
fi
has "$RUNNER" 'EXITOS_STATS_ON_WORKER_EXIT=1' "process mode arms the worker-exit counter dump"
# --- the temporary work root lives inside the mount; a successful run must not
# --- leave it behind.
leftovers=$(find "$TMP/mount" -maxdepth 1 -name '.exitos-fio-bpftime.*' -print 2>/dev/null | wc -l)
if [ "$leftovers" -eq 0 ]; then
    ok "a successful run leaves no work root inside the mount"
else
    nok "a successful run leaves no work root inside the mount ($leftovers left)"
fi
# --- the documented commands have to carry what the runner now requires.
for doc in "$EXAMPLE/README.md" "$ROOT/example/README.md"; do
    has "$doc" '--device' "$(basename "$(dirname "$doc")")/README.md documents --device"
    has "$doc" '--expect-identity' "$(basename "$(dirname "$doc")")/README.md documents --expect-identity"
done
has "$EXAMPLE/multi-process/README.md" '--frontend preload' "the process entry documents the frontend it needs"

printf '1..%d  (%d failed)\n' "$n" "$fail"
[ "$fail" -eq 0 ]
