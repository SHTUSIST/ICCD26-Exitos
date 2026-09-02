#!/bin/bash
# Device-free contract for the fixed-QD1 4 KiB 4T/8T diagnostic matrix.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
RUNNER=$ROOT/tools/run-4k-thread-qd1-matrix.sh
TMP=$(mktemp -d /tmp/exitos-4k-thread-qd1-contract.XXXXXX)
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

mkdir -p -- "$TMP/mount" "$TMP/results" "$TMP/bin"
for name in active transformer; do
    printf 'contract fixture\n' >"$TMP/$name.so"
done
cat >"$TMP/bin/fio" <<'EOF'
#!/bin/sh
if [ "${1-}" = --version ]; then printf '%s\n' fio-contract; exit 0; fi
echo 'plan contract must not execute fio' >&2
exit 99
EOF
chmod +x "$TMP/bin/fio"
cat >"$TMP/bin/mountpoint" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$TMP/bin/mountpoint"
cat >"$TMP/bin/findmnt" <<'EOF'
#!/bin/sh
printf '%s\n' '/dev/fakep1 ext4'
EOF
chmod +x "$TMP/bin/findmnt"

PLAN=$TMP/plan.txt
PATH="$TMP/bin:$PATH" "$RUNNER" --plan \
    --mount "$TMP/mount" --result-dir "$TMP/results/must-not-exist" \
    --cpus 0,1,2,3,4,5,6,7 --active-so "$TMP/active.so" \
    --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --device /dev/fakep1 \
    --expect-identity eui.fake >"$PLAN"

contains "plan fixes the two requested thread cells" "$PLAN" \
    'configs=threads4:4,threads8:8'
contains "plan fixes 4 KiB requests" "$PLAN" 'bs=4k'
contains "plan fixes one request per worker" "$PLAN" 'per_worker_qd=1'
contains "plan uses synchronous callers" "$PLAN" 'ioengine=psync'
contains "plan keeps fio allocation controls at verified defaults" "$PLAN" \
    'direct=1 fallocate=native(default) overwrite=0(default) create_on_open=0(default)'
contains "plan requests fio fdatasync-after-write semantics" "$PLAN" 'fdatasync=1'
contains "plan records fio's exact final-write sync count" "$PLAN" \
    'sync_ios=write_ios-jobs'
contains "plan hard-selects passthrough polling" "$PLAN" \
    'backend=uringpoll opcode=IORING_OP_URING_CMD bypass_block_layer=1'
contains "plan labels the fio scheduler-policy pilots precisely" "$PLAN" \
    'scheduler-policy-pilots=4T+8T parent=verified-per-cell'
contains "plan holds frontend and donor preparation constant" "$PLAN" \
    'arms=B:active-bpftime-prepared-ext4,C:active-bpftime-uringpoll'
contains "plan changes only the fastpath boolean" "$PLAN" \
    'B:EXITOS_FASTPATH=0 C:EXITOS_FASTPATH=1'
contains "plan publishes balanced thread and arm order" "$PLAN" \
    'blocks=4B-4C-8B-8C,8C-8B-4C-4B,4C-4B-8C-8B,8B-8C-4B-4C,8B-8C-4B-4C,4C-4B-8C-8B,8C-8B-4C-4B,4B-4C-8B-8C'
contains "plan defaults to ten-second cooldown" "$PLAN" 'cooldown_seconds=10'
contains "plan places cooldown after preparation and before timed snapshots" "$PLAN" \
    'prepare_timing=serialized-setup_files->Nth-PREPARE_READY->ARM->selected-close/forget->SIGSTOP->cooldown->timed-pre-snapshot->SIGCONT->timed-io'
contains "plan fixes equal bytes in every cell" "$PLAN" \
    'logical_write_gib_per_cell=32'
contains "plan cannot let healthy 4T mask an 8T regression" "$PLAN" \
    'thread-scaling=(C8/B8)/(C4/B4):-5pct+7of8'
contains "plan requires one final readback" "$PLAN" \
    'correctness=one-untimed-complete-readback'
plan_fio=$(grep '^PLAN_FIO ' "$PLAN")
if [[ $plan_fio == *--fallocate=* || $plan_fio == *--overwrite=* ||
      $plan_fio == *--create_on_open=* ]]; then
    bad "timed fio argv omits allocation-policy overrides"
else
    ok "timed fio argv omits allocation-policy overrides"
fi
[ ! -e "$TMP/results/must-not-exist" ] && \
    ok "plan is read-only" || bad "plan is read-only"

if PATH="$TMP/bin:$PATH" "$RUNNER" --plan --mount "$TMP/mount" \
    --result-dir "$TMP/results/bad" --cpus 0,1,2,3,4,5,6,7 \
    --backend uringwritepoll >/dev/null 2>&1; then
    bad "runner rejects any backend override"
else
    ok "runner rejects any backend override"
fi
if PATH="$TMP/bin:$PATH" "$RUNNER" --plan --mount "$TMP/mount" \
    --result-dir "$TMP/results/bad" --cpus 0,1,2,3 \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" \
    --device /dev/fakep1 --expect-identity eui.fake >/dev/null 2>&1; then
    bad "runner rejects fewer than eight distinct CPUs"
else
    ok "runner rejects fewer than eight distinct CPUs"
fi

sha_calls=$(grep -Eo '(^|[^[:alnum:]_])sha256sum([^[:alnum:]_]|$)' "$RUNNER" | wc -l)
[ "$sha_calls" -eq 1 ] && ok "runner has exactly one final sha invocation" || \
    bad "runner has exactly one final sha invocation"
if grep -Fq 'EXITOS_*|LD_PRELOAD|LD_AUDIT' "$RUNNER" &&
   grep -Fq 'env "${clean_unsets[@]}"' "$RUNNER"; then
    ok "runner clears inherited frontend and trace environment"
else
    bad "runner clears inherited frontend and trace environment"
fi

contains "runner explicitly serializes all setup before creating timed workers" \
    "$RUNNER" '--create_serialize=1'
if grep -Fq -- '--group_reporting=1' "$RUNNER"; then
    bad "runner preserves per-job fio JSON instead of group aggregation"
else
    ok "runner preserves per-job fio JSON instead of group aggregation"
fi
contains "scheduler pilot retries until the full worker invariant converges" \
    "$RUNNER" 'scheduler_snapshot_converged=1'
contains "scheduler pilot publishes only an atomic converged snapshot" \
    "$RUNNER" 'mv -- "$scheduler_snapshot_tmp" "$cell/scheduler-workers.txt"'
actual_attest_calls=$(grep -Fc 'attest_fio_tasks "$cell" "$jobs" "$allowed"' \
    "$RUNNER" || true)
if [ "$actual_attest_calls" -eq 0 ]; then
    ok "setup-stage stop is not mislabeled as actual worker attestation"
else
    bad "setup-stage stop is not mislabeled as actual worker attestation"
fi
contains "release revalidates the exact stopped fio process" "$RUNNER" \
    'revalidate_stopped_fio "$cell"'
contains "fio discovery compares the executable rather than its basename" \
    "$RUNNER" '"$exe" = "$fio_resolved"'
if grep -Fq 'pgrep -s "$1" -x fio' "$RUNNER"; then
    bad "fio discovery does not require a hard-coded process name"
else
    ok "fio discovery does not require a hard-coded process name"
fi
contains "timed cells have a bounded but ample timeout" "$RUNNER" \
    'cell_timeout_seconds=3600'
contains "preparation writeback is flushed outside the timed interval" "$RUNNER" \
    'sync -f -- "$mount_dir"'
contains "thermal-management transitions are frozen across the campaign" "$RUNNER" \
    '.thm_temp2_trans_count] | @tsv'
contains "timed temperatures stay below the controller warning threshold" \
    "$RUNNER" 'timed_temperature_at_or_above_warning'
contains "runner arms the shared frontend preparation stop" "$RUNNER" \
    'EXITOS_PREPARE_READY_STOP=1'
contains "runner makes the expected preparation count equal the jobs" "$RUNNER" \
    'EXITOS_PREPARE_EXPECTED="$jobs"'
contains "active bpftime publishes the exact preparation stop marker" \
    "$ROOT/src/bpftime_hook.c" 'PREPARE_STOP_READY v=1 frontend=bpftime'
contains "LD_PRELOAD implements the same preparation stop contract" \
    "$ROOT/src/preload.c" 'PREPARE_STOP_READY v=1 frontend=ld_preload'
if [ "$(grep -F 'raise(SIGSTOP)' "$ROOT/src/bpftime_hook.c" "$ROOT/src/preload.c" | wc -l)" -eq 2 ]; then
    ok "both interception frontends issue the same process stop"
else
    bad "both interception frontends issue the same process stop"
fi

if PATH="$TMP/bin:$PATH" "$RUNNER" --plan --mount "$TMP/mount" \
    --result-dir "$TMP/results/bad" --cpus 0,1,2,3,4,5,6,7 \
    --active-so "$TMP/active.so" --transformer "$TMP/transformer.so" \
    --fio "$TMP/bin/fio" --device /dev/fakep1 --expect-identity eui.fake \
    --cooldown-seconds 61 >/dev/null 2>&1; then
    bad "runner bounds the stopped-process cooldown"
else
    ok "runner bounds the stopped-process cooldown"
fi

if [ "$FAIL" -ne 0 ]; then
    printf 'FAIL test_4k_thread_qd1_matrix assertions=%d failures=%d\n' \
        "$((PASS + FAIL))" "$FAIL" >&2
    exit 1
fi
printf 'PASS test_4k_thread_qd1_matrix assertions=%d\n' "$PASS"
