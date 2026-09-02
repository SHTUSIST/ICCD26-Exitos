#!/bin/bash
# Device-free contract for the public single-thread real-QD example.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
EXAMPLE=$ROOT/tests/uring-passthrough-qd
RUNNER=$EXAMPLE/run.sh
SUMMARY=$EXAMPLE/summarize.py
TMP=$(mktemp -d /tmp/exitos-uring-qd-example.XXXXXX)
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

if [ -x "$RUNNER" ] && bash -n "$RUNNER"; then
    ok "runner exists, is executable, and parses"
else
    bad "runner exists, is executable, and parses"
fi
if [ -x "$SUMMARY" ] && python3 -m py_compile "$SUMMARY"; then
    ok "summarizer exists, is executable, and parses"
else
    bad "summarizer exists, is executable, and parses"
fi

mkdir -p -- "$TMP/bin" "$TMP/mount" "$TMP/results"
: >"$TMP/active.so"
: >"$TMP/transformer.so"
: >"$TMP/whole"
: >"$TMP/generic"
: >"$TMP/partition"
cat >"$TMP/bin/fio" <<'EOF'
#!/bin/bash
exit 0
EOF
chmod +x "$TMP/bin/fio"

cat >"$TMP/bin/sleep" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >>"${FAKE_SLEEP_LOG:?}"
EOF
chmod +x "$TMP/bin/sleep"

cat >"$TMP/bin/qd_bench" <<'EOF'
#!/bin/bash
set -euo pipefail
arm= file= partition= char_device= expect_identity= qd= operations= output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --arm) arm=$2; shift 2 ;;
        --file) file=$2; shift 2 ;;
        --partition) partition=$2; shift 2 ;;
        --char-device) char_device=$2; shift 2 ;;
        --expect-identity) expect_identity=$2; shift 2 ;;
        --qd) qd=$2; shift 2 ;;
        --operations) operations=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        *) printf 'unexpected fake qd_bench option: %s\n' "$1" >&2; exit 90 ;;
    esac
done
printf '%s %s operations=%s file=%s partition=%s char=%s identity=%s\n' \
    "$arm" "$qd" "$operations" "$file" "$partition" "$char_device" \
    "$expect_identity" \
    >>"${FAKE_QD_LOG:?}"

# The runner's test-only readback samples the first and last data blocks.  The
# production path reads every byte once after the complete campaign.
block=${TMPDIR:-/tmp}/pattern.$$
head -c 4096 /dev/zero | tr '\0' 'Z' >"$block"
dd if="$block" of="$file" bs=4096 seek=1 count=1 conv=notrunc status=none
dd if="$block" of="$file" bs=4096 seek="$operations" count=1 conv=notrunc status=none
rm -f -- "$block"

backend=ext4-iopoll
opcode=IORING_OP_WRITE
[ "$arm" = passthrough ] && backend=nvme-uring-cmd-iopoll && opcode=IORING_OP_URING_CMD
max_batch=$qd
[ "${FAKE_QD_BAD_BATCH:-0}" = 1 ] && max_batch=1
json_identity=$expect_identity
[ "${FAKE_QD_BAD_IDENTITY:-0}" = 1 ] && json_identity=eui.wrong-namespace
submit_calls=$((operations / qd))
bytes=$((operations * 4096))
cat >"$output" <<JSON
{"schema_version":1,"threads":1,"bs":4096,"backend":"$backend","opcode":"$opcode","iopoll":true,"pattern_byte":90,"requested_qd":$qd,"max_submitted_batch":$max_batch,"submit_calls":$submit_calls,"submitted_commands":$operations,"completed_commands":$operations,"completion_errors":0,"bytes":$bytes,"elapsed_ns":1000000000,"iops":$operations,"file_path":"$file","file_dev_major":1,"file_dev_minor":2,"file_inode":3,"partition_path":"$partition","partition_dev_major":4,"partition_dev_minor":5,"char_device_path":"$char_device","char_device_major":6,"char_device_minor":7,"namespace_id":1,"namespace_identity":"$json_identity","partition_start_lba":2048}
JSON
EOF
chmod +x "$TMP/bin/qd_bench"

common_args=(
    --mount "$TMP/mount"
    --cpus 2,3
    --active-so "$TMP/active.so"
    --transformer "$TMP/transformer.so"
    --fio "$TMP/bin/fio"
    --device "$TMP/partition"
    --expect-identity eui.fake
)
common_env=(
    EXITOS_QD_EXAMPLE_TEST_MODE=1
    EXITOS_QD_BENCH_BIN="$TMP/bin/qd_bench"
    EXITOS_TRANSACTION_WHOLE="$TMP/whole"
    EXITOS_TRANSACTION_GENERIC="$TMP/generic"
    PATH="$TMP/bin:$PATH"
)

PLAN=$TMP/plan.txt
PLAN_RESULT=$TMP/results/plan-must-not-exist
if env "${common_env[@]}" "$RUNNER" --plan "${common_args[@]}" \
    --result-dir "$PLAN_RESULT" --manifest-mode outer >"$PLAN" 2>"$TMP/plan.err"; then
    ok "plan accepts the transaction-compatible interface"
else
    bad "plan accepts the transaction-compatible interface"
fi
contains "plan fixes one thread, 4 KiB, and 8 GiB per cell" "$PLAN" \
    'threads=1 bs=4096 operations=2097152 bytes=8589934592'
contains "plan publishes only the static queue depths" "$PLAN" \
    'qd_list=1,2,4,8 adaptive=forbidden'
contains "plan publishes four balanced repetitions" "$PLAN" \
    'reps=4 qd_orders=1,2,4,8/8,4,2,1/2,1,8,4/4,8,1,2'
contains "plan reverses the two arms" "$PLAN" \
    'arm_orders=ext4-passthrough/passthrough-ext4'
contains "plan leaves passthrough as the final writer for correctness readback" "$PLAN" \
    'arm_rep_map=ext4-passthrough,passthrough-ext4,passthrough-ext4,ext4-passthrough final_cell=passthrough'
contains "plan fixes ten-second cooldown" "$PLAN" 'cooldown_seconds=10'
contains "plan declares identical command geometry" "$PLAN" \
    'same_region=true same_pattern=0x5a same_commands=2097152 same_batch_qd=true'
contains "plan declares no fallback" "$PLAN" \
    'ext4=IORING_OP_WRITE+iopoll passthrough=IORING_OP_URING_CMD+iopoll fallback=forbidden'
contains "plan declares one final complete readback" "$PLAN" \
    'readback=once-final-complete files=1 data=0x5a guards=0 checksum_per_cell=forbidden'
contains "outer mode delegates the one manifest" "$PLAN" \
    'manifest_mode=outer inner_manifest=absent'
contains "plan requires qd_bench to match its Makefile dependencies" "$PLAN" \
    'qd_bench_freshness=make-q'
if [ "$(grep -Ec '^[[:space:]]*dd if=/dev/zero .*oflag=direct' "$RUNNER")" -eq 2 ]; then
    ok "production initialization uses direct I/O for both writes"
else
    bad "production initialization uses direct I/O for both writes"
fi
if [ ! -e "$PLAN_RESULT" ] && [ ! -e "$TMP/qd-plan.log" ] && \
   [ ! -e "$TMP/sleep-plan.log" ]; then
    ok "plan mode has no workload, cooldown, or result side effects"
else
    bad "plan mode has no workload, cooldown, or result side effects"
fi

if env "${common_env[@]}" "$RUNNER" --plan "${common_args[@]}" \
    --result-dir "$TMP/results/subset-plan" --manifest-mode self \
    --qd-list 8,2 >"$TMP/subset.plan" 2>"$TMP/subset.err"; then
    ok "plan accepts a static subset of the supported queue depths"
else
    bad "plan accepts a static subset of the supported queue depths"
fi
contains "subset remains static" "$TMP/subset.plan" 'qd_list=8,2 adaptive=forbidden'
for invalid in 3 1,3 auto 1,1 '1, 2'; do
    if env "${common_env[@]}" "$RUNNER" --plan "${common_args[@]}" \
        --result-dir "$TMP/results/reject-${invalid//[^a-zA-Z0-9]/_}" \
        --manifest-mode self --qd-list "$invalid" >/dev/null 2>&1; then
        bad "runner rejects invalid or non-static qd-list $invalid"
    else
        ok "runner rejects invalid or non-static qd-list $invalid"
    fi
done

RUN_RESULT=$TMP/results/outer
RUN_OUT=$TMP/run.out
RUN_LOG=$TMP/qd.log
SLEEP_LOG=$TMP/sleep.log
READBACK_LOG=$TMP/readback.log
if env "${common_env[@]}" FAKE_QD_LOG="$RUN_LOG" FAKE_SLEEP_LOG="$SLEEP_LOG" \
    EXITOS_QD_TEST_READBACK_LOG="$READBACK_LOG" \
    "$RUNNER" --run "${common_args[@]}" --result-dir "$RUN_RESULT" \
    --manifest-mode outer >"$RUN_OUT" 2>"$TMP/run.err"; then
    ok "device-free fake campaign completes"
else
    bad "device-free fake campaign completes"
    sed -n '1,160p' "$TMP/run.err" >&2
fi

[ "$(wc -l <"$RUN_LOG" 2>/dev/null || printf 0)" -eq 32 ] &&
    ok "default matrix invokes qd_bench exactly 32 times" ||
    bad "default matrix invokes qd_bench exactly 32 times"
[ "$(grep -c '^10$' "$SLEEP_LOG" 2>/dev/null || true)" -eq 32 ] &&
    ok "every cell receives the fixed ten-second cooldown" ||
    bad "every cell receives the fixed ten-second cooldown"
if awk '$3 != "operations=2097152" { exit 1 } END { if (NR != 32) exit 1 }' "$RUN_LOG"; then
    ok "every cell sends exactly 2097152 four-KiB commands"
else
    bad "every cell sends exactly 2097152 four-KiB commands"
fi
if awk '$7 != "identity=eui.fake" { exit 1 } END { if (NR != 32) exit 1 }' "$RUN_LOG"; then
    ok "every cell binds the opened partition to the expected namespace identity"
else
    bad "every cell binds the opened partition to the expected namespace identity"
fi
sed -E 's/ operations=.*//' "$RUN_LOG" >"$TMP/order.actual"
cat >"$TMP/order.expected" <<'EOF'
ext4 1
passthrough 1
ext4 2
passthrough 2
ext4 4
passthrough 4
ext4 8
passthrough 8
passthrough 8
ext4 8
passthrough 4
ext4 4
passthrough 2
ext4 2
passthrough 1
ext4 1
passthrough 2
ext4 2
passthrough 1
ext4 1
passthrough 8
ext4 8
passthrough 4
ext4 4
ext4 4
passthrough 4
ext4 8
passthrough 8
ext4 1
passthrough 1
ext4 2
passthrough 2
EOF
if diff -u "$TMP/order.expected" "$TMP/order.actual" >/dev/null; then
    ok "matrix uses the balanced QD and reversed arm orders"
else
    bad "matrix uses the balanced QD and reversed arm orders"
    diff -u "$TMP/order.expected" "$TMP/order.actual" >&2 || true
fi
if [ "$(tail -n 1 "$RUN_LOG" | awk '{print $1}')" = passthrough ]; then
    ok "the final complete readback observes the final passthrough overwrite"
else
    bad "the final complete readback observes the final passthrough overwrite"
fi
if [ "$(sed -n 's/.* file=\([^ ]*\) partition=.*/\1/p' "$RUN_LOG" | sort -u | wc -l)" -eq 1 ]; then
    ok "both arms overwrite the exact same backing file and physical region"
else
    bad "both arms overwrite the exact same backing file and physical region"
fi
if [ "$(wc -l <"$READBACK_LOG" 2>/dev/null || printf 0)" -eq 1 ] &&
   grep -Fq 'FINAL_READBACK_OK files=1' "$RUN_RESULT/final-readback.txt"; then
    ok "campaign performs exactly one final readback pass over the shared file"
else
    bad "campaign performs exactly one final readback pass over the shared file"
fi
if [ ! -e "$RUN_RESULT/MANIFEST.sha256" ] &&
   grep -Fq 'MANIFEST_DEFERRED owner=outer' "$RUN_OUT"; then
    ok "outer manifest mode creates no nested manifest"
else
    bad "outer manifest mode creates no nested manifest"
fi
if python3 - "$RUN_RESULT/summary.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1], encoding="ascii"))
assert s["schema_version"] == 1
assert sorted(s["qds"], key=int) == ["1", "2", "4", "8"]
for qd in s["qds"].values():
    assert len(qd["ext4"]["samples_iops"]) == 4
    assert len(qd["passthrough"]["samples_iops"]) == 4
    assert "passthrough_vs_ext4_percent" in qd
PY
then
    ok "summary reports four samples, medians, and comparison for every QD"
else
    bad "summary reports four samples, medians, and comparison for every QD"
fi

SELF_RESULT=$TMP/results/self
if env "${common_env[@]}" FAKE_QD_LOG="$TMP/self-qd.log" \
    FAKE_SLEEP_LOG="$TMP/self-sleep.log" \
    EXITOS_QD_TEST_READBACK_LOG="$TMP/self-readback.log" \
    "$RUNNER" --run "${common_args[@]}" --result-dir "$SELF_RESULT" \
    --manifest-mode self --qd-list 1 >"$TMP/self.out" 2>"$TMP/self.err" &&
   [ -s "$SELF_RESULT/MANIFEST.sha256" ] &&
   [ "$(find "$SELF_RESULT" -name MANIFEST.sha256 -type f | wc -l)" -eq 1 ] &&
   grep -Fq 'MANIFEST_OK mode=self count=1' "$TMP/self.out"; then
    ok "self mode emits one final campaign manifest"
else
    bad "self mode emits one final campaign manifest"
    sed -n '1,120p' "$TMP/self.err" >&2
fi

BAD_RESULT=$TMP/results/bad-batch
if env "${common_env[@]}" FAKE_QD_BAD_BATCH=1 \
    FAKE_QD_LOG="$TMP/bad-qd.log" FAKE_SLEEP_LOG="$TMP/bad-sleep.log" \
    "$RUNNER" --run "${common_args[@]}" --result-dir "$BAD_RESULT" \
    --manifest-mode outer --qd-list 2 >"$TMP/bad.out" 2>"$TMP/bad.err"; then
    bad "runner rejects a fake that did not reach the requested batch"
else
    ok "runner rejects a fake that did not reach the requested batch"
fi
contains "batch mismatch is explicit" "$TMP/bad.err" 'max_submitted_batch'

BAD_ID_RESULT=$TMP/results/bad-identity
if env "${common_env[@]}" FAKE_QD_BAD_IDENTITY=1 \
    FAKE_QD_LOG="$TMP/bad-id-qd.log" FAKE_SLEEP_LOG="$TMP/bad-id-sleep.log" \
    "$RUNNER" --run "${common_args[@]}" --result-dir "$BAD_ID_RESULT" \
    --manifest-mode outer --qd-list 1 >"$TMP/bad-id.out" 2>"$TMP/bad-id.err"; then
    bad "runner rejects a cell whose namespace identity differs from expectation"
else
    ok "runner rejects a cell whose namespace identity differs from expectation"
fi
contains "identity mismatch is explicit" "$TMP/bad-id.err" \
    'namespace_identity mismatch'

contains "README gives a copy-paste runner invocation" "$EXAMPLE/README.md" './tests/uring-passthrough-qd/run.sh --plan'
contains "README distinguishes this from fio psync numjobs" "$EXAMPLE/README.md" 'not a fio `psync`/`numjobs` benchmark'
contains "README states there is no fallback" "$EXAMPLE/README.md" 'no fallback'

if [ "$FAIL" -eq 0 ]; then
    printf 'PASS test_uring_passthrough_qd_example assertions=%d\n' "$PASS"
    exit 0
fi
printf 'FAIL test_uring_passthrough_qd_example pass=%d fail=%d\n' "$PASS" "$FAIL" >&2
exit 1
