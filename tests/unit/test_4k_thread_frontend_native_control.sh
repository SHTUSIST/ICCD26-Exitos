#!/bin/bash
# Device-free contract for the 4 KiB frontend/native thread control runner.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
RUNNER=$ROOT/tools/run-4k-thread-frontend-native-control.sh
TMP=$(mktemp -d /tmp/exitos-frontend-native-control.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); printf 'ok %d - %s\n' "$PASS" "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'not ok - %s\n' "$1" >&2; }
contains()
{
    if grep -Fq -- "$3" "$2"; then ok "$1"; else
        bad "$1 (missing: $3)"
    fi
}

if [ -x "$RUNNER" ]; then ok "runner exists and is executable"; else
    bad "runner exists and is executable"
fi
if [ -r "$RUNNER" ] && bash -n "$RUNNER"; then ok "runner parses as bash"; else
    bad "runner parses as bash"
fi

mkdir -p -- "$TMP/bin" "$TMP/mount" "$TMP/results"
cat >"$TMP/empty.c" <<'EOF'
static void fixture(void) {}
EOF
cc -shared -fPIC -o "$TMP/libexitos_bpftime_active.so" "$TMP/empty.c"
cc -shared -fPIC -o "$TMP/libexitos_preload.so" "$TMP/empty.c"
cp -- "$TMP/libexitos_preload.so" "$TMP/transformer.so"

cat >"$TMP/bin/sleep" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >>"${FRONTEND_NATIVE_FAKE_SLEEP_LOG:?}"
EOF
chmod +x "$TMP/bin/sleep"

cat >"$TMP/bin/fio" <<'EOF'
#!/bin/bash
set -euo pipefail
if [ "${1-}" = --version ]; then printf '%s\n' fio-3.41; exit 0; fi
if [ "${1-}" = --enghelp=io_uring_cmd ]; then
    printf '%s\n' hipri cmd_type fixedbufs registerfiles writefua
    exit 0
fi

name= output= filename_format= filename= offset=0 offset_increment=0 jobs=1
rw= size= bs= iodepth= thread=
for arg in "$@"; do
    case "$arg" in
        --name=*) name=${arg#*=} ;;
        --output=*) output=${arg#*=} ;;
        --filename_format=*) filename_format=${arg#*=} ;;
        --filename=*) filename=${arg#*=} ;;
        --offset=*) offset=${arg#*=} ;;
        --offset_increment=*) offset_increment=${arg#*=} ;;
        --numjobs=*) jobs=${arg#*=} ;;
        --rw=*) rw=${arg#*=} ;;
        --size=*) size=${arg#*=} ;;
        --bs=*) bs=${arg#*=} ;;
        --iodepth=*) iodepth=${arg#*=} ;;
        --thread=*) thread=${arg#*=} ;;
    esac
done

frontend=native
if [[ ${LD_PRELOAD-} == *libexitos_bpftime_active.so* ]]; then
    frontend=active-bpftime
elif [[ ${LD_PRELOAD-} == *libexitos_preload.so* ]]; then
    frontend=preload
elif [ "$name" = frontend-native-prepare ]; then
    frontend=prepare
fi
{
    printf 'CALL frontend=%s name=%s jobs=%s rw=%s size=%s bs=%s iodepth=%s thread=%s' \
        "$frontend" "$name" "$jobs" "$rw" "$size" "$bs" "$iodepth" "$thread"
    printf ' fastpath=%s iopath=%s strict=%s filename=%s offset=%s offset_increment=%s ARGV' \
        "${EXITOS_FASTPATH-}" "${EXITOS_IOPATH-}" "${EXITOS_STRICT-}" \
        "$filename" "$offset" "$offset_increment"
    printf ' %q' "$@"
    printf '\n'
} >>"${FRONTEND_NATIVE_FAKE_LOG:?}"

if [ "$frontend" = prepare ]; then
    for ((job = 0; job < jobs; job++)); do
        path=${filename_format//\$jobnum/$job}
        mkdir -p -- "$(dirname -- "$path")"
        head -c 4096 /dev/zero | tr '\0' 'Z' >"$path"
    done
    exit 0
fi

per_job_ios=131072
sync_per_job=0
if [ "$frontend" = active-bpftime ] || [ "$frontend" = preload ]; then
    sync_per_job=$((per_job_ios - 1))
    stats=${EXITOS_STATS//%p/$$}
    fast_write=$((per_job_ios * jobs))
    if [ "${FRONTEND_NATIVE_FAKE_BAD_STATS:-0}" = 1 ]; then
        fast_write=$((fast_write - 1))
    fi
    printf '%s\n%s\n0\n0\n0\n' "$fast_write" \
        "$((sync_per_job * jobs))" >"$stats"
    for ((job = 0; job < jobs; job++)); do
        printf 'register job=%s -> FAST PATH\n' "$job" >&2
    done
    if [ "$frontend" = active-bpftime ]; then
        printf '%s\n' 'ARMED (syscall instructions rewritten)' >&2
        printf '[exitos/bpftime] hook entered %s times, claimed %s\n' \
            "$((fast_write * 2 + jobs))" "$((fast_write * 2))" >&2
    fi
else
    case "$offset_increment" in
        *m) offset_increment=$(( ${offset_increment%m} * 1024 * 1024 )) ;;
    esac
    python3 - "$offset" "$offset_increment" "$jobs" \
        "${FRONTEND_NATIVE_FAKE_WHOLE:?}" <<'PY'
import os, sys
offset, increment, jobs = map(int, sys.argv[1:4])
fd = os.open(sys.argv[4], os.O_RDWR)
try:
    for job in range(jobs):
        os.pwrite(fd, b"Z" * 4096, offset + job * increment)
finally:
    os.close(fd)
PY
fi

python3 - "$output" "$jobs" "$per_job_ios" "$sync_per_job" <<'PY'
import json, sys
path, jobs, ios, syncs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
data = {"jobs": []}
for _ in range(jobs):
    data["jobs"].append({
        "error": 0,
        "write": {"iops": 100000.0, "total_ios": ios},
        "sync": {"lat_ns": {"N": syncs}},
    })
with open(path, "w", encoding="ascii") as stream:
    json.dump(data, stream)
PY
EOF
chmod +x "$TMP/bin/fio"

WHOLE=$TMP/whole
GENERIC=$TMP/generic
DEVICE=$TMP/partition
truncate -s 6G "$WHOLE"
: >"$GENERIC"
: >"$DEVICE"

PLAN=$TMP/plan.txt
PLAN_RESULT=$TMP/results/plan-must-not-exist
PLAN_LOG=$TMP/plan-fio.log
PLAN_SLEEP=$TMP/plan-sleep.log
if EXITOS_TRANSACTION_WHOLE="$WHOLE" \
   EXITOS_TRANSACTION_GENERIC="$GENERIC" \
   FRONTEND_NATIVE_FAKE_LOG="$PLAN_LOG" \
   FRONTEND_NATIVE_FAKE_SLEEP_LOG="$PLAN_SLEEP" \
   PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
      --mount "$TMP/mount" --result-dir "$PLAN_RESULT" \
      --cpus 0,1,2,3,4,5,6,7 \
      --active-so "$TMP/libexitos_bpftime_active.so" \
      --transformer "$TMP/transformer.so" --fio "$TMP/bin/fio" \
      --device "$DEVICE" --expect-identity eui.fake \
      --manifest-mode outer >"$PLAN" 2>"$TMP/plan.err"; then
    ok "plan accepts the transaction-compatible interface"
else
    bad "plan accepts the transaction-compatible interface"
fi

contains "plan fixes 4T and 8T at per-worker QD1" "$PLAN" \
    'configs=threads4:4,threads8:8 bs=4k per_worker_qd=1'
contains "plan fixes one process with fio worker threads" "$PLAN" \
    'process_model=single-process thread=1'
contains "plan publishes all three control arms" "$PLAN" \
    'arms=active-bpftime,preload,native-io_uring_cmd'
contains "plan publishes two balanced reverse repetitions" "$PLAN" \
    'reps=2 rep0=active-bpftime-preload-native rep1=native-preload-active-bpftime'
contains "plan fixes ten-second cooldown" "$PLAN" 'cooldown_seconds=10'
contains "plan precreates and initializes all eight files" "$PLAN" \
    'files=8 file_mib=512 initialize=0x5a'
contains "plan forces uringpoll without fallback for both frontends" "$PLAN" \
    'frontends=active-bpftime+preload fastpath=1 iopath=uringpoll fallback=forbidden'
contains "plan uses native polled uring command" "$PLAN" \
    'ioengine=io_uring_cmd cmd_type=nvme hipri=1'
contains "plan places native data after the partition" "$PLAN" \
    'raw_window=partition-end+0 bytes=4294967296'
contains "plan performs one campaign-level readback" "$PLAN" \
    'readback=once-final ext4_bytes=4294967296 raw_bytes=4294967296 expected_byte=0x5a'
contains "plan delegates the manifest to its outer transaction" "$PLAN" \
    'manifest_mode=outer inner_manifest=absent'
if [ ! -e "$PLAN_RESULT" ] && [ ! -e "$PLAN_LOG" ] && [ ! -e "$PLAN_SLEEP" ]; then
    ok "plan mode has no filesystem, fio, or cooldown side effects"
else
    bad "plan mode has no filesystem, fio, or cooldown side effects"
fi

RUN_RESULT=$TMP/results/run
RUN_LOG=$TMP/run-fio.log
RUN_SLEEP=$TMP/run-sleep.log
RUN_OUT=$TMP/run.out
if EXITOS_FRONTEND_NATIVE_TEST_MODE=1 \
   EXITOS_TEST_PART_START_SECTORS=2048 \
   EXITOS_TEST_PART_SIZE_SECTORS=8192 \
   EXITOS_TEST_WHOLE_BYTES=$((6 * 1024 * 1024 * 1024)) \
   EXITOS_TRANSACTION_WHOLE="$WHOLE" \
   EXITOS_TRANSACTION_GENERIC="$GENERIC" \
   FRONTEND_NATIVE_FAKE_WHOLE="$WHOLE" \
   FRONTEND_NATIVE_FAKE_LOG="$RUN_LOG" \
   FRONTEND_NATIVE_FAKE_SLEEP_LOG="$RUN_SLEEP" \
   PATH="$TMP/bin:$PATH" "$RUNNER" --run \
      --mount "$TMP/mount" --result-dir "$RUN_RESULT" \
      --cpus 0,1,2,3,4,5,6,7 \
      --active-so "$TMP/libexitos_bpftime_active.so" \
      --transformer "$TMP/transformer.so" --fio "$TMP/bin/fio" \
      --device "$DEVICE" --expect-identity eui.fake \
      --manifest-mode outer >"$RUN_OUT" 2>"$TMP/run.err"; then
    ok "device-free fake campaign completes"
else
    bad "device-free fake campaign completes"
    sed -n '1,120p' "$TMP/run.err" >&2
    sed -n '1,80p' \
        "$RUN_RESULT/cells/rep0-threads4-ord2-native/stderr.txt" >&2 || true
fi

[ "$(grep -c '^CELL_OK ' "$RUN_OUT" 2>/dev/null || true)" -eq 12 ] &&
    ok "campaign runs exactly twelve cells" || bad "campaign runs exactly twelve cells"
[ "$(grep -c '^CALL frontend=prepare ' "$RUN_LOG" 2>/dev/null || true)" -eq 1 ] &&
    ok "campaign preinitializes files exactly once" || bad "campaign preinitializes files exactly once"
for arm in active-bpftime preload native; do
    if [ "$(grep -c "^CALL frontend=$arm " "$RUN_LOG" 2>/dev/null || true)" -eq 4 ]; then
        ok "campaign runs four $arm cells"
    else
        bad "campaign runs four $arm cells"
    fi
done
[ "$(grep -c '^10$' "$RUN_SLEEP" 2>/dev/null || true)" -eq 12 ] &&
    ok "every timed cell receives the fixed ten-second cooldown" ||
    bad "every timed cell receives the fixed ten-second cooldown"
if grep '^CALL frontend=active-bpftime\|^CALL frontend=preload' "$RUN_LOG" |
   grep -vq 'fastpath=1 iopath=uringpoll strict=0'; then
    bad "both interception frontends hard-select fast uringpoll"
else
    ok "both interception frontends hard-select fast uringpoll"
fi
if grep '^CALL frontend=active-bpftime\|^CALL frontend=preload' "$RUN_LOG" |
   grep -vq -- '--ioengine=psync.*--iodepth=1.*--thread=1'; then
    bad "frontend workloads remain psync QD1 in one process"
else
    ok "frontend workloads remain psync QD1 in one process"
fi
if grep '^CALL frontend=native ' "$RUN_LOG" |
   grep -vq -- '--ioengine=io_uring_cmd.*--cmd_type=nvme.*--hipri=1.*--iodepth=1.*--thread=1'; then
    bad "native workload is polled io_uring_cmd QD1 in one process"
else
    ok "native workload is polled io_uring_cmd QD1 in one process"
fi

if [ -f "$RUN_RESULT/results.tsv" ] &&
   [ "$(($(wc -l <"$RUN_RESULT/results.tsv") - 1))" -eq 12 ]; then
    ok "results.tsv retains one row per cell"
else
    bad "results.tsv retains one row per cell"
fi
if awk -F '\t' 'NR == 1 {next} $5 == "active-bpftime" || $5 == "preload" {
        if ($10 != $8 || $11 != $9 || $12 != 0 || $13 != 0 || $14 != 0) exit 1
        seen++
    } END {exit seen == 8 ? 0 : 1}' "$RUN_RESULT/results.tsv" 2>/dev/null; then
    ok "results prove exact frontend takeover and zero fallback"
else
    bad "results prove exact frontend takeover and zero fallback"
fi
if python3 - "$RUN_RESULT/summary.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="ascii"))
assert set(data["median_pair_us"]) == {
    f"threads{threads}:{arm}"
    for threads in (4, 8)
    for arm in ("active-bpftime", "preload", "native")
}
assert all(len(values) == 2 for values in data["pair_us"].values())
PY
then
    ok "summary retains both samples for every config and arm"
else
    bad "summary retains both samples for every config and arm"
fi
contains "campaign performs its final ext4 sample readback" \
    "$RUN_RESULT/final-readback.txt" 'EXT4_READBACK_TEST_SAMPLE_OK files=8'
contains "campaign performs its final raw sample readback" \
    "$RUN_RESULT/final-readback.txt" 'RAW_READBACK_TEST_SAMPLE_OK jobs=8'
[ ! -e "$RUN_RESULT/final-manifest.sha256" ] &&
    ok "outer mode emits no inner manifest" || bad "outer mode emits no inner manifest"

BAD_RESULT=$TMP/results/bad-stats
if EXITOS_FRONTEND_NATIVE_TEST_MODE=1 \
   EXITOS_TEST_PART_START_SECTORS=2048 \
   EXITOS_TEST_PART_SIZE_SECTORS=8192 \
   EXITOS_TEST_WHOLE_BYTES=$((6 * 1024 * 1024 * 1024)) \
   EXITOS_TRANSACTION_WHOLE="$WHOLE" \
   EXITOS_TRANSACTION_GENERIC="$GENERIC" \
   FRONTEND_NATIVE_FAKE_WHOLE="$WHOLE" \
   FRONTEND_NATIVE_FAKE_BAD_STATS=1 \
   FRONTEND_NATIVE_FAKE_LOG="$TMP/bad-fio.log" \
   FRONTEND_NATIVE_FAKE_SLEEP_LOG="$TMP/bad-sleep.log" \
   PATH="$TMP/bin:$PATH" "$RUNNER" --run \
      --mount "$TMP/mount" --result-dir "$BAD_RESULT" \
      --cpus 0,1,2,3,4,5,6,7 \
      --active-so "$TMP/libexitos_bpftime_active.so" \
      --transformer "$TMP/transformer.so" --fio "$TMP/bin/fio" \
      --device "$DEVICE" --expect-identity eui.fake \
      --manifest-mode outer >"$TMP/bad.out" 2>"$TMP/bad.err"; then
    bad "campaign rejects an off-by-one takeover count"
elif grep -Fq 'takeover_count_mismatch' "$TMP/bad.err"; then
    ok "campaign rejects an off-by-one takeover count"
else
    bad "campaign rejects an off-by-one takeover count (wrong failure)"
fi

if [ -r "$RUNNER" ]; then
    sha_calls=$(grep -Eo '(^|[^[:alnum:]_])sha256sum([^[:alnum:]_]|$)' "$RUNNER" | wc -l)
    [ "$sha_calls" -eq 1 ] && ok "runner has one final manifest checksum site" ||
        bad "runner has one final manifest checksum site"
    run_cell_body=$(sed -n '/^run_cell()/,/^}/p' "$RUNNER")
    if grep -Eq 'sha256sum|md5sum|cksum' <<<"$run_cell_body"; then
        bad "timed cells contain no checksum"
    else
        ok "timed cells contain no checksum"
    fi
    if grep -Fq 'EXITOS_TRANSACTION_WHOLE' "$RUNNER" &&
       grep -Fq 'EXITOS_TRANSACTION_GENERIC' "$RUNNER"; then
        ok "runner consumes transaction-resolved whole and generic devices"
    else
        bad "runner consumes transaction-resolved whole and generic devices"
    fi
fi

if [ "$FAIL" -ne 0 ]; then
    printf 'FAIL test_4k_thread_frontend_native_control assertions=%d failures=%d\n' \
        "$((PASS + FAIL))" "$FAIL" >&2
    exit 1
fi
printf 'PASS test_4k_thread_frontend_native_control assertions=%d\n' "$PASS"
