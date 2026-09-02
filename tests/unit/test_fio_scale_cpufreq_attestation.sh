#!/bin/bash
# Pure-fake contract for consuming cpufreq-multicore-transaction.sh v1
# attestations in fio-scale-pilot.sh.  No real sysfs, cpufreq control, fio, or
# block device is touched by this test.
set -u

script=tools/fio-scale-pilot.sh
tmp=$(mktemp -d /tmp/exitos-fio-cpufreq-attest-test.XXXXXX) || exit 1
case "$tmp" in
    /tmp/exitos-fio-cpufreq-attest-test.*) ;;
    *) exit 1 ;;
esac
holder_pid=
race_holder_pid=
race_runner_pid=
cleanup()
{
    if [[ ${race_runner_pid:-} =~ ^[0-9]+$ ]]; then
        kill -CONT "$race_runner_pid" 2>/dev/null || true
        kill -TERM "$race_runner_pid" 2>/dev/null || true
        wait "$race_runner_pid" 2>/dev/null || true
    fi
    if [[ ${race_holder_pid:-} =~ ^[0-9]+$ ]]; then
        kill -TERM "$race_holder_pid" 2>/dev/null || true
        wait "$race_holder_pid" 2>/dev/null || true
    fi
    if [[ ${holder_pid:-} =~ ^[0-9]+$ ]]; then
        kill -TERM "$holder_pid" 2>/dev/null || true
        wait "$holder_pid" 2>/dev/null || true
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT

n=0
bad=0
ok() { n=$((n + 1)); echo "ok $n - $1"; }
not_ok() { n=$((n + 1)); bad=$((bad + 1)); echo "not ok $n - $1"; }
expect_ok()
{
    local label=$1
    shift
    if "$@"; then ok "$label"; else not_ok "$label"; fi
}
expect_fail_with()
{
    local label=$1 pattern=$2 output=$3
    shift 3
    if "$@" >"$output.out" 2>"$output.err"; then
        not_ok "$label"
    elif grep -q -- "$pattern" "$output.err"; then
        ok "$label"
    else
        not_ok "$label"
    fi
}

wait_for_file_from_pid()
{
    local marker=$1 pid=$2
    for _ in $(seq 1 300); do
        [ -f "$marker" ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.01
    done
    return 1
}

sys=$tmp/cpu
locks=$tmp/locks
mkdir -m 0700 -p "$sys/cpufreq/policy2" "$sys/cpufreq/policy8" \
    "$locks/exitos-cpufreq"
chmod 0700 "$locks/exitos-cpufreq"
printf '%s\n' '2,8,10,16' >"$sys/online"

make_policy()
{
    local name=$1 related=$2
    local policy=$sys/cpufreq/$name
    printf '%s\n' "$related" >"$policy/related_cpus"
    printf '%s\n' performance >"$policy/scaling_governor"
    printf '%s\n' 'performance powersave schedutil' \
        >"$policy/scaling_available_governors"
    printf '%s\n' performance >"$policy/energy_performance_preference"
    printf '%s\n' 'performance balance_performance balance_power' \
        >"$policy/energy_performance_available_preferences"
    printf '%s\n' 3800000 >"$policy/scaling_min_freq"
    printf '%s\n' 3800000 >"$policy/scaling_max_freq"
    printf '%s\n' 800000 >"$policy/cpuinfo_min_freq"
    printf '%s\n' 3800000 >"$policy/cpuinfo_max_freq"
    printf '%s\n' 3800000 >"$policy/scaling_cur_freq"
}

make_cpu()
{
    local cpu=$1 siblings=$2 policy=$3
    mkdir -p "$sys/cpu$cpu/topology"
    printf '%s\n' 1 >"$sys/cpu$cpu/online"
    printf '%s\n' "$siblings" >"$sys/cpu$cpu/topology/thread_siblings_list"
    ln -s "../cpufreq/$policy" "$sys/cpu$cpu/cpufreq"
}

make_policy policy2 2,10
make_policy policy8 8,16
make_cpu 2 2,10 policy2
make_cpu 10 2,10 policy2
make_cpu 8 8,16 policy8
make_cpu 16 8,16 policy8
for policy in policy2 policy8; do
    : >"$locks/exitos-cpufreq/$policy.lock"
    chmod 0600 "$locks/exitos-cpufreq/$policy.lock"
done

lock_holder=$tmp/lock-holder.py
cat >"$lock_holder" <<'PY'
#!/usr/bin/env python3
import fcntl
import os
import signal
import sys

marker = sys.argv[1]
fds = []
for path in sys.argv[2:]:
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    fcntl.flock(fd, fcntl.LOCK_EX)
    fds.append(fd)
with open(marker, "w", encoding="ascii") as output:
    output.write("LOCKS_READY\n")
stopping = False
def stop(_signum, _frame):
    global stopping
    stopping = True
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
while not stopping:
    signal.pause()
for fd in fds:
    os.close(fd)
PY
chmod 0700 "$lock_holder"

start_holder()
{
    local marker=$tmp/locks-ready
    rm -f -- "$marker"
    "$lock_holder" "$marker" "$@" &
    holder_pid=$!
    for _ in $(seq 1 200); do
        [ -f "$marker" ] && return 0
        kill -0 "$holder_pid" 2>/dev/null || return 1
        sleep 0.01
    done
    return 1
}

stop_holder()
{
    [ -z "${holder_pid:-}" ] || kill -TERM "$holder_pid" 2>/dev/null || true
    [ -z "${holder_pid:-}" ] || wait "$holder_pid" 2>/dev/null || true
    holder_pid=
}

token=0123456789abcdef0123456789abcdef
policy2_id=$(stat -Lc '%d:%i' "$sys/cpufreq/policy2")
policy8_id=$(stat -Lc '%d:%i' "$sys/cpufreq/policy8")

expected_profile_sha()
{
    local profile=$tmp/expected-profile
    printf '%s\n' \
        'format=exitos-cpufreq-live-profile-v1' \
        'requested_cpus=8,2' \
        'canonical_selected_cpus=2,8' \
        'logical_cpus=2,8,10,16' \
        'policy_count=2' \
        'target_khz=3800000' \
        "policy=1 path=$sys/cpufreq/policy2 identity=$policy2_id related_cpus=2,10 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000" \
        "policy=2 path=$sys/cpufreq/policy8 identity=$policy8_id related_cpus=8,16 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000" \
        'locks_held=yes' >"$profile"
    sha256sum -- "$profile" | awk '{print $1}'
}

write_attestation()
{
    local path=$1
    printf '%s\n' \
        "EXITOS_CPUFREQ_ATTEST version=1 token=$token requested_cpus=8,2 canonical_selected_cpus=2,8 logical_cpus=2,8,10,16 policy_count=2 target_khz=3800000 control_profile_verified=yes actual_frequency_verified=post-keeper-pre-fork locks_held=yes" \
        "EXITOS_CPUFREQ_ATTEST_POLICY index=1 path=$sys/cpufreq/policy2 identity=$policy2_id related_cpus=2,10 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000" \
        "EXITOS_CPUFREQ_ATTEST_POLICY index=2 path=$sys/cpufreq/policy8 identity=$policy8_id related_cpus=8,16 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000" \
        'EXITOS_CPUFREQ_ATTEST_ACTUAL logical_cpus=2:3800000,8:3800000,10:3800000,16:3800000 tolerance_khz=38000 stable_samples=3 loaded_khz=2:3800000,8:3800000,10:3800000,16:3800000 concurrent_keeper_verification=yes keepers_stopped_before_final=yes verification_scope=post-keeper-pre-fork' \
        >"$path"
    chmod 0400 "$path"
}

run_validate()
{
    local fd=$1 supplied_token=$2 pool=${3:-8,2}
    env EXITOS_SCALE_TEST_MODE=1 \
        EXITOS_CPUFREQ_ATTEST_FD="$fd" \
        EXITOS_CPUFREQ_ATTEST_TOKEN="$supplied_token" \
        bash "$script" --test-cpufreq-attestation-validate \
        "$sys" "$locks" "$pool" 3800000
}

open_ro_unlinked()
{
    local path=$1
    exec {att_fd}<"$path"
    rm -f -- "$path"
}

close_attestation()
{
    [ -z "${att_fd:-}" ] || exec {att_fd}<&-
    att_fd=
}

start_holder "$locks/exitos-cpufreq/policy2.lock" \
    "$locks/exitos-cpufreq/policy8.lock" || exit 1

# Positive control: exact v1 contents, an unlinked read-only mode-0400 inode,
# live topology/profile equality, and dynamically contended policy locks.
write_attestation "$tmp/att-valid"
open_ro_unlinked "$tmp/att-valid"
valid_rc=0
valid_output=$(run_validate "$att_fd" "$token" 2>"$tmp/valid.err") || valid_rc=$?
if [ "$valid_rc" -eq 0 ] &&
   [[ $valid_output =~ ^CPUFREQ_ATTESTATION_READY\ version=1\ attestation_sha256=[0-9a-f]{64}\ live_profile_sha256=[0-9a-f]{64}\ requested_cpus=8,2\ canonical_selected_cpus=2,8\ logical_cpus=2,8,10,16\ policies=2\ target_khz=3800000$ ]]; then
    ok "an exact wrapper-v1 attestation and live profile are accepted"
else
    not_ok "an exact wrapper-v1 attestation and live profile are accepted"
fi
if ! rg -q 'valid_output=.*run_validate.*\|\| true' "$0"; then
    ok "positive control preserves and requires the validator exit status"
else
    not_ok "positive control preserves and requires the validator exit status"
fi
close_attestation

expect_fail_with "production-style validation requires the attestation FD" \
    'cpufreq_attestation_fd_missing' "$tmp/missing-fd" \
    env EXITOS_SCALE_TEST_MODE=1 EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
    bash "$script" --test-cpufreq-attestation-validate \
    "$sys" "$locks" 8,2 3800000

write_attestation "$tmp/att-linked"
exec {att_fd}<"$tmp/att-linked"
expect_fail_with "a still-linked attestation inode is refused" \
    'cpufreq_attestation_fd_not_unlinked' "$tmp/linked" \
    run_validate "$att_fd" "$token"
close_attestation
rm -f -- "$tmp/att-linked"

write_attestation "$tmp/att-rdwr"
chmod 0600 "$tmp/att-rdwr"
exec {att_fd}<>"$tmp/att-rdwr"
rm -f -- "$tmp/att-rdwr"
expect_fail_with "an O_RDWR attestation descriptor is refused" \
    'cpufreq_attestation_fd_not_read_only' "$tmp/rdwr" \
    run_validate "$att_fd" "$token"
close_attestation

write_attestation "$tmp/att-mode"
chmod 0600 "$tmp/att-mode"
open_ro_unlinked "$tmp/att-mode"
expect_fail_with "an attestation inode without exact mode 0400 is refused" \
    'cpufreq_attestation_fd_bad_metadata' "$tmp/mode" \
    run_validate "$att_fd" "$token"
close_attestation

write_attestation "$tmp/att-token"
open_ro_unlinked "$tmp/att-token"
expect_fail_with "the inherited token must authenticate the v1 header exactly" \
    'cpufreq_attestation_token_mismatch' "$tmp/token" \
    run_validate "$att_fd" ffffffffffffffffffffffffffffffff
close_attestation

write_attestation "$tmp/att-malicious"
chmod 0600 "$tmp/att-malicious"
printf '%s\n' '$(touch /tmp/EXIToS_ATTEST_MUST_NOT_EXECUTE)' \
    >>"$tmp/att-malicious"
chmod 0400 "$tmp/att-malicious"
open_ro_unlinked "$tmp/att-malicious"
expect_fail_with "extra shell-like attestation text is parsed as data and refused" \
    'cpufreq_attestation_shape_invalid' "$tmp/malicious" \
    run_validate "$att_fd" "$token"
expect_ok "malicious attestation text is never evaluated" \
    test ! -e /tmp/EXIToS_ATTEST_MUST_NOT_EXECUTE
close_attestation

write_attestation "$tmp/att-order"
open_ro_unlinked "$tmp/att-order"
expect_fail_with "requested CPU order must equal CPU_POOL order" \
    'cpufreq_requested_cpus_mismatch' "$tmp/order" \
    run_validate "$att_fd" "$token" 2,8
close_attestation

write_attestation "$tmp/att-closure"
chmod 0600 "$tmp/att-closure"
sed -i \
    -e 's/logical_cpus=2,8,10,16 policy_count=/logical_cpus=2,8,10 policy_count=/' \
    -e 's/logical_cpus=2:3800000,8:3800000,10:3800000,16:3800000 tolerance/logical_cpus=2:3800000,8:3800000,10:3800000 tolerance/' \
    -e 's/loaded_khz=2:3800000,8:3800000,10:3800000,16:3800000 concurrent/loaded_khz=2:3800000,8:3800000,10:3800000 concurrent/' \
    "$tmp/att-closure"
chmod 0400 "$tmp/att-closure"
open_ro_unlinked "$tmp/att-closure"
expect_fail_with "logical CPUs must be the complete online sibling closure" \
    'cpufreq_logical_cpus_mismatch' "$tmp/closure" \
    run_validate "$att_fd" "$token"
close_attestation

write_attestation "$tmp/att-scope"
chmod 0600 "$tmp/att-scope"
sed -i 's/related_cpus=8,16 /related_cpus=8 /' "$tmp/att-scope"
chmod 0400 "$tmp/att-scope"
open_ro_unlinked "$tmp/att-scope"
expect_fail_with "attested policy scope must equal live related_cpus" \
    'cpufreq_policy_scope_mismatch' "$tmp/scope" \
    run_validate "$att_fd" "$token"
close_attestation

write_attestation "$tmp/att-inode"
chmod 0600 "$tmp/att-inode"
sed -i "s/identity=$policy8_id /identity=1:1 /" "$tmp/att-inode"
chmod 0400 "$tmp/att-inode"
open_ro_unlinked "$tmp/att-inode"
expect_fail_with "attested policy identity must equal the live directory inode" \
    'cpufreq_policy_identity_mismatch' "$tmp/inode" \
    run_validate "$att_fd" "$token"
close_attestation

# policy02 is a different basename with the same numeric ID as policy2.  The
# consumer must reject the alias itself, not merely happen to sort it oddly.
mkdir "$sys/cpufreq/policy02"
for field in "$sys/cpufreq/policy8"/*; do
    cp -- "$field" "$sys/cpufreq/policy02/${field##*/}"
done
rm -f -- "$sys/cpu8/cpufreq" "$sys/cpu16/cpufreq"
ln -s ../cpufreq/policy02 "$sys/cpu8/cpufreq"
ln -s ../cpufreq/policy02 "$sys/cpu16/cpufreq"
alias_id=$(stat -Lc '%d:%i' "$sys/cpufreq/policy02")
write_attestation "$tmp/att-alias"
chmod 0600 "$tmp/att-alias"
sed -i \
    -e "s,path=$sys/cpufreq/policy8 identity=$policy8_id,path=$sys/cpufreq/policy02 identity=$alias_id," \
    "$tmp/att-alias"
chmod 0400 "$tmp/att-alias"
open_ro_unlinked "$tmp/att-alias"
expect_fail_with "policy basenames reject leading-zero numeric aliases" \
    'cpufreq_policy_name_not_canonical' "$tmp/alias" \
    run_validate "$att_fd" "$token"
close_attestation
rm -f -- "$sys/cpu8/cpufreq" "$sys/cpu16/cpufreq"
ln -s ../cpufreq/policy8 "$sys/cpu8/cpufreq"
ln -s ../cpufreq/policy8 "$sys/cpu16/cpufreq"

for drift in \
    cpuinfo_max_freq:3799000:cpufreq_cpuinfo_max_mismatch \
    scaling_governor:powersave:cpufreq_governor_mismatch \
    energy_performance_preference:balance_performance:cpufreq_epp_mismatch \
    scaling_min_freq:3700000:cpufreq_min_khz_mismatch \
    scaling_max_freq:3700000:cpufreq_max_khz_mismatch
do
    IFS=: read -r field bad_value diagnostic <<<"$drift"
    old_value=$(<"$sys/cpufreq/policy8/$field")
    printf '%s\n' "$bad_value" >"$sys/cpufreq/policy8/$field"
    write_attestation "$tmp/att-$field"
    open_ro_unlinked "$tmp/att-$field"
    expect_fail_with "live $field must match the exact maximum profile" \
        "$diagnostic" "$tmp/drift-$field" \
        run_validate "$att_fd" "$token"
    close_attestation
    printf '%s\n' "$old_value" >"$sys/cpufreq/policy8/$field"
done

write_attestation "$tmp/att-actual"
chmod 0600 "$tmp/att-actual"
sed -i 's/8:3800000/8:3600000/' "$tmp/att-actual"
chmod 0400 "$tmp/att-actual"
open_ro_unlinked "$tmp/att-actual"
expect_fail_with "the wrapper startup observation must be within its exact tolerance" \
    'cpufreq_attested_actual_mismatch' "$tmp/actual" \
    run_validate "$att_fd" "$token"
close_attestation

stop_holder
start_holder "$locks/exitos-cpufreq/policy2.lock" || exit 1
write_attestation "$tmp/att-unlocked"
open_ro_unlinked "$tmp/att-unlocked"
expect_fail_with "every attested policy lock must still be held by the wrapper" \
    'cpufreq_policy_lock_not_held' "$tmp/unlocked" \
    run_validate "$att_fd" "$token"
close_attestation
stop_holder
start_holder "$locks/exitos-cpufreq/policy2.lock" \
    "$locks/exitos-cpufreq/policy8.lock" || exit 1

# Force the policy-directory pathname to change after the validator has
# accepted its inode but before it reads the control fields.  A pathname-only
# consumer can accidentally combine the old attested identity with a new
# directory whose fields happen to look valid.
write_attestation "$tmp/att-policy-race"
open_ro_unlinked "$tmp/att-policy-race"
policy_pause=$tmp/policy-race.pause
policy_race_rc=0
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_CPUFREQ_PAUSE_POINT=policy-after-identity:policy8 \
    EXITOS_SCALE_TEST_CPUFREQ_PAUSE_MARKER="$policy_pause" \
    EXITOS_CPUFREQ_ATTEST_FD="$att_fd" \
    EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
    bash "$script" --test-cpufreq-attestation-validate \
    "$sys" "$locks" 8,2 3800000 \
    >"$tmp/policy-race.out" 2>"$tmp/policy-race.err" &
race_runner_pid=$!
policy_pause_ready=0
if wait_for_file_from_pid "$policy_pause" "$race_runner_pid"; then
    policy_pause_ready=1
    mv -- "$sys/cpufreq/policy8" "$sys/cpufreq/policy8-old"
    mkdir -- "$sys/cpufreq/policy8"
    make_policy policy8 8,16
    (umask 077; : >"$policy_pause.resume")
fi
wait "$race_runner_pid" || policy_race_rc=$?
race_runner_pid=
if [ "$policy_pause_ready" -eq 1 ]; then
    mv -- "$sys/cpufreq/policy8" "$tmp/policy8-raced"
    mv -- "$sys/cpufreq/policy8-old" "$sys/cpufreq/policy8"
fi
if [ "$policy_pause_ready" -eq 1 ] && [ "$policy_race_rc" -ne 0 ] &&
   grep -q cpufreq_policy_identity_mismatch "$tmp/policy-race.err"; then
    ok "policy replacement after identity validation is detected deterministically"
else
    not_ok "policy replacement after identity validation is detected deterministically"
    echo "# policy-race pause_ready=$policy_pause_ready rc=$policy_race_rc stderr=$(tr '\n' ' ' <"$tmp/policy-race.err")"
fi
close_attestation

# Likewise, replace a lock between lstat and open, then contend the replacement.
# Merely observing EWOULDBLOCK on the newly opened inode must not prove that the
# wrapper still owns the originally inspected lock object.
write_attestation "$tmp/att-lock-race"
open_ro_unlinked "$tmp/att-lock-race"
lock_pause=$tmp/lock-race.pause
lock_race_rc=0
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_CPUFREQ_PAUSE_POINT=lock-after-lstat:policy8 \
    EXITOS_SCALE_TEST_CPUFREQ_PAUSE_MARKER="$lock_pause" \
    EXITOS_CPUFREQ_ATTEST_FD="$att_fd" \
    EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
    bash "$script" --test-cpufreq-attestation-validate \
    "$sys" "$locks" 8,2 3800000 \
    >"$tmp/lock-race.out" 2>"$tmp/lock-race.err" &
race_runner_pid=$!
lock_pause_ready=0
race_lock_ready=0
if wait_for_file_from_pid "$lock_pause" "$race_runner_pid"; then
    lock_pause_ready=1
    mv -- "$locks/exitos-cpufreq/policy8.lock" \
        "$locks/exitos-cpufreq/policy8.lock-old"
    : >"$locks/exitos-cpufreq/policy8.lock"
    chmod 0600 "$locks/exitos-cpufreq/policy8.lock"
    "$lock_holder" "$tmp/race-lock-ready" \
        "$locks/exitos-cpufreq/policy8.lock" &
    race_holder_pid=$!
    if wait_for_file_from_pid "$tmp/race-lock-ready" "$race_holder_pid"; then
        race_lock_ready=1
    fi
    (umask 077; : >"$lock_pause.resume")
fi
wait "$race_runner_pid" || lock_race_rc=$?
race_runner_pid=
if [[ ${race_holder_pid:-} =~ ^[0-9]+$ ]]; then
    kill -TERM "$race_holder_pid" 2>/dev/null || true
    wait "$race_holder_pid" 2>/dev/null || true
fi
race_holder_pid=
if [ "$lock_pause_ready" -eq 1 ]; then
    mv -- "$locks/exitos-cpufreq/policy8.lock" "$tmp/policy8-raced.lock"
    mv -- "$locks/exitos-cpufreq/policy8.lock-old" \
        "$locks/exitos-cpufreq/policy8.lock"
fi
if [ "$lock_pause_ready" -eq 1 ] && [ "$race_lock_ready" -eq 1 ] &&
   [ "$lock_race_rc" -ne 0 ] &&
   grep -q cpufreq_policy_lock_identity_mismatch "$tmp/lock-race.err"; then
    ok "lock replacement between lstat and open is detected deterministically"
else
    not_ok "lock replacement between lstat and open is detected deterministically"
    echo "# lock-race pause_ready=$lock_pause_ready replacement_contended=$race_lock_ready rc=$lock_race_rc stderr=$(tr '\n' ' ' <"$tmp/lock-race.err")"
fi
close_attestation

run_cell_test()
{
    local fd=$1 command=$2 tag=$3
    env EXITOS_SCALE_TEST_MODE=1 \
        EXITOS_SCALE_TEST_CPU_WINDOW_HELPER="${EXITOS_SCALE_TEST_CPU_WINDOW_HELPER:-$cpu_window_helper}" \
        EXITOS_CPUFREQ_ATTEST_FD="$fd" \
        EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
        EXPECTED_CPU_WINDOW="${EXPECTED_CPU_WINDOW:-}" \
        EXPECTED_PRE="${EXPECTED_PRE:-}" \
        EXPECTED_POST="${EXPECTED_POST:-}" \
        EXPECTED_ATTEST_ID="${EXPECTED_ATTEST_ID:-}" \
        CHILD_MARKER="${CHILD_MARKER:-}" \
        bash "$script" --test-cpufreq-attestation-cell \
        "$sys" "$locks" 8,2 3800000 \
        "$tmp/$tag.meta" "$tmp/$tag.pre" "$tmp/$tag.post" \
        "$tmp/$tag.summary" "$command"
}

cpu_window_helper=$tmp/cpu-window-helper
cat >"$cpu_window_helper" <<'EOF'
#!/bin/bash
set -eu
[ "$#" -eq 1 ]
[ ! -e "$1" ]
(umask 077; : >"$1")
EOF
chmod 0700 "$cpu_window_helper"

child_check=$tmp/child-check
cat >"$child_check" <<'EOF'
#!/bin/bash
set -eu
[ -f "$EXPECTED_CPU_WINDOW" ]
[ -f "$EXPECTED_PRE" ]
[ ! -e "$EXPECTED_POST" ]
[ -z "${EXITOS_CPUFREQ_ATTEST_FD+x}" ]
[ -z "${EXITOS_CPUFREQ_ATTEST_TOKEN+x}" ]
for descriptor in /proc/self/fd/*; do
    identity=$(stat -Lc '%d:%i' "$descriptor" 2>/dev/null || true)
    [ "$identity" != "$EXPECTED_ATTEST_ID" ] || exit 91
done
printf '%s\n' child-clean >"$CHILD_MARKER"
EOF
chmod 0700 "$child_check"

# Idle/downclock values are evidence only.  One missing file and one far-below
# value must be recorded at both boundaries without invalidating exact control
# fields or the campaign.
printf '%s\n' 3100000 >"$sys/cpufreq/policy2/scaling_cur_freq"
rm -f -- "$sys/cpufreq/policy8/scaling_cur_freq"
write_attestation "$tmp/att-cell-ok"
open_ro_unlinked "$tmp/att-cell-ok"
att_identity=$(stat -Lc '%d:%i' "/proc/$$/fd/$att_fd")
expected_att_sha=$(sha256sum -- "/proc/$$/fd/$att_fd" | awk '{print $1}')
expected_live_sha=$(expected_profile_sha)
cell_ok_rc=0
EXITOS_SCALE_TEST_CPU_WINDOW_HELPER="$cpu_window_helper" \
EXPECTED_CPU_WINDOW="$tmp/cell-ok.meta.cpu-window" \
EXPECTED_PRE="$tmp/cell-ok.pre" EXPECTED_POST="$tmp/cell-ok.post" \
EXPECTED_ATTEST_ID="$att_identity" CHILD_MARKER="$tmp/cell-ok.child" \
    run_cell_test "$att_fd" "$child_check" cell-ok \
    >"$tmp/cell-ok.out" 2>"$tmp/cell-ok.err" || cell_ok_rc=$?
if [ "$cell_ok_rc" -eq 0 ] && [ -f "$tmp/cell-ok.child" ] &&
   [ -f "$tmp/cell-ok.summary" ] &&
   grep -qx "cpufreq_attestation_sha256=$expected_att_sha" "$tmp/cell-ok.meta" &&
   grep -qx "cpufreq_live_profile_sha256=$expected_live_sha" "$tmp/cell-ok.meta" &&
   grep -q '^frequency_claim=boundary_observation_only_not_continuous_hardware_frequency$' "$tmp/cell-ok.pre" &&
   grep -q 'boundary_actual_khz=.*2:3100000' "$tmp/cell-ok.pre" &&
   grep -q 'boundary_actual_khz=.*8:unavailable' "$tmp/cell-ok.pre" &&
   grep -q '^frequency_claim=boundary_observation_only_not_continuous_hardware_frequency$' "$tmp/cell-ok.post"; then
    ok "shared cell gates order the CPU window/pre/I/O/post path, retain exact hashes, and strip the child"
else
    not_ok "shared cell gates order the CPU window/pre/I/O/post path, retain exact hashes, and strip the child"
fi
close_attestation
printf '%s\n' 3800000 >"$sys/cpufreq/policy2/scaling_cur_freq"
printf '%s\n' 3800000 >"$sys/cpufreq/policy8/scaling_cur_freq"

post_drift=$tmp/post-drift-command
cat >"$post_drift" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' powersave >"$DRIFT_POLICY/scaling_governor"
printf '%s\n' ran >"$DRIFT_COMMAND_MARKER"
EOF
chmod 0700 "$post_drift"
write_attestation "$tmp/att-post-drift"
open_ro_unlinked "$tmp/att-post-drift"
post_drift_rc=0
DRIFT_POLICY="$sys/cpufreq/policy8" \
DRIFT_COMMAND_MARKER="$tmp/post-drift.ran" \
    run_cell_test "$att_fd" "$post_drift" post-drift \
    >"$tmp/post-drift.out" 2>"$tmp/post-drift.err" || post_drift_rc=$?
if [ "$post_drift_rc" -ne 0 ] && [ -f "$tmp/post-drift.ran" ] &&
   [ ! -e "$tmp/post-drift.summary" ] &&
   grep -q cpufreq_governor_mismatch "$tmp/post-drift.err"; then
    ok "post-cell control drift is refused before summary"
else
    not_ok "post-cell control drift is refused before summary"
fi
close_attestation
printf '%s\n' performance >"$sys/cpufreq/policy8/scaling_governor"

pre_command=$tmp/pre-command
cat >"$pre_command" <<'EOF'
#!/bin/bash
printf '%s\n' should-not-run >"$PRE_COMMAND_MARKER"
EOF
chmod 0700 "$pre_command"
write_attestation "$tmp/att-pre-drift"
open_ro_unlinked "$tmp/att-pre-drift"
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_PAUSE_AFTER_CPUFREQ_INIT=1 \
    EXITOS_SCALE_TEST_CPU_WINDOW_HELPER="$cpu_window_helper" \
    EXITOS_CPUFREQ_ATTEST_FD="$att_fd" \
    EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
    PRE_COMMAND_MARKER="$tmp/pre-drift.ran" \
    bash "$script" --test-cpufreq-attestation-cell \
    "$sys" "$locks" 8,2 3800000 \
    "$tmp/pre-drift.meta" "$tmp/pre-drift.pre" "$tmp/pre-drift.post" \
    "$tmp/pre-drift.summary" "$pre_command" \
    >"$tmp/pre-drift.out" 2>"$tmp/pre-drift.err" &
pre_runner=$!
pre_ready=0
for _ in $(seq 1 200); do
    if grep -qx CPUFREQ_TEST_INIT_READY "$tmp/pre-drift.out" 2>/dev/null; then
        pre_ready=1
        break
    fi
    kill -0 "$pre_runner" 2>/dev/null || break
    sleep 0.01
done
if [ "$pre_ready" -eq 1 ]; then
    printf '%s\n' 3700000 >"$sys/cpufreq/policy8/scaling_min_freq"
    kill -CONT "$pre_runner" 2>/dev/null || true
fi
pre_rc=0
wait "$pre_runner" || pre_rc=$?
if [ "$pre_ready" -eq 1 ] && [ "$pre_rc" -ne 0 ] &&
   [ ! -e "$tmp/pre-drift.ran" ] && [ ! -e "$tmp/pre-drift.summary" ] &&
   grep -q cpufreq_min_khz_mismatch "$tmp/pre-drift.err"; then
    ok "pre-cell control drift is refused before the command"
else
    kill -CONT "$pre_runner" 2>/dev/null || true
    kill -KILL "$pre_runner" 2>/dev/null || true
    not_ok "pre-cell control drift is refused before the command"
fi
close_attestation
printf '%s\n' 3800000 >"$sys/cpufreq/policy8/scaling_min_freq"

fio_fail=$tmp/fio-fail
cat >"$fio_fail" <<'EOF'
#!/bin/bash
printf '%s\n' attempted >"$FIO_FAIL_MARKER"
exit 7
EOF
chmod 0700 "$fio_fail"
write_attestation "$tmp/att-fio-fail"
open_ro_unlinked "$tmp/att-fio-fail"
fio_fail_rc=0
FIO_FAIL_MARKER="$tmp/fio-fail.ran" \
    run_cell_test "$att_fd" "$fio_fail" fio-fail \
    >"$tmp/fio-fail.out" 2>"$tmp/fio-fail.err" || fio_fail_rc=$?
if [ "$fio_fail_rc" -ne 0 ] && [ -f "$tmp/fio-fail.ran" ] &&
   [ -f "$tmp/fio-fail.post" ] && [ ! -e "$tmp/fio-fail.summary" ] &&
   grep -q 'test_cell_command_rc=7' "$tmp/fio-fail.err"; then
    ok "a failing fio-equivalent command still runs the post-frequency gate first"
else
    not_ok "a failing fio-equivalent command still runs the post-frequency gate first"
fi
close_attestation

# Execute the production-shared generation fragment against the fake profile
# and compare both digests independently.  This replaces source-text greps as
# the primary evidence that campaign generation retains the attestation state.
write_attestation "$tmp/att-generation"
open_ro_unlinked "$tmp/att-generation"
generation_att_sha=$(sha256sum -- "/proc/$$/fd/$att_fd" | awk '{print $1}')
generation_live_sha=$(expected_profile_sha)
generation_rc=0
generation_output=$(env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_CPUFREQ_ATTEST_FD="$att_fd" \
    EXITOS_CPUFREQ_ATTEST_TOKEN="$token" \
    bash "$script" --test-cpufreq-attestation-generation \
    "$sys" "$locks" 8,2 3800000 \
    2>"$tmp/generation.err") || generation_rc=$?
expected_generation=$(printf '%s\n' \
    "CPUFREQ_ATTESTATION_READY version=1 attestation_sha256=$generation_att_sha live_profile_sha256=$generation_live_sha requested_cpus=8,2 canonical_selected_cpus=2,8 logical_cpus=2,8,10,16 policies=2 target_khz=3800000" \
    "cpufreq_attestation_sha256=$generation_att_sha" \
    "cpufreq_live_profile_sha256=$generation_live_sha" \
    'cpufreq_target_khz=3800000' \
    'cpufreq_logical_cpus=2,8,10,16')
if [ "$generation_rc" -eq 0 ] &&
   [ "$generation_output" = "$expected_generation" ]; then
    ok "campaign generation dynamically retains the independently computed attestation and live-profile hashes"
else
    not_ok "campaign generation dynamically retains the independently computed attestation and live-profile hashes"
fi
close_attestation

# The same mode gate used by the production entry point is exercised directly:
# only the two write-capable campaigns require the authenticated retained FD.
expect_fail_with "endpoint pilot mode fails closed without an attestation FD" \
    'cpufreq_attestation_fd_missing' "$tmp/mode-endpoints" \
    env -u EXITOS_CPUFREQ_ATTEST_FD -u EXITOS_CPUFREQ_ATTEST_TOKEN \
    EXITOS_SCALE_TEST_MODE=1 bash "$script" \
    --test-cpufreq-mode-requirement --pilot-endpoints \
    "$sys" "$locks" 8,2 3800000

expect_fail_with "factorial pilot mode fails closed without an attestation FD" \
    'cpufreq_attestation_fd_missing' "$tmp/mode-factorial" \
    env -u EXITOS_CPUFREQ_ATTEST_FD -u EXITOS_CPUFREQ_ATTEST_TOKEN \
    EXITOS_SCALE_TEST_MODE=1 bash "$script" \
    --test-cpufreq-mode-requirement --pilot-factorial \
    "$sys" "$locks" 8,2 3800000

plan_mode_rc=0
plan_mode_output=$(env -u EXITOS_CPUFREQ_ATTEST_FD \
    -u EXITOS_CPUFREQ_ATTEST_TOKEN EXITOS_SCALE_TEST_MODE=1 \
    bash "$script" --test-cpufreq-mode-requirement --plan \
    "$sys" "$locks" 8,2 3800000 \
    2>"$tmp/mode-plan.err") || plan_mode_rc=$?
if [ "$plan_mode_rc" -eq 0 ] &&
   [ "$plan_mode_output" = 'CPUFREQ_MODE_BYPASS mode=--plan' ]; then
    ok "read-only plan mode dynamically bypasses the write-campaign attestation requirement"
else
    not_ok "read-only plan mode dynamically bypasses the write-campaign attestation requirement"
fi

echo "1..$n  ($bad failed)"
exit "$bad"
