#!/bin/bash
# Static contract for the real-NVMe scaling pilot.  Dynamic device identity and
# counter checks are repeated by the runner itself on the target host; this test
# ensures future edits cannot silently remove the gates or revive the old
# process-stats/CPU-placement mistakes.
set -u

script=tools/fio-scale-pilot.sh
preload=src/preload.c
frontend_config=src/frontend_config.c
tmp=$(mktemp -d /tmp/exitos-fio-scale-pilot-test.XXXXXX) || exit 1
trap 'rm -rf -- "$tmp"' EXIT
n=0
bad=0
ok() { n=$((n + 1)); echo "ok $n - $1"; }
not_ok() { n=$((n + 1)); bad=$((bad + 1)); echo "not ok $n - $1"; }
has() { rg -q -- "$1" "$script" && ok "$2" || not_ok "$2"; }
expect_ok()
{
    local label=$1
    shift
    if "$@"; then ok "$label"; else not_ok "$label"; fi
}
expect_fail()
{
    local label=$1
    shift
    if "$@"; then not_ok "$label"; else ok "$label"; fi
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

if [ -x "$script" ]; then ok "pilot runner exists and is executable"; else not_ok "pilot runner exists and is executable"; fi
if [ -r "$script" ] && bash -n "$script"; then ok "pilot runner parses as bash"; else not_ok "pilot runner parses as bash"; fi
has 'EXAMPLESERIAL0001' "target serial is an exact constant"
has 'eui\.00000000000000000000000000000000' "target WWID is an exact constant"
has '0000:01:00\.0' "target PCI function is an exact constant"
has 'start=2048' "temporary partition start is verified"
has 'count=419430400' "temporary partition length is verified"
has '00000000-0000-0000-0000-000000000000' "current filesystem generation UUID is verified"
has 'stats-%p' "process workers use PID-distinct statistics files"
has 'cpus_allowed_policy=split' "fio jobs are split across pinned CPUs"
has '8,9,10,11,12,13,14,15' "default jobs use the placeholder pinned core map"
has 'CPU_UTIL_MAX' "legacy CPU-util setting is treated as a recorded reference"
has 'QUIET_SAMPLES' "a campaign requires a sustained quiet window before timing"
has 'fuser -mM' "each cell refuses a foreign filesystem user before writes"
has 'fuser.*"\$DEV".*"\$PART"' "each cell refuses foreign raw-device openers"
has '/sys/block/\$BASE/stat' "each cell proves target-disk writes are idle before timing"
has 'CPU_UTIL_MAX.*15' "selected CPUs and SMT siblings record the 15 percent reference"
has 'thread_siblings' "idle gate covers each selected CPU and its SMT sibling"
has 'IO_NICE=-20' "I/O uses the highest normal nice priority"
has 'os\.SCHED_OTHER' "I/O explicitly remains in SCHED_OTHER"
has 'EXITOS_STRICT' "strict-mode environment is explicitly removed"
has 'EXITOS_STAGE' "staging environment is explicitly removed"
has 'fast_write' "runner validates takeover counts, not registrations alone"
has 'cpu_us_per_io' "runner reports an Amdahl CPU-time budget per I/O"
has 'output-format=json' "full fio JSON is retained"
has 'EXITOS_SCALE_VARIANTS_DIR' "campaign requires one validated four-artifact directory"
has 'validate_scale_variants' "runner validates source snapshot, membership, and artifacts before timing"
has 'PRE_CELL_IDENTITY_OK' "identity is rechecked immediately before each cell"
has 'local jobs last cpu sib' "CPU-map arithmetic initializes jobs before set-u expansion"

# Content hashes identify a frozen milestone; they must not be recomputed for
# every benchmark cell.  Cell boundaries use frozen object identities and
# exact field comparisons instead.
expect_ok "per-cell object gates never recompute content hashes" \
    bash -c '
        for function in assert_scale_variants verify_workset \
                        exec_bound_objects campaign_generation_snapshot \
                        assert_generation_value; do
            body=$(sed -n "/^${function}()/,/^}$/p" "$1")
            test -n "$body" || exit 1
            ! grep -Eq "sha256sum|hashlib\\.sha256|file_sha256_required" \
                <<<"$body" || exit 1
        done
        cell=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        ! grep -Eq "sha256sum|hashlib\\.sha256|file_sha256_required" \
            <<<"$cell"
    ' bash "$script"
expect_ok "per-cell evidence compares exact records without hashing them" \
    bash -c '
        body=$(sed -n '\''/^cell_evidence_python()/,/^}$/p'\'' "$1")
        test -n "$body" &&
        ! grep -Eq "hashlib|sha256" <<<"$body"
    ' bash "$script"

# Result artifacts are never allowed to fall back to a broad directory below
# /root.  Exercise the production-mode requirement through a test-only entry
# which calls the same validator but cannot reach any device or create output.
if rg -q 'EXITOS_SCALE_RESULT_ROOT:-/root/exitos-results' "$script"; then
    not_ok "production has no implicit /root/exitos-results fallback"
else
    ok "production has no implicit /root/exitos-results fallback"
fi
expect_fail_with "a production pilot requires an explicitly supplied result root" \
    'EXITOS_SCALE_RESULT_ROOT_missing.*mode=--pilot-factorial' \
    "$tmp/result-root-missing" \
    env -u EXITOS_SCALE_RESULT_ROOT EXITOS_SCALE_TEST_MODE=1 \
        bash "$script" --test-result-root-required --pilot-factorial
expect_fail_with "a production pilot rejects a relative explicit result root" \
    'result_root_not_absolute=relative-results' \
    "$tmp/result-root-relative" \
    env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_RESULT_ROOT=relative-results \
        bash "$script" --test-result-root-required --pilot-factorial

# Use a fake privileged home and project workspace so this contract is not
# coupled to the machine running the unit suite.  A direct child of the broad
# home is forbidden; an explicit nested project artifact directory is valid.
fake_root_home=$tmp/fake-root-home
fake_project_root=$fake_root_home/project
mkdir -p "$fake_project_root"
expect_fail_with "a direct child of the broad root home is rejected" \
    'result_root_is_direct_root_child=' "$tmp/result-root-direct-child" \
    env EXITOS_SCALE_TEST_MODE=1 \
        EXITOS_SCALE_TEST_PROTECTED_ROOT_HOME="$fake_root_home" \
        bash "$script" --test-result-root-policy \
        "$fake_root_home/factorial-results"
expect_fail_with "a noncanonical spelling cannot disguise a broad-root child" \
    'result_root_not_canonical=' "$tmp/result-root-dotdot" \
    env EXITOS_SCALE_TEST_MODE=1 \
        EXITOS_SCALE_TEST_PROTECTED_ROOT_HOME="$fake_root_home" \
        bash "$script" --test-result-root-policy \
        "$fake_project_root/../sibling-results"
expect_ok "an explicit result root nested below the project workspace is accepted" \
    env EXITOS_SCALE_TEST_MODE=1 \
        EXITOS_SCALE_TEST_PROTECTED_ROOT_HOME="$fake_root_home" \
        bash "$script" --test-result-root-policy \
        "$fake_project_root/performance-results"

# The real --plan dispatcher used to create both a result directory and a
# coordination lock before later validation failed.  Deliberately stop it at
# an invalid variants directory and prove that even this path has zero writes
# below the supplied result/lock sandbox.  This is a dynamic production-entry
# check, not a grep-only claim.
plan_fakebin=$tmp/plan-fakebin
plan_lock_root=$tmp/plan-lock-root
plan_result_root=$fake_project_root/plan-must-not-exist
mkdir -m 0700 "$plan_fakebin" "$plan_lock_root"
ln -s /bin/true "$plan_fakebin/fio"
ln -s /bin/true "$plan_fakebin/nvme"
cat >"$plan_fakebin/mktemp" <<'EOF'
#!/bin/bash
printf '%s\n' MKTEMP_CALLED >>"$EXITOS_TEST_MKTEMP_LOG"
exec /usr/bin/mktemp "$@"
EOF
chmod 0700 "$plan_fakebin/mktemp"
plan_mktemp_log=$tmp/plan-mktemp-called
plan_before=$(find "$fake_project_root" "$plan_lock_root" -mindepth 1 \
    -printf '%p:%y\n' | sort)
plan_rc=0
env PATH="$plan_fakebin:/usr/bin:/bin" EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_TEST_MKTEMP_LOG="$plan_mktemp_log" \
    EXITOS_SCALE_TEST_LOCK_ROOT="$plan_lock_root" \
    EXITOS_SCALE_RESULT_ROOT="$plan_result_root" \
    EXITOS_SCALE_VARIANTS_DIR="$tmp/intentionally-missing-variants" \
    bash "$script" --plan >"$tmp/plan-ro.out" 2>"$tmp/plan-ro.err" || plan_rc=$?
plan_after=$(find "$fake_project_root" "$plan_lock_root" -mindepth 1 \
    -printf '%p:%y\n' | sort)
expect_ok "--plan creates no result directory, pilot directory, or coordination lock" \
    bash -c 'test "$1" -ne 0 -a ! -e "$2" -a ! -e "$3" -a "$4" = "$5"' \
        bash "$plan_rc" "$plan_result_root" "$plan_mktemp_log" \
        "$plan_before" "$plan_after"
expect_ok "the --plan branch contains no writable artifact helper" \
    bash -c '
        plan=$(sed -n '\''/if \[ "\$1" = --plan \]/,/^fi$/p'\'' "$1")
        ! grep -Eq "RESULT_DIR|prepare_result_root|mktemp|cpu_quiet_window" <<<"$plan" &&
        grep -q "cpu_observation_stdout" <<<"$plan"
    ' bash "$script"

# A successful pure-fake invocation of the real --plan mode must also be
# observational.  Hostile write helpers log any accidental call.
plan_success_fakebin=$tmp/plan-success-fakebin
plan_success_log=$tmp/plan-success-write.log
plan_success_result=$fake_project_root/plan-success-must-not-exist
plan_success_lock=$tmp/plan-success-lock-must-not-exist
mkdir -m 0700 "$plan_success_fakebin"
for helper in mkdir mktemp flock; do
    cat >"$plan_success_fakebin/$helper" <<'EOF'
#!/bin/bash
printf '%s\n' "$(basename "$0") $*" >>"$EXITOS_TEST_PLAN_WRITE_LOG"
exit 97
EOF
    chmod 0700 "$plan_success_fakebin/$helper"
done
plan_success_rc=0
env PATH="$plan_success_fakebin:/usr/bin:/bin" EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_TEST_PLAN_WRITE_LOG="$plan_success_log" \
    EXITOS_SCALE_RESULT_ROOT="$plan_success_result" \
    EXITOS_SCALE_TEST_LOCK_ROOT="$plan_success_lock" \
    bash "$script" --test-production-plan test-pure-plan \
    >"$tmp/plan-success.out" \
    2>"$tmp/plan-success.err" || plan_success_rc=$?
expect_ok "successful pure-fake --plan creates no result, lock, or helper side effect" \
    bash -c '
        test "$1" -eq 0 -a ! -e "$2" -a ! -e "$3" -a ! -e "$4" &&
        grep -q "PLAN_OK .*arm=uringpoll .*poll_required=yes" "$5"
    ' bash "$plan_success_rc" "$plan_success_result" "$plan_success_lock" \
        "$plan_success_log" "$tmp/plan-success.out"
expect_ok "successful plan test uses the production plan function, not a bypass" \
    bash -c '! rg -q EXITOS_SCALE_TEST_PURE_PLAN "$1"' bash "$script"

# P0 regression: work files must be created through a retained, owned 0700
# directory and O_EXCL/O_NOFOLLOW.  The test-only entry exercises the same
# helper as --pilot-endpoints; it has no device, mount, or fio dependency.
clean=$tmp/clean
mkdir -m 0700 "$clean"
if EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$clean" \
        >"$tmp/clean.out" 2>"$tmp/clean.err"; then
    clean_ok=1
    for i in $(seq 0 7); do
        meta=$(stat -c '%F:%u:%a:%h:%s' "$clean/w$i.log" 2>/dev/null || true)
        [ "$meta" = "regular empty file:$(id -u):600:1:0" ] || clean_ok=0
    done
    [ "$clean_ok" -eq 1 ] && ok "work-file initializer creates only owned 0600 single-link regular files" ||
        not_ok "work-file initializer creates only owned 0600 single-link regular files"
else
    not_ok "work-file initializer creates only owned 0600 single-link regular files"
fi

sentinel=$tmp/sentinel
printf '%s\n' DO_NOT_TOUCH >"$sentinel"
symlink_dir=$tmp/symlink
mkdir -m 0700 "$symlink_dir"
ln -s "$sentinel" "$symlink_dir/w0.log"
expect_fail "work-file initializer refuses a pre-existing symlink" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$symlink_dir"
expect_ok "refused symlink target is unchanged" \
    grep -qx DO_NOT_TOUCH "$sentinel"

hardlink_dir=$tmp/hardlink
mkdir -m 0700 "$hardlink_dir"
ln "$sentinel" "$hardlink_dir/w0.log"
expect_fail "work-file initializer refuses a pre-existing hardlink" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$hardlink_dir"
expect_ok "refused hardlink target is unchanged" \
    grep -qx DO_NOT_TOUCH "$sentinel"

if [ "$(id -u)" -eq 0 ]; then
    nonowned_dir=$tmp/nonowned
    mkdir -m 0700 "$nonowned_dir"
    : >"$nonowned_dir/w0.log"
    chown 65534:65534 "$nonowned_dir/w0.log"
    expect_fail "work-file initializer refuses a non-owned file" \
        env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$nonowned_dir"
else
    ok "work-file initializer non-owner case requires root fixture (skipped)"
fi

# A path-only check has a TOCTOU window.  Pause after retention, replace the
# pathname, then ensure creation continues in the retained directory object.
race=$tmp/race
race_old=$tmp/race-retained
mkdir -m 0700 "$race"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_AFTER_RETAIN=1 \
    bash "$script" --test-workfile-init "$race" \
    >"$tmp/race.out" 2>"$tmp/race.err" & race_pid=$!
race_ready=0
for _ in $(seq 1 200); do
    if grep -qx WORKDIR_RETAINED_READY "$tmp/race.out" 2>/dev/null; then
        race_ready=1
        break
    fi
    kill -0 "$race_pid" 2>/dev/null || break
    sleep 0.01
done
race_rc=0
if [ "$race_ready" -eq 1 ]; then
    mv "$race" "$race_old"
    mkdir -m 0700 "$race"
    ln -s "$sentinel" "$race/w0.log"
    kill -CONT "$race_pid" 2>/dev/null || true
fi
wait "$race_pid" || race_rc=$?
expect_ok "work-file creation is rooted at the retained directory, not a replaced pathname" \
    bash -c 'test "$1" -eq 1 -a "$2" -eq 0 -a -f "$3/w0.log" -a ! -L "$3/w0.log" -a -L "$4/w0.log" && grep -qx DO_NOT_TOUCH "$5"' \
        bash "$race_ready" "$race_rc" "$race_old" "$race" "$sentinel"

has 'flock -n' "one campaign holds a nonblocking coordination lock"
has 'EXITOS_DEV="\$PART"' "the production registration guard is scoped to the exact filesystem partition"
if rg -q 'EXITOS_WINDOW_IN_PART="\$PART"' "$script"; then
    not_ok "runner does not present the unused WINDOW_IN_PART variable as a production guard"
else
    ok "runner does not present the unused WINDOW_IN_PART variable as a production guard"
fi
has 'mktemp -d "\$MNT/\.exitos-scale-work\.XXXXXXXX"' \
    "production creates a fresh per-campaign work directory"
has 'secure_init_workfiles "\$WORK_FD_PATH"' \
    "production initialization uses the O_EXCL retained-directory helper"
has 'exec_bound_objects unsealed' \
    "prepare binds exact workload leaves before fio opens them"
if rg -q -- '--directory="\$MNT"' "$script"; then
    not_ok "no production fio command writes w*.log through the shared mount root"
else
    ok "no production fio command writes w*.log through the shared mount root"
fi
has '--pilot-endpoints' \
    "one integrated endpoint command retains its work directory across prepare and timing"

# The private directory prevents an unprivileged rename race, while this
# manifest catches accidental/root-level replacement between cells.  Same
# name, size, mode and owner with a new inode must still be rejected.
workset=$tmp/workset
mkdir -m 0700 "$workset"
EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$workset" \
    >"$tmp/workset-init.out" 2>"$tmp/workset-init.err" || true
for i in $(seq 0 7); do truncate -s 4096 "$workset/w$i.log"; done
seal_out=""
if seal_out=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" \
        --test-workset-seal "$workset" TEST-GENERATION 4096 \
        2>"$tmp/workset-seal.err") &&
   [[ $seal_out =~ WORKSET_SEALED[[:space:]]identity=([0-9]+:[0-9]+:[0-9]+:[0-7]+:[0-9]+:[0-9]+:[0-9]+:[0-9]+) ]]; then
    workset_identity=${BASH_REMATCH[1]}
    ok "workset seal freezes the manifest object identity"
else
    workset_identity=missing
    not_ok "workset seal freezes the manifest object identity"
fi
expect_ok "untouched workset passes exact manifest verification" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workset-verify \
        "$workset" TEST-GENERATION 4096 "$workset_identity"
mv "$workset/w0.log" "$workset/w0.old"
: >"$workset/w0.log"
chmod 0600 "$workset/w0.log"
truncate -s 4096 "$workset/w0.log"
expect_fail_with "same-name same-size replacement inode is rejected" \
    'workfile identity changed: w0.log' "$tmp/replaced-inode" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workset-verify \
        "$workset" TEST-GENERATION 4096 "$workset_identity"
rm -f "$workset/w0.log"
mv "$workset/w0.old" "$workset/w0.log"
expect_fail_with "workset prepared under another campaign generation is rejected" \
    'workset manifest generation/shape mismatch' "$tmp/wrong-workset-generation" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workset-verify \
        "$workset" OTHER-GENERATION 4096 "$workset_identity"

has 'verify_workset pre-cell' "every timed cell verifies the exact workset before writing"
has 'verify_workset post-cell' "every timed cell verifies the exact workset after writing"

# Generation comparison itself is tested with an injected read-only snapshot;
# production builds the snapshot from boot/device/controller/mount/CPU sysfs.
generation=$tmp/generation
printf '%s\n' alpha >"$generation"
expect_ok "an unchanged frozen generation is accepted" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-generation-freeze "$generation"
printf '%s\n' alpha >"$generation"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_AFTER_GENERATION=1 \
    bash "$script" --test-generation-freeze "$generation" \
    >"$tmp/generation.out" 2>"$tmp/generation.err" & generation_pid=$!
generation_ready=0
for _ in $(seq 1 200); do
    if grep -qx CAMPAIGN_GENERATION_FROZEN "$tmp/generation.out" 2>/dev/null; then
        generation_ready=1
        break
    fi
    kill -0 "$generation_pid" 2>/dev/null || break
    sleep 0.01
done
generation_rc=0
if [ "$generation_ready" -eq 1 ]; then
    printf '%s\n' beta >"$generation"
    kill -CONT "$generation_pid" 2>/dev/null || true
fi
wait "$generation_pid" || generation_rc=$?
expect_ok "a generation change after freeze is refused" \
    bash -c 'test "$1" -eq 1 -a "$2" -ne 0 && grep -q generation_drift "$3"' \
        bash "$generation_ready" "$generation_rc" "$tmp/generation.err"
has 'boot_id=' "generation includes the boot ID"
has 'diskseq=' "generation includes the namespace diskseq"
has 'whole_dev_t=' "generation includes the whole-device dev_t"
has 'part_dev_t=' "generation includes the partition dev_t"
has 'controller_path=' "generation includes the canonical controller path"
has 'mount_id=' "generation includes the exact mount generation"
has 'cpu_topology_' "generation includes the selected CPU topology generation"
has 'assert_campaign_generation' "identity gates compare every cell with the frozen generation"

# Poll capability and the complete current mq map are authoritative generation
# state.  IRQ identity/affinity is diagnostic only and cannot gate a cell.
expect_ok "generation freezes poll support and every hardware queue CPU list without IRQ gates" \
    bash -c '
        body=$(sed -n '\''/^campaign_generation_snapshot()/,/^}$/p'\'' "$1")
        grep -q "poll_queues=" <<<"$body" &&
        grep -q "io_poll=" <<<"$body" &&
        grep -q "mq_generation_" <<<"$body" &&
        ! grep -Eq "msi_irqs|effective_affinity|irq_generation" <<<"$body"
    ' bash "$script"

# Opener gate injection: the fake records its arguments and can report the
# generic namespace character node busy.  A usage failure cannot satisfy these
# assertions because both the clean marker and the exact refusal are required.
fakebin=$tmp/fakebin
mkdir "$fakebin"
opener_char=$tmp/fake-ng
mknod -m 0600 "$opener_char" c 1 9
cat >"$fakebin/fuser" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >>"$FAKE_FUSER_LOG"
if [[ " $* " == *" $FAKE_CHAR_PATH "* ]]; then
    [ -n "${FAKE_CHAR_FD:-}" ] || exit 91
    [ ! -e "/proc/self/fd/$FAKE_CHAR_FD" ] || {
        printf '%s\n' inherited-char-fd >&2
        exit 92
    }
    printf '%s\n' "$FAKE_RETAINING_PID"
    [ "${FAKE_CHAR_BUSY:-0}" != 1 ] || printf '%s\n' 4242
elif [ -n "${FAKE_BUSY_AFTER_MARKER:-}" ] &&
     [ -e "$FAKE_BUSY_AFTER_MARKER" ] &&
     [[ " $* " == *" /dev/fake-whole /dev/fake-p1 /dev/fake-ctrl "* ]]; then
    printf '%s\n' 4242
fi
EOF
chmod 0700 "$fakebin/fuser"
: >"$tmp/fuser.log"
expect_ok "clean opener gate checks filesystem, block, p1, generic-char and controller-char" \
    env PATH="$fakebin:$PATH" FAKE_FUSER_LOG="$tmp/fuser.log" \
        FAKE_CHAR_PATH="$opener_char" \
        EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-opener-gate \
        /fake/mnt /dev/fake-whole /dev/fake-p1 "$opener_char" /dev/fake-ctrl
expect_ok "fake fuser observed all four target device objects" \
    bash -c 'grep -q -- "-mM /fake/mnt" "$1" && grep -q "/dev/fake-whole /dev/fake-p1 /dev/fake-ctrl" "$1" && grep -Fqx "$2" "$1"' \
        bash "$tmp/fuser.log" "$opener_char"
expect_fail_with "busy generic-char opener is refused by the production helper" \
    'foreign_raw_openers=.*4242' "$tmp/busy-char" \
    env PATH="$fakebin:$PATH" FAKE_FUSER_LOG="$tmp/fuser.log" FAKE_CHAR_BUSY=1 \
        FAKE_CHAR_PATH="$opener_char" \
        EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-opener-gate \
        /fake/mnt /dev/fake-whole /dev/fake-p1 "$opener_char" /dev/fake-ctrl
has 'exec \{char_probe_fd\}<&-' \
    "fuser child closes its inherited generic-char descriptor before probing"

cat >"$tmp/post-opener-command" <<'EOF'
#!/bin/bash
set -eu
: >"$POST_OPENER_MARKER"
EOF
chmod 0700 "$tmp/post-opener-command"
post_opener_marker=$tmp/post-opener-command-finished
expect_fail_with "a foreign raw opener appearing after I/O is refused" \
    'foreign_raw_openers=.*4242' "$tmp/post-opener-busy" \
    env PATH="$fakebin:$PATH" FAKE_FUSER_LOG="$tmp/fuser.log" \
        FAKE_CHAR_PATH="$opener_char" \
        FAKE_BUSY_AFTER_MARKER="$post_opener_marker" \
        POST_OPENER_MARKER="$post_opener_marker" EXITOS_SCALE_TEST_MODE=1 \
        bash "$script" --test-post-opener-gate /fake/mnt /dev/fake-whole \
        /dev/fake-p1 "$opener_char" /dev/fake-ctrl "$tmp/post-opener-command"
expect_ok "production post-opener gate precedes summary persistence and sealing" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        wait_line=$(grep -n '\''wait "\$CELL_IO_PID"'\'' <<<"$body" | head -1 | cut -d: -f1)
        opener=$(grep -n '\''opener_gate'\'' <<<"$body" | tail -1 | cut -d: -f1)
        persist=$(grep -n '\''persist_cell_summary'\'' <<<"$body" | head -1 | cut -d: -f1)
        seal=$(grep -n '\''seal_cell_evidence_success'\'' <<<"$body" | head -1 | cut -d: -f1)
        test -n "$wait_line" -a -n "$opener" -a -n "$persist" -a -n "$seal" &&
        test "$wait_line" -lt "$opener" -a "$opener" -lt "$persist" -a "$persist" -lt "$seal"
    ' bash "$script"
has '/sys/class/nvme-generic' "generic namespace character device is derived from current sysfs"
has 'CTRL_DEV=/dev/\$ctrl' "controller character node is derived from the resolved controller"
has 'CHAR=' "generic namespace character node is retained in campaign identity"
has 'namespace_char_retained_identity=' \
    "generation binds the retained generic-char inode identity"

# Kernel 7 exposes ng* -> namespace ownership but no generic-character nsid
# attribute.  Candidate basenames are deliberately misleading: selection must
# come from a read-only namespace-ID query on the retained char inode.
generic_sys=$tmp/generic-sys
generic_dev=$tmp/generic-dev
generic_owner=$tmp/generic-owner
generic_nvme=$tmp/generic-nvme
generic_nvme_log=$tmp/generic-nvme.log
mkdir -m 0700 "$generic_sys" "$generic_dev" "$generic_owner"
mkdir "$generic_sys/ng2n1" "$generic_sys/ng2n7" "$generic_sys/ng2n99"
ln -s "$generic_owner" "$generic_sys/ng2n1/device"
ln -s "$generic_owner" "$generic_sys/ng2n7/device"
ln -s "$generic_owner" "$generic_sys/ng2n99/device"
printf '%s\n' 1:8 >"$generic_sys/ng2n1/dev"
printf '%s\n' 1:5 >"$generic_sys/ng2n7/dev"
printf '%s\n' 1:3 >"$generic_sys/ng2n99/dev"
generic_fixture_ok=1
mknod -m 0600 "$generic_dev/ng2n99" c 1 3 2>/dev/null || generic_fixture_ok=0
mknod -m 0600 "$generic_dev/ng2n7" c 1 5 2>/dev/null || generic_fixture_ok=0
mknod -m 0600 "$generic_dev/ng2n1" c 1 7 2>/dev/null || generic_fixture_ok=0
cat >"$generic_nvme" <<'EOF'
#!/bin/bash
[ "$#" -eq 2 ] && [ "$1" = get-ns-id ] || exit 2
case "$2" in /proc/[0-9]*/fd/[0-9]*) ;; *) exit 3 ;; esac
printf '%s\n' "$2" >>"$FAKE_NVME_LOG"
case $(stat -Lc '%t:%T' -- "$2") in
    1:3) printf '%s: namespace-id:%s\n' "${2##*/}" 7 ;;
    1:5) printf '%s: namespace-id:%s\n' "${2##*/}" "${FAKE_ALL_NSID:-8}" ;;
    1:7) printf '%s: namespace-id:%s\n' "${2##*/}" 7 ;;
    *) exit 4 ;;
esac
EOF
chmod 0700 "$generic_nvme"
: >"$generic_nvme_log"
generic_out=
generic_rc=0
if [ "$generic_fixture_ok" -eq 1 ]; then
    generic_out=$(FAKE_NVME_LOG="$generic_nvme_log" EXITOS_SCALE_TEST_MODE=1 \
        bash "$script" --test-generic-char "$generic_sys" "$generic_dev" \
        "$generic_owner" 7 "$generic_nvme" 2>"$tmp/generic.err") || generic_rc=$?
else
    generic_rc=1
fi
if [ "$generic_rc" -eq 0 ] &&
   [[ $generic_out == GENERIC_CHAR_OK\ path="$generic_dev/ng2n99"\ nsid=7\ dev_t=1:3\ retained=/proc/*/fd/* ]] &&
   [ "$(wc -l <"$generic_nvme_log")" -eq 2 ] &&
   ! find "$generic_sys" -name nsid -print -quit | grep -q .; then
    ok "Kernel 7 layout selects generic char by retained-fd namespace ioctl"
else
    not_ok "Kernel 7 layout selects generic char by retained-fd namespace ioctl"
fi
expect_ok "generic-char probe never passes a mutable /dev pathname to nvme" \
    awk '!/^\/proc\/[0-9]+\/fd\/[0-9]+$/ { bad=1 }
         END { exit (bad || NR != 2) }' \
        "$generic_nvme_log"

duplicate_rc=0
duplicate_out=$(FAKE_NVME_LOG="$generic_nvme_log" FAKE_ALL_NSID=7 \
    EXITOS_SCALE_TEST_REPORT_GENERIC_CLOSE=1 EXITOS_SCALE_TEST_MODE=1 \
    bash "$script" --test-generic-char "$generic_sys" "$generic_dev" \
    "$generic_owner" 7 "$generic_nvme" 2>"$tmp/generic-duplicate.err") ||
    duplicate_rc=$?
if [ "$duplicate_rc" -ne 0 ] &&
   grep -q 'generic_char_matches=2' "$tmp/generic-duplicate.err" &&
   grep -qx 'GENERIC_CHAR_REJECT_CLOSED count=2' <<<"$duplicate_out"; then
    ok "duplicate matching generic chars are refused after all candidate fds close"
else
    not_ok "duplicate matching generic chars are refused after all candidate fds close"
fi

generic_replace_old=$tmp/generic-ng2n99-old
generic_replace_new=$tmp/generic-ng2n99-replacement
env FAKE_NVME_LOG="$generic_nvme_log" EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_PAUSE_GENERIC_REPEAT=1 \
    bash "$script" --test-generic-char-repeat "$generic_sys" "$generic_dev" \
    "$generic_owner" 7 "$generic_nvme" 2 \
    >"$tmp/generic-repeat-drift.out" 2>"$tmp/generic-repeat-drift.err" &
generic_repeat_pid=$!
generic_repeat_ready=0
for _ in $(seq 1 200); do
    if grep -qx GENERIC_CHAR_RETAINED_READY \
            "$tmp/generic-repeat-drift.out" 2>/dev/null; then
        generic_repeat_ready=1
        break
    fi
    kill -0 "$generic_repeat_pid" 2>/dev/null || break
    sleep 0.01
done
generic_repeat_rc=0
if [ "$generic_repeat_ready" -eq 1 ]; then
    mv "$generic_dev/ng2n99" "$generic_replace_old"
    mknod -m 0600 "$generic_dev/ng2n99" c 1 3
    kill -CONT "$generic_repeat_pid" 2>/dev/null || true
fi
wait "$generic_repeat_pid" || generic_repeat_rc=$?
if [ "$generic_repeat_ready" -eq 1 ]; then
    mv "$generic_dev/ng2n99" "$generic_replace_new"
    mv "$generic_replace_old" "$generic_dev/ng2n99"
fi
if [ "$generic_repeat_ready" -eq 1 ] && [ "$generic_repeat_rc" -ne 0 ] &&
   grep -q generic_char_identity_drift "$tmp/generic-repeat-drift.err"; then
    ok "second resolve rejects a replacement generic-char pathname inode"
else
    not_ok "second resolve rejects a replacement generic-char pathname inode"
fi

generic_repeat_out=
generic_repeat_rc=0
generic_repeat_out=$(FAKE_NVME_LOG="$generic_nvme_log" \
    EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-generic-char-repeat \
    "$generic_sys" "$generic_dev" "$generic_owner" 7 "$generic_nvme" 5 \
    2>"$tmp/generic-repeat-stable.err") || generic_repeat_rc=$?
if [ "$generic_repeat_rc" -eq 0 ] &&
   [[ $generic_repeat_out == GENERIC_CHAR_REPEAT_OK\ repeats=5\ persistent_delta=1\ stable=yes ]]; then
    ok "unchanged repeated resolves retain exactly one persistent generic-char fd"
else
    not_ok "unchanged repeated resolves retain exactly one persistent generic-char fd"
fi

order1=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-endpoint-order 1 2>/dev/null || true)
order2=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-endpoint-order 2 2>/dev/null || true)
order3=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-endpoint-order 3 2>/dev/null || true)
order4=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-endpoint-order 4 2>/dev/null || true)
if [ "$order1" = 'ENDPOINT_ORDER block=1 variants=gg,ss' ] &&
   [ "$order2" = 'ENDPOINT_ORDER block=2 variants=ss,gg' ] &&
   [ "$order3" = 'ENDPOINT_ORDER block=3 variants=gg,ss' ] &&
   [ "$order4" = 'ENDPOINT_ORDER block=4 variants=ss,gg' ]; then
    ok "endpoint campaign uses four balanced AB/BA blocks"
else
    not_ok "endpoint campaign uses four balanced AB/BA blocks"
fi
has 'run_endpoint_pilot' "production has a dedicated endpoint-only pilot"
has 'run_cell uringpoll thread 8' "endpoint pilot measures only uringpoll, threads, and eight jobs"
endpoint_body=$(sed -n '/^run_endpoint_pilot()/,/^}/p' "$script")
if grep -Eq 'process|run_cell (base|nvme|uring|pwrite)[[:space:]]|run_cell uringpoll (process|thread 1)' \
        <<<"$endpoint_body"; then
    not_ok "endpoint pilot does not expand into base/nvme/uring/pwrite/1T/process cells"
else
    ok "endpoint pilot does not expand into base/nvme/uring/pwrite/1T/process cells"
fi
has 'variant=%s' "summary records which trusted endpoint artifact produced each cell"

endpoint_input=$tmp/endpoint-summary.txt
printf '%s\n' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=1 variant=gg pair_us=20.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=1 variant=ss pair_us=18.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=2 variant=ss pair_us=21.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=2 variant=gg pair_us=22.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=3 variant=gg pair_us=24.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=3 variant=ss pair_us=20.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=4 variant=ss pair_us=23.000000' \
    'CELL_OK arm=uringpoll mode=thread jobs=8 rep=4 variant=gg pair_us=26.000000' \
    >"$endpoint_input"
endpoint_result=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" \
    --test-endpoint-summary "$endpoint_input" 2>/dev/null || true)
expect_ok "endpoint result reports the paired combined saving in microseconds" \
    test "$endpoint_result" = \
    'ENDPOINT_RESULT blocks=4 gg_pair_us_median=23.000000 ss_pair_us_median=20.500000 paired_saved_pair_us_median=2.500000 paired_saved_percent_of_gg_median=10.869565'
has 'summarize_endpoint "\$RESULT_DIR/summary.txt"' \
    "production emits the endpoint paired microsecond result"

# The authoritative follow-up is a separate factorial command.  It keeps the
# old endpoint-only pilot intact, but measures the current table/stats 2x2 plus
# one-thread and process controls in four deterministic balanced blocks.
factorial1=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-factorial-order 1 2>/dev/null || true)
factorial2=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-factorial-order 2 2>/dev/null || true)
factorial3=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-factorial-order 3 2>/dev/null || true)
factorial4=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-factorial-order 4 2>/dev/null || true)
if [ "$factorial1" = 'FACTORIAL_ORDER block=1 cells=thread:1:base:base,thread:8:uringpoll:gg,process:8:uringpoll:ss,thread:1:uringpoll:ss,thread:8:uringpoll:sg,process:8:base:base,thread:1:uringpoll:gg,thread:8:base:base,thread:8:uringpoll:ss,thread:8:uringpoll:gs' ] &&
   [ "$factorial2" = 'FACTORIAL_ORDER block=2 cells=thread:8:uringpoll:gs,thread:8:uringpoll:ss,thread:8:base:base,thread:1:uringpoll:gg,process:8:base:base,thread:8:uringpoll:sg,thread:1:uringpoll:ss,process:8:uringpoll:ss,thread:8:uringpoll:gg,thread:1:base:base' ] &&
   [ "$factorial3" = 'FACTORIAL_ORDER block=3 cells=thread:8:base:base,thread:1:uringpoll:ss,thread:8:uringpoll:gs,process:8:base:base,thread:8:uringpoll:gg,thread:1:base:base,process:8:uringpoll:ss,thread:8:uringpoll:ss,thread:1:uringpoll:gg,thread:8:uringpoll:sg' ] &&
   [ "$factorial4" = 'FACTORIAL_ORDER block=4 cells=thread:8:uringpoll:sg,thread:1:uringpoll:gg,thread:8:uringpoll:ss,process:8:uringpoll:ss,thread:1:base:base,thread:8:uringpoll:gg,process:8:base:base,thread:8:uringpoll:gs,thread:1:uringpoll:ss,thread:8:base:base' ]; then
    ok "factorial campaign has four frozen balanced orders"
else
    not_ok "factorial campaign has four frozen balanced orders"
fi
has 'run_factorial_pilot' "production has one integrated factorial pilot"
has '--pilot-factorial' "factorial pilot has one explicit production entry"
for allowed_shape in \
    'base thread 1 base' 'uringpoll thread 1 gg' 'uringpoll thread 1 ss' \
    'base thread 8 base' 'uringpoll thread 8 gg' 'uringpoll thread 8 gs' \
    'uringpoll thread 8 sg' 'uringpoll thread 8 ss' \
    'base process 8 base' 'uringpoll process 8 ss'; do
    # shellcheck disable=SC2086
    expect_ok "production whitelist accepts $allowed_shape" \
        env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-shape $allowed_shape
done
for forbidden_shape in \
    'nvme thread 8 ss' 'uring thread 8 ss' 'pwrite thread 8 ss' \
    'base thread 1 gg' 'uringpoll process 8 gg'; do
    # shellcheck disable=SC2086
    expect_fail "production whitelist rejects $forbidden_shape" \
        env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-shape $forbidden_shape
done
expect_ok "plan declares uringpoll with mandatory poll support" \
    bash -c 'grep -q "PLAN_OK .*arm=uringpoll .*poll_required=yes" "$1"' \
        bash "$script"
expect_ok "intercepted launch requests literal uringpoll with no fallback" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        grep -q '\''EXITOS_IOPATH=uringpoll'\'' <<<"$body" &&
        ! grep -q '\''EXITOS_IOPATH="\$arm"'\'' <<<"$body" &&
        grep -q '\''backend_requested=%s'\'' <<<"$body" &&
        grep -q '\''fallback=%s'\'' <<<"$body" &&
        grep -q '\''backend_requested=uringpoll'\'' <<<"$body" &&
        grep -q '\''fallback=forbidden'\'' <<<"$body"
    ' bash "$script"
expect_ok "only intercepted process cells opt into worker _exit stats" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        grep -q '\''mode" = process.*arm" != base'\'' <<<"$body" &&
        grep -q '\''EXITOS_STATS_ON_WORKER_EXIT=1'\'' <<<"$body" &&
        grep -q -- '\''-u EXITOS_STATS_ON_WORKER_EXIT'\'' <<<"$body"
    ' bash "$script"

factorial_input=$tmp/factorial-summary.txt
: >"$factorial_input"
for block in 1 2 3 4; do
    printf '%s\n' \
        "CELL_OK arm=base mode=thread jobs=1 rep=$block variant=base pair_us=10.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=1 rep=$block variant=gg pair_us=11.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=1 rep=$block variant=ss pair_us=10.500000" \
        "CELL_OK arm=base mode=thread jobs=8 rep=$block variant=base pair_us=10.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=8 rep=$block variant=gg pair_us=22.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=8 rep=$block variant=gs pair_us=20.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=8 rep=$block variant=sg pair_us=18.000000" \
        "CELL_OK arm=uringpoll mode=thread jobs=8 rep=$block variant=ss pair_us=16.000000" \
        "CELL_OK arm=base mode=process jobs=8 rep=$block variant=base pair_us=10.000000" \
        "CELL_OK arm=uringpoll mode=process jobs=8 rep=$block variant=ss pair_us=11.000000" \
        >>"$factorial_input"
done
factorial_result=$(EXITOS_SCALE_TEST_MODE=1 bash "$script" \
    --test-factorial-summary "$factorial_input" 2>/dev/null || true)
expect_ok "factorial summary reports table, stats, interaction, residual, and thread/process DiD" \
    test "$factorial_result" = \
    'FACTORIAL_RESULT blocks=4 one_thread_gg_residual_pair_us_median=1.000000 one_thread_ss_residual_pair_us_median=0.500000 one_thread_gg_to_ss_saved_pair_us_median=0.500000 table_saved_global_stats_pair_us_median=4.000000 table_saved_sharded_stats_pair_us_median=4.000000 stats_saved_global_table_pair_us_median=2.000000 stats_saved_sharded_table_pair_us_median=2.000000 interaction_pair_us_median=0.000000 eight_thread_ss_residual_pair_us_median=6.000000 eight_process_ss_residual_pair_us_median=1.000000 thread_process_residual_did_pair_us_median=5.000000'
factorial_incomplete=$tmp/factorial-incomplete.txt
sed '$d' "$factorial_input" >"$factorial_incomplete"
expect_fail "factorial summary rejects an incomplete 40-cell matrix" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" \
        --test-factorial-summary "$factorial_incomplete"
has 'summarize_factorial "\$RESULT_DIR/summary.txt"' \
    "production emits the factorial microsecond decomposition"

# Summary validation is behavioral: the intercepted arm must account for
# every write/sync with exact worker files, while the sole unpreloaded control
# must have neither stats nor Exitos preload markers.
summary_cell=$tmp/summary-cell
mkdir -m 0700 "$summary_cell"
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":5,"iops":5.0,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":2}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":3}]}
EOF
printf '%s\n' 5 2 0 0 0 >"$summary_cell/stats-101.txt"
expect_ok "uringpoll summary counts sync.lat_ns.N for exact takeover" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
printf '%s\n' 5 2 0 0 1 >"$summary_cell/stats-101.txt"
expect_fail "uringpoll summary rejects a nonzero refresh counter" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
printf '%s\n' 5 2 0 0 0 >"$summary_cell/stats-101.txt"
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":5,"iops":NaN,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":2}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":3}]}
EOF
expect_fail "cell summary rejects a NaN fio metric" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":5,"iops":5.0,"lat_ns":{"mean":Infinity}},"sync":{"lat_ns":{"mean":2000,"N":2}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":3}]}
EOF
expect_fail "cell summary rejects an infinite fio metric" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":5,"iops":5.0,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":2}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":-1}]}
EOF
expect_fail "cell summary rejects a negative fio metric" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":0,"iops":5.0,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":0}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":3}]}
EOF
expect_fail "cell summary rejects a zero I/O denominator" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
cat >"$summary_cell/fio.json" <<'EOF'
{"jobs":[{"error":0,"write":{"total_ios":5,"iops":5.0,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":2}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":3}]}
EOF
printf '%s\n' 4 2 0 0 0 >"$summary_cell/stats-101.txt"
expect_fail "uringpoll summary rejects a fast-write mismatch" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
printf '%s\n' 5 2 0 0 0 >"$summary_cell/stats-101.txt"
cp "$summary_cell/stats-101.txt" "$summary_cell/stats-102.txt"
expect_fail "uringpoll summary rejects extra worker stats files" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
rm -f "$summary_cell"/stats-*.txt
printf '%s\n' 5 2 0 0 0 999 >"$summary_cell/stats-101.txt"
expect_fail "uringpoll summary rejects a stats file with an extra line" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" uringpoll thread 1 1 ss
rm -f "$summary_cell"/stats-*.txt
: >"$summary_cell/stdout.txt"
: >"$summary_cell/stderr.txt"
expect_ok "base summary requires zero stats and no Exitos markers" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" base thread 1 1 base
printf '%s\n' 'EXITOS ARMED' >"$summary_cell/stderr.txt"
expect_fail "base summary rejects an Exitos preload marker" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$summary_cell" base thread 1 1 base

process_summary_cell=$tmp/process-summary-cell
mkdir -m 0700 "$process_summary_cell"
{
    printf '{"jobs":['
    for worker in $(seq 1 8); do
        [ "$worker" -eq 1 ] || printf ','
        printf '{"error":0,"write":{"total_ios":1,"iops":1.0,"lat_ns":{"mean":1000}},"sync":{"lat_ns":{"mean":2000,"N":1}},"job_runtime":1000,"usr_cpu":1.0,"sys_cpu":2.0,"ctx":1}'
        printf '%s\n' 1 1 0 0 0 >"$process_summary_cell/stats-$worker.txt"
    done
    printf ']}\n'
} >"$process_summary_cell/fio.json"
expect_ok "process summary requires exactly eight nonzero worker stats files" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$process_summary_cell" uringpoll process 8 1 ss
printf '%s\n' 0 0 0 0 0 >"$process_summary_cell/stats-parent.txt"
expect_fail "process summary rejects a ninth zero parent stats file" \
    env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-cell-summary \
        "$process_summary_cell" uringpoll process 8 1 ss

# A drift during the final cell must fail before that cell is summarized; a
# next-cell precheck is too late for the last cell in the campaign.
post_generation=$tmp/post-generation
post_summary=$tmp/post-summary
printf '%s\n' before >"$post_generation"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_POST_CELL=1 \
    bash "$script" --test-post-cell-generation "$post_generation" "$post_summary" \
    >"$tmp/post-generation.out" 2>"$tmp/post-generation.err" & post_pid=$!
post_ready=0
for _ in $(seq 1 200); do
    if grep -qx POST_CELL_IO_COMPLETE "$tmp/post-generation.out" 2>/dev/null; then
        post_ready=1
        break
    fi
    kill -0 "$post_pid" 2>/dev/null || break
    sleep 0.01
done
post_rc=0
if [ "$post_ready" -eq 1 ]; then
    printf '%s\n' after >"$post_generation"
    kill -CONT "$post_pid" 2>/dev/null || true
fi
wait "$post_pid" || post_rc=$?
expect_ok "post-cell generation drift is refused before summary" \
    bash -c 'test "$1" -eq 1 -a "$2" -ne 0 -a ! -e "$3" && grep -q generation_drift "$4"' \
        bash "$post_ready" "$post_rc" "$post_summary" "$tmp/post-generation.err"
has 'assert_campaign_generation post-cell' \
    "production rechecks frozen generation after each cell"

# Coordination is per target, not per output directory.  Two otherwise valid
# runners with different RESULT_ROOT values must contend on the same fixed
# serial-derived lock object.
lock_root=$tmp/lock-root
result_one=$tmp/result-one
result_two=$tmp/result-two
mkdir -m 0700 "$lock_root"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_LOCK_ROOT="$lock_root" \
    EXITOS_SCALE_TEST_PAUSE_AFTER_LOCK=1 \
    bash "$script" --test-campaign-lock "$result_one" \
    >"$tmp/lock-one.out" 2>"$tmp/lock-one.err" & lock_pid=$!
lock_ready=0
for _ in $(seq 1 200); do
    if grep -qx CAMPAIGN_LOCK_HELD "$tmp/lock-one.out" 2>/dev/null; then
        lock_ready=1
        break
    fi
    kill -0 "$lock_pid" 2>/dev/null || break
    sleep 0.01
done
second_lock_rc=0
if [ "$lock_ready" -eq 1 ]; then
    env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_LOCK_ROOT="$lock_root" \
        bash "$script" --test-campaign-lock "$result_two" \
        >"$tmp/lock-two.out" 2>"$tmp/lock-two.err" || second_lock_rc=$?
    kill -CONT "$lock_pid" 2>/dev/null || true
fi
wait "$lock_pid" 2>/dev/null || true
expect_ok "different result roots cannot bypass the fixed target lock" \
    bash -c 'test "$1" -eq 1 -a "$2" -ne 0 && grep -q another_scale_campaign_running "$3"' \
        bash "$lock_ready" "$second_lock_rc" "$tmp/lock-two.err"
has 'LOCK_ROOT=/run' "production lock root is fixed under /run"
if rg -q 'lock_path=\$RESULT_ROOT' "$script"; then
    not_ok "coordination lock identity is independent of RESULT_ROOT"
else
    ok "coordination lock identity is independent of RESULT_ROOT"
fi

# Binding the parent directory is insufficient: bind each exact workload inode
# to fd 100..107 before exec.  Replace w0 after the bind and prove the command
# writes only the retained old inode, never the replacement symlink target.
bound_dir=$tmp/bound-leaves
bound_old=$tmp/bound-w0-old
bound_sentinel=$tmp/bound-sentinel
bound_command=$tmp/bound-command
mkdir -m 0700 "$bound_dir"
EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$bound_dir" \
    >"$tmp/bound-init.out" 2>"$tmp/bound-init.err" || true
printf '%s\n' DO_NOT_TOUCH >"$bound_sentinel"
cat >"$bound_command" <<'EOF'
#!/bin/bash
printf '%s\n' RETAINED_WRITE > /proc/self/fd/100
grep -qx RETAINED_WRITE /proc/self/fd/100
printf '%s\n' BOUND_COMMAND_OK
EOF
chmod 0700 "$bound_command"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_AFTER_BOUND_FDS=1 \
    bash "$script" --test-bound-workfiles "$bound_dir" "$bound_command" \
    >"$tmp/bound.out" 2>"$tmp/bound.err" & bound_pid=$!
bound_ready=0
for _ in $(seq 1 200); do
    if grep -qx BOUND_FDS_READY "$tmp/bound.out" 2>/dev/null; then
        bound_ready=1
        break
    fi
    kill -0 "$bound_pid" 2>/dev/null || break
    sleep 0.01
done
bound_rc=0
if [ "$bound_ready" -eq 1 ]; then
    bound_child=$(ps --ppid "$bound_pid" -o pid= | awk 'NR == 1 { print $1 }')
    [ -n "$bound_child" ] || bound_child=$bound_pid
    mv "$bound_dir/w0.log" "$bound_old"
    ln -s "$bound_sentinel" "$bound_dir/w0.log"
    [ -n "$bound_child" ] && kill -CONT "$bound_child" 2>/dev/null || true
fi
wait "$bound_pid" || bound_rc=$?
expect_ok "actual command I/O stays on the retained leaf inode after path replacement" \
    bash -c 'test "$1" -eq 1 -a "$2" -eq 0 && grep -qx BOUND_COMMAND_OK "$3" && grep -qx RETAINED_WRITE "$4" && grep -qx DO_NOT_TOUCH "$5"' \
        bash "$bound_ready" "$bound_rc" "$tmp/bound.out" "$bound_old" "$bound_sentinel"

# The final measured exec receives only stdio and the explicitly bound workload
# and preload descriptors.  In particular, evidence/character/lock descriptors
# inherited by the launcher must not cross this boundary.
cat >"$tmp/exact-fd-command" <<'EOF'
#!/usr/bin/env python3
import fcntl
import os
allowed={0,1,2,100,101,102,103,104,105,106,107}
live=set()
for fd in range(0,256):
    try: fcntl.fcntl(fd,fcntl.F_GETFD)
    except OSError: continue
    live.add(fd)
if live != allowed:
    raise SystemExit("unexpected inherited fd set: %r" % sorted(live))
print("BOUND_FD_SET_OK")
EOF
chmod 0700 "$tmp/exact-fd-command"
bound_fd_dir=$tmp/bound-fd-leaves
mkdir -m 0700 "$bound_fd_dir"
EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-workfile-init "$bound_fd_dir" \
    >"$tmp/bound-fd-init.out" 2>"$tmp/bound-fd-init.err" || true
exec 9<"$script"
exec 10<"$script"
exec 11<"$script"
bound_fd_rc=0
env EXITOS_SCALE_TEST_MODE=1 CELL_EVIDENCE_DIR_FD=9 \
    EXITOS_CHAR_FD=10 EXITOS_SCALE_LOCK_FD=11 \
    bash "$script" --test-bound-workfiles "$bound_fd_dir" "$tmp/exact-fd-command" \
    >"$tmp/bound-fd-set.out" 2>"$tmp/bound-fd-set.err" || bound_fd_rc=$?
exec 9<&- 10<&- 11<&-
expect_ok "final measured exec inherits exactly stdio and fd 100..107" \
    bash -c 'test "$1" -eq 0 && grep -qx BOUND_FD_SET_OK "$2"' \
        bash "$bound_fd_rc" "$tmp/bound-fd-set.out"

# A successful direct fio/master wait is not sufficient if a process-mode
# descendant remains in the launcher's private group after closing all target
# fds.  The runner must kill the survivor and reject the cell.
cat >"$tmp/fake-success-with-survivor" <<'EOF'
#!/usr/bin/env python3
import os
import signal
child=os.fork()
if child == 0:
    for fd in range(100,108):
        try: os.close(fd)
        except OSError: pass
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
    with open(os.environ["SURVIVOR_PID_FILE"],"w",encoding="ascii") as out:
        out.write("%d\n" % os.getpid())
    while True: signal.pause()
os._exit(0)
EOF
chmod 0700 "$tmp/fake-success-with-survivor"
survivor_rc=0
env EXITOS_SCALE_TEST_MODE=1 SURVIVOR_PID_FILE="$tmp/survivor.pid" \
    bash "$script" --test-bound-group-exit "$tmp/fake-success-with-survivor" \
    >"$tmp/survivor.out" 2>"$tmp/survivor.err" || survivor_rc=$?
survivor_pid=$(cat "$tmp/survivor.pid" 2>/dev/null || true)
expect_ok "successful leader with a stubborn private-group descendant is cleaned and rejected" \
    bash -c '
        test "$1" -ne 0 && test -n "$2" &&
        ! kill -0 "$2" 2>/dev/null &&
        grep -q bound_io_group_not_empty "$3"
    ' bash "$survivor_rc" "$survivor_pid" "$tmp/survivor.err"
has 'filename_format=/proc/self/fd/10\$jobnum' \
    "production fio opens only retained workload descriptors"

# The shared frontend configuration implements EXITOS_FILES as colon-separated
# substring tokens over the original open(2) pathname, and preload.c delegates
# selection to it.  Evaluate that exact contract against the production
# assignment: every retained workload fd must match, while adjacent fds, the
# preload artifact, stats, and the old .log pathname must remain unselected.
if bash -c '
    runner=$1
    preload=$2
    frontend_config=$3
    body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$runner")
    pattern=$(sed -n '\''s/.*EXITOS_FILES=\([^[:space:]]*\).*/\1/p'\'' \
        <<<"$body" | head -1)
    rg -q '\''exitos_frontend_config_path_selected\([^,]*, path\)'\'' \
        "$preload" || exit 1
    rg -q '\''path_contains\(path, begin, '\'' "$frontend_config" || exit 1
    python3 - "$pattern" <<'\''PY'\''
import sys
pattern=sys.argv[1]
tokens=[token for token in pattern.split(":") if token]
selected=lambda path:any(token in path for token in tokens)
required=[f"/proc/self/fd/{fd}" for fd in range(100,108)]
excluded=["/proc/self/fd/108","/proc/self/fd/109","/proc/self/fd/120",
          "/results/stats-123.txt","/tmp/w0.log"]
raise SystemExit(0 if (tokens and all(map(selected,required)) and
                       not any(map(selected,excluded))) else 1)
PY
' bash "$script" "$preload" "$frontend_config"; then
    ok "production preload filter selects exactly the eight retained workload fds"
else
    not_ok "production preload filter selects exactly the eight retained workload fds"
fi
if ! sed -n '/^prepare_files()/,/^}$/p' "$script" |
        rg -q 'LD_PRELOAD|EXITOS_FILES'; then
    ok "prepare remains outside preload interception"
else
    not_ok "prepare remains outside preload interception"
fi

# The test entry must reach the shared capture helper in both directions: a
# readable value is returned intact, while a failed read cannot be hidden by a
# later successful printf.
printf '%s\n' required-value >"$tmp/generation-readable"
if env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-required-generation-read \
        "$tmp/generation-readable" >"$tmp/generation-read-ok.out" \
        2>"$tmp/generation-read-ok.err" &&
   grep -qx 'REQUIRED_GENERATION_READ_OK value=required-value' \
        "$tmp/generation-read-ok.out" &&
   ! env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-required-generation-read \
        /does/not/exist >"$tmp/generation-read-bad.out" \
        2>"$tmp/generation-read-bad.err" &&
   grep -q 'required_generation_read_failed' "$tmp/generation-read-bad.err"; then
    ok "required generation reads preserve values and fail closed"
else
    not_ok "required generation reads preserve values and fail closed"
fi

# Even a failed fio may have issued writes.  Source order must put all post
# gates before the rc refusal.
expect_ok "fio rc is evaluated only after post-cell gates" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        post=$(grep -n "verify_workset post-cell" <<<"$body" | head -1 | cut -d: -f1)
        rc=$(grep -n '\''\[ "$rc" -eq 0 \]'\'' <<<"$body" | head -1 | cut -d: -f1)
        test -n "$post" -a -n "$rc" -a "$post" -lt "$rc"
    ' bash "$script"

# Continuous-noise fixtures.  The 20 ms test poll and 100 ms compiler burst
# preserve the relationship of a 10 s build burst hidden between the old
# 30 s point samples without making the unit test wait ten minutes.
noise_proc=$tmp/noise-proc
noise_cpu=$tmp/noise-cpu
noise_busy=$tmp/noise-busy
mkdir -p "$noise_proc" \
    "$noise_cpu/cpu2/topology" "$noise_cpu/cpu6/topology"
printf '%s\n' '0.00 0.00 1/1 1' >"$noise_proc/loadavg"
for cpu in 2 6; do
    printf '%s\n' 0 >"$noise_cpu/cpu$cpu/topology/physical_package_id"
    printf '%s\n' 1 >"$noise_cpu/cpu$cpu/topology/core_id"
    printf '%s\n' 2,6 >"$noise_cpu/cpu$cpu/topology/thread_siblings_list"
    printf '%s\n' 1 >"$noise_cpu/cpu$cpu/online"
done
cat >"$tmp/noise-stat-updater" <<'EOF'
#!/bin/bash
set -eu
proc=$1
busy=$2
base_busy=${NOISE_BASE_BUSY:-1}
base_idle=$((100 - base_busy))
u2=10 i2=1000 u6=10 i6=1000 seq=0
while :; do
    seq=$((seq + 1))
    if [ -e "$busy.selected" ]; then
        u2=$((u2 + 100))
    else
        u2=$((u2 + base_busy)); i2=$((i2 + base_idle))
    fi
    if [ -e "$busy" ]; then
        u6=$((u6 + 100))
    else
        u6=$((u6 + base_busy)); i6=$((i6 + base_idle))
    fi
    next=$proc/stat.next.$$
    printf 'cpu  %s 0 0 %s 0 0 0 0 0 0\n' "$((u2 + u6))" "$((i2 + i6))" >"$next"
    printf 'cpu2 %s 0 0 %s 0 0 0 0 0 0\n' "$u2" "$i2" >>"$next"
    printf 'cpu6 %s 0 0 %s 0 0 0 0 0 0\n' "$u6" "$i6" >>"$next"
    blocked=0
    [ ! -e "$busy.blocked" ] || blocked=1
    printf 'procs_running 1\nprocs_blocked %s\n' "$blocked" >>"$next"
    mv -f -- "$next" "$proc/stat"
    sleep 0.02
done
EOF
chmod 0700 "$tmp/noise-stat-updater"

start_stat_updater()
{
    rm -f -- "$noise_busy" "$noise_busy.selected" "$noise_busy.blocked"
    "$tmp/noise-stat-updater" "$noise_proc" "$noise_busy" &
    stat_updater_pid=$!
    for _ in $(seq 1 100); do
        [ -f "$noise_proc/stat" ] && return 0
        sleep 0.01
    done
    return 1
}

start_percent_stat_updater()
{
    local percent=$1
    rm -f -- "$noise_busy" "$noise_busy.selected"
    env NOISE_BASE_BUSY="$percent" \
        "$tmp/noise-stat-updater" "$noise_proc" "$noise_busy" &
    stat_updater_pid=$!
    for _ in $(seq 1 100); do
        [ -f "$noise_proc/stat" ] && return 0
        sleep 0.01
    done
    return 1
}

stop_stat_updater()
{
    kill -TERM "$stat_updater_pid" 2>/dev/null || true
    wait "$stat_updater_pid" 2>/dev/null || true
}

wait_for_marker()
{
    local marker=$1 file=$2 pid=$3
    for _ in $(seq 1 200); do
        grep -q -- "$marker" "$file" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.01
    done
    return 1
}

process_is_live()
{
    local pid=${1:-} state
    [[ $pid =~ ^[0-9]+$ ]] || return 1
    state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
    case "$state" in ''|Z|X) return 1 ;; esac
    return 0
}

start_stat_updater
clean_monitor_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-clean.out" 2>"$tmp/noise-clean.err" || clean_monitor_rc=$?
stop_stat_updater
clean_elapsed=$(sed -n \
    's/^NOISE_MONITOR_OK mode=preflight .* elapsed_ms=\([0-9][0-9]*\) .*/\1/p' \
    "$tmp/noise-clean.out" | head -1)
if [ "$clean_monitor_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_READY mode=preflight ' "$tmp/noise-clean.out" &&
   grep -q '^CPU_UTIL_WINDOW mode=preflight scope=tail ' "$tmp/noise-clean.out" &&
   awk '
       $1 == "CPU_UTIL_WINDOW" {
           cpu = scope = window = ""
           for (i = 1; i <= NF; i++) {
               split($i, pair, "=")
               if (pair[1] == "cpu") cpu = pair[2]
               if (pair[1] == "scope") scope = pair[2]
               if (pair[1] == "window_ms") window = pair[2]
           }
           if (cpu == 2 || cpu == 6) {
               count[cpu]++
               total[cpu] += window
               if (scope == "tail") tail[cpu]++
           }
       }
       END {
           exit !(count[2] == 3 && count[6] == 3 &&
                  tail[2] == 1 && tail[6] == 1 &&
                  total[2] >= 250 && total[2] <= 275 &&
                  total[6] >= 250 && total[6] <= 275)
       }
   ' "$tmp/noise-clean.out" &&
   [[ $clean_elapsed =~ ^[0-9]+$ ]] &&
   [ "$clean_elapsed" -ge 240 ] && [ "$clean_elapsed" -le 285 ] &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-clean.out"; then
    ok "preflight covers the exact requested duration including its partial tail"
else
    not_ok "preflight covers the exact requested duration including its partial tail"
fi

# A preflight monitor has no legitimate early-success signal.  An external
# TERM must make the observation fail instead of truncating a requested window
# and printing NOISE_MONITOR_OK.
start_stat_updater
preflight_early_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 2000 20 100 15.0 \
    >"$tmp/noise-preflight-early.out" 2>"$tmp/noise-preflight-early.err" &
preflight_early_runner=$!
preflight_early_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=preflight ' \
        "$tmp/noise-preflight-early.out" "$preflight_early_runner"; then
    preflight_early_ready=1
    preflight_early_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=preflight pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-preflight-early.out" | head -1)
    kill -TERM "$preflight_early_monitor" 2>/dev/null || true
fi
wait "$preflight_early_runner" || preflight_early_rc=$?
stop_stat_updater
if [ "$preflight_early_ready" -eq 1 ] && [ "$preflight_early_rc" -ne 0 ] &&
   grep -q '^NOISE_EVENT kind=unauthorized_stop mode=preflight ' \
        "$tmp/noise-preflight-early.out" &&
   ! grep -q '^NOISE_MONITOR_OK mode=preflight ' \
        "$tmp/noise-preflight-early.out"; then
    ok "preflight refuses an early external TERM instead of truncating coverage"
else
    not_ok "preflight refuses an early external TERM instead of truncating coverage"
fi

# Force TERM to become pending after one loop has consumed its signal state but
# before any terminal break.  This kills the mutation where stopping=True is
# observed later in the same iteration and preflight incorrectly prints OK.
start_stat_updater
preflight_late_rc=0
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_PAUSE_AFTER_SIGNAL_SCAN=1 \
    bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 600 20 100 15.0 \
    >"$tmp/noise-preflight-late.out" 2>"$tmp/noise-preflight-late.err" &
preflight_late_runner=$!
preflight_late_ready=0
if wait_for_marker '^NOISE_SIGNAL_SCAN_PASSED mode=preflight ' \
        "$tmp/noise-preflight-late.out" "$preflight_late_runner"; then
    preflight_late_ready=1
    preflight_late_monitor=$(sed -n \
        's/^NOISE_SIGNAL_SCAN_PASSED mode=preflight pid=\([0-9][0-9]*\)$/\1/p' \
        "$tmp/noise-preflight-late.out" | head -1)
    kill -TERM "$preflight_late_monitor" 2>/dev/null || true
    kill -CONT "$preflight_late_monitor" 2>/dev/null || true
fi
wait "$preflight_late_runner" || preflight_late_rc=$?
stop_stat_updater
if [ "$preflight_late_ready" -eq 1 ] && [ "$preflight_late_rc" -ne 0 ] &&
   grep -q '^NOISE_EVENT kind=unauthorized_stop mode=preflight signal=15$' \
        "$tmp/noise-preflight-late.out" &&
   ! grep -q '^NOISE_MONITOR_OK mode=preflight ' \
        "$tmp/noise-preflight-late.out"; then
    ok "preflight refuses TERM delivered after its per-loop signal scan"
else
    [ -z "${preflight_late_monitor:-}" ] || \
        kill -KILL "$preflight_late_monitor" 2>/dev/null || true
    kill -KILL "$preflight_late_runner" 2>/dev/null || true
    not_ok "preflight refuses TERM delivered after its per-loop signal scan"
fi

start_percent_stat_updater 14
util14_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-util14.out" 2>"$tmp/noise-util14.err" || util14_rc=$?
stop_stat_updater
start_percent_stat_updater 15
util15_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-util15.out" 2>"$tmp/noise-util15.err" || util15_rc=$?
stop_stat_updater
if [ "$util14_rc" -eq 0 ] && [ "$util15_rc" -eq 0 ] &&
   grep -q 'CPU_UTIL_WINDOW mode=preflight .*util_pct=14\.000' \
        "$tmp/noise-util14.out" &&
   grep -q 'CPU_UTIL_WINDOW mode=preflight .*util_pct=15\.000' \
        "$tmp/noise-util15.out" &&
   grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=preflight .*reference=15\.000' \
        "$tmp/noise-util15.out" &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-util15.out" &&
   ! grep -q '^NOISE_EVENT kind=cpu_busy ' "$tmp/noise-util15.out"; then
    ok "15 percent is recorded as a reference exceedance and never rejects preflight"
else
    not_ok "15 percent is recorded as a reference exceedance and never rejects preflight"
fi

# A sibling burst after the final complete window is still part of the timed
# cell.  Stopping the sentinel must evaluate that tail rather than silently
# accepting the interval between the last boundary and fio completion.
start_stat_updater
cell_tail_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 500 15.0 \
    >"$tmp/noise-cell-tail.out" 2>"$tmp/noise-cell-tail.err" &
cell_tail_runner=$!
cell_tail_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-cell-tail.out" "$cell_tail_runner"; then
    cell_tail_ready=1
    cell_tail_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-tail.out" | head -1)
    sleep 0.12
    : >"$noise_busy"
    sleep 0.05
    kill -TERM "$cell_tail_monitor" 2>/dev/null || true
    rm -f -- "$noise_busy"
fi
wait "$cell_tail_runner" || cell_tail_rc=$?
stop_stat_updater
if [ "$cell_tail_ready" -eq 1 ] && [ "$cell_tail_rc" -eq 0 ] &&
   grep -q 'CPU_UTIL_WINDOW mode=cell scope=tail .*cpu=6 role=sibling ' \
        "$tmp/noise-cell-tail.out" &&
   grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=cell .*cpu=6 role=sibling' \
        "$tmp/noise-cell-tail.out" &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-tail.out" &&
   ! grep -q '^NOISE_EVENT kind=cpu_busy ' "$tmp/noise-cell-tail.out"; then
    ok "cell sentinel records a busy final partial window without rejecting it"
else
    not_ok "cell sentinel records a busy final partial window without rejecting it"
fi

start_stat_updater
blind_monitor_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_SAMPLE_DELAY_MS=1100 \
    bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-blind.out" 2>"$tmp/noise-blind.err" || blind_monitor_rc=$?
stop_stat_updater
if [ "$blind_monitor_rc" -ne 0 ] &&
   grep -q '^NOISE_MONITOR_READY mode=preflight ' "$tmp/noise-blind.out" &&
   grep -q 'NOISE_EVENT kind=monitor_gap gap_ms=' "$tmp/noise-blind.out"; then
    ok "monitor refuses when scheduling creates an actual blind interval over one second"
else
    not_ok "monitor refuses when scheduling creates an actual blind interval over one second"
fi

start_stat_updater
cell_blind_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_SAMPLE_DELAY_MS=1100 \
    bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/noise-cell-blind.out" 2>"$tmp/noise-cell-blind.err" &
cell_blind_runner=$!
cell_blind_latched=0
cell_blind_live=0
cell_blind_still_live=0
cell_blind_ticks_delta=999999
if wait_for_marker 'NOISE_EVENT kind=monitor_gap gap_ms=' \
        "$tmp/noise-cell-blind.out" "$cell_blind_runner"; then
    cell_blind_latched=1
    cell_blind_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-blind.out" | head -1)
    cell_blind_state=$(awk '{print $3}' "/proc/$cell_blind_monitor/stat" \
        2>/dev/null || true)
    case "$cell_blind_state" in
        ''|Z|X) ;;
        *)
            cell_blind_live=1
            cell_blind_ticks_before=$(awk '{print $14+$15}' \
                "/proc/$cell_blind_monitor/stat")
            sleep 0.20
            if process_is_live "$cell_blind_monitor"; then
                cell_blind_still_live=1
                cell_blind_ticks_after=$(awk '{print $14+$15}' \
                    "/proc/$cell_blind_monitor/stat")
                cell_blind_ticks_delta=$((cell_blind_ticks_after - cell_blind_ticks_before))
                kill -TERM "$cell_blind_monitor" 2>/dev/null || true
            fi
            ;;
    esac
fi
wait "$cell_blind_runner" || cell_blind_rc=$?
stop_stat_updater
if [ "$cell_blind_latched" -eq 1 ] && [ "$cell_blind_live" -eq 1 ] &&
   [ "$cell_blind_still_live" -eq 1 ] &&
   [ "$cell_blind_ticks_delta" -le 3 ] && [ "$cell_blind_rc" -ne 0 ] &&
   grep -q '^NOISE_MONITOR_READY mode=cell ' "$tmp/noise-cell-blind.out" &&
   grep -q 'NOISE_EVENT kind=monitor_gap gap_ms=' "$tmp/noise-cell-blind.out" &&
   ! grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-blind.out"; then
    ok "latched cell monitor gaps fail closed without exit/reuse or tight-spin"
else
    [ -z "${cell_blind_monitor:-}" ] || \
        kill -KILL "$cell_blind_monitor" 2>/dev/null || true
    kill -KILL "$cell_blind_runner" 2>/dev/null || true
    not_ok "latched cell monitor gaps fail closed without exit/reuse or tight-spin"
fi

# A healthy cell monitor must sleep between deadlines.  This catches a removed
# sleep or a stale deadline that turns the observer into a measurement-polluting
# tight loop.
start_stat_updater
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/noise-cell-cpu.out" 2>"$tmp/noise-cell-cpu.err" &
cell_cpu_runner=$!
cell_cpu_ready=0
cell_cpu_ticks_delta=999999
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-cell-cpu.out" "$cell_cpu_runner"; then
    cell_cpu_ready=1
    cell_cpu_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-cpu.out" | head -1)
    cell_cpu_ticks_before=$(awk '{print $14+$15}' "/proc/$cell_cpu_monitor/stat")
    sleep 0.20
    cell_cpu_ticks_after=$(awk '{print $14+$15}' "/proc/$cell_cpu_monitor/stat")
    cell_cpu_ticks_delta=$((cell_cpu_ticks_after - cell_cpu_ticks_before))
    kill -TERM "$cell_cpu_monitor" 2>/dev/null || true
fi
cell_cpu_rc=0
wait "$cell_cpu_runner" || cell_cpu_rc=$?
stop_stat_updater
if [ "$cell_cpu_ready" -eq 1 ] && [ "$cell_cpu_rc" -eq 0 ] &&
   [ "$cell_cpu_ticks_delta" -le 3 ] &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-cpu.out"; then
    ok "healthy cell observer remains low-rate and cannot tight-spin"
else
    [ -z "${cell_cpu_monitor:-}" ] || kill -KILL "$cell_cpu_monitor" 2>/dev/null || true
    not_ok "healthy cell observer remains low-rate and cannot tight-spin"
fi

env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_PAUSE_BEFORE_MONITOR_PDEATHSIG=1 \
    bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/monitor-parent-death.out" 2>"$tmp/monitor-parent-death.err" &
monitor_parent_runner=$!
monitor_prearm_ready=0
for _ in $(seq 1 300); do
    if grep -q '^MONITOR_PRE_PDEATHSIG_READY pid=[0-9][0-9]* expected_parent=[0-9][0-9]*$' \
            "$tmp/monitor-parent-death.out" 2>/dev/null; then
        monitor_prearm_ready=1
        break
    fi
    kill -0 "$monitor_parent_runner" 2>/dev/null || break
    sleep 0.01
done
monitor_prearm_child=$(sed -n \
    's/^MONITOR_PRE_PDEATHSIG_READY pid=\([0-9][0-9]*\) expected_parent=.*/\1/p' \
    "$tmp/monitor-parent-death.out" 2>/dev/null | head -1)
monitor_prearm_expected=$(sed -n \
    's/^MONITOR_PRE_PDEATHSIG_READY .* expected_parent=\([0-9][0-9]*\)$/\1/p' \
    "$tmp/monitor-parent-death.out" 2>/dev/null | head -1)
monitor_prearm_parent=$(ps -o ppid= -p "$monitor_prearm_child" 2>/dev/null |
    tr -d '[:space:]')
monitor_parent_rc=0
if [ "$monitor_prearm_ready" -eq 1 ]; then
    kill -KILL "$monitor_parent_runner" 2>/dev/null || true
fi
wait "$monitor_parent_runner" 2>/dev/null || monitor_parent_rc=$?
[ -z "$monitor_prearm_child" ] || kill -CONT "$monitor_prearm_child" 2>/dev/null || true
monitor_child_live=0
if [ -n "$monitor_prearm_child" ]; then
    monitor_child_live=1
    for _ in $(seq 1 300); do
        if [ ! -r "/proc/$monitor_prearm_child/stat" ]; then
            monitor_child_live=0
            break
        fi
        monitor_child_state=$(awk '{print $3}' "/proc/$monitor_prearm_child/stat" 2>/dev/null || true)
        case "$monitor_child_state" in Z|X|'') monitor_child_live=0; break;; esac
        sleep 0.01
    done
fi
if [ "$monitor_prearm_ready" -eq 1 ] && [ "$monitor_parent_rc" -eq 137 ] &&
   [ "$monitor_prearm_expected" = "$monitor_parent_runner" ] &&
   [ "$monitor_prearm_parent" = "$monitor_parent_runner" ] &&
   [ "$monitor_child_live" -eq 0 ]; then
    ok "noise monitor refuses when expected parent dies before PDEATHSIG is armed"
else
    [ -z "$monitor_prearm_child" ] || kill -KILL "$monitor_prearm_child" 2>/dev/null || true
    kill -KILL "$monitor_parent_runner" 2>/dev/null || true
    not_ok "noise monitor refuses when expected parent dies before PDEATHSIG is armed"
fi

# Once PR_SET_PDEATHSIG is armed, parent death must terminate a cell sentinel
# without letting the ordinary authorized-stop handler manufacture MONITOR_OK.
start_stat_updater
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/monitor-parent-death-armed.out" \
    2>"$tmp/monitor-parent-death-armed.err" &
monitor_armed_parent=$!
monitor_armed_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/monitor-parent-death-armed.out" "$monitor_armed_parent"; then
    monitor_armed_ready=1
fi
monitor_armed_child=$(sed -n \
    's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
    "$tmp/monitor-parent-death-armed.out" | head -1)
monitor_armed_child_parent=$(ps -o ppid= -p "$monitor_armed_child" \
    2>/dev/null | tr -d '[:space:]')
monitor_armed_parent_rc=0
[ "$monitor_armed_ready" -ne 1 ] || \
    kill -KILL "$monitor_armed_parent" 2>/dev/null || true
wait "$monitor_armed_parent" 2>/dev/null || monitor_armed_parent_rc=$?
for _ in $(seq 1 300); do
    ! process_is_live "$monitor_armed_child" && break
    sleep 0.01
done
stop_stat_updater
if [ "$monitor_armed_ready" -eq 1 ] && [ "$monitor_armed_parent_rc" -eq 137 ] &&
   [ "$monitor_armed_child_parent" = "$monitor_armed_parent" ] &&
   ! process_is_live "$monitor_armed_child" &&
   ! grep -q '^NOISE_MONITOR_OK mode=cell ' \
        "$tmp/monitor-parent-death-armed.out"; then
    ok "armed PDEATHSIG kills the sentinel without a fabricated valid stop"
else
    [ -z "$monitor_armed_child" ] || \
        kill -KILL "$monitor_armed_child" 2>/dev/null || true
    kill -KILL "$monitor_armed_parent" 2>/dev/null || true
    not_ok "armed PDEATHSIG kills the sentinel without a fabricated valid stop"
fi

# A deliberately wrong externally captured PPID must reach the real child and
# be rejected there.  This dynamic negative control fails if the child mutates
# the snapshot into os.getppid(), or if the shell silently ignores the token.
start_stat_updater
monitor_bad_parent=99999999
monitor_bad_parent_rc=0
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE="$monitor_bad_parent" \
    EXITOS_SCALE_TEST_PAUSE_BEFORE_MONITOR_PDEATHSIG=1 \
    bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/monitor-bad-parent.out" 2>"$tmp/monitor-bad-parent.err" &
monitor_bad_parent_runner=$!
monitor_bad_parent_ready=0
if wait_for_marker '^MONITOR_PRE_PDEATHSIG_READY ' \
        "$tmp/monitor-bad-parent.out" "$monitor_bad_parent_runner"; then
    monitor_bad_parent_ready=1
fi
monitor_bad_parent_child=$(sed -n \
    's/^MONITOR_PRE_PDEATHSIG_READY pid=\([0-9][0-9]*\) expected_parent=.*/\1/p' \
    "$tmp/monitor-bad-parent.out" | head -1)
monitor_bad_parent_seen=$(sed -n \
    's/^MONITOR_PRE_PDEATHSIG_READY .* expected_parent=\([0-9][0-9]*\)$/\1/p' \
    "$tmp/monitor-bad-parent.out" | head -1)
monitor_bad_parent_actual=$(ps -o ppid= -p "$monitor_bad_parent_child" \
    2>/dev/null | tr -d '[:space:]')
[ -z "$monitor_bad_parent_child" ] || \
    kill -CONT "$monitor_bad_parent_child" 2>/dev/null || true
wait "$monitor_bad_parent_runner" || monitor_bad_parent_rc=$?
stop_stat_updater
if [ "$monitor_bad_parent_ready" -eq 1 ] &&
   [ "$monitor_bad_parent_seen" = "$monitor_bad_parent" ] &&
   [ "$monitor_bad_parent_actual" = "$monitor_bad_parent_runner" ] &&
   [ "$monitor_bad_parent_rc" -eq 143 ] &&
   ! grep -q '^NOISE_MONITOR_READY ' "$tmp/monitor-bad-parent.out"; then
    ok "noise monitor dynamically rejects a mutated external PPID snapshot"
else
    [ -z "$monitor_bad_parent_child" ] || \
        kill -KILL "$monitor_bad_parent_child" 2>/dev/null || true
    kill -KILL "$monitor_bad_parent_runner" 2>/dev/null || true
    not_ok "noise monitor dynamically rejects a mutated external PPID snapshot"
fi

start_stat_updater
compiler_monitor_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 440 20 100 15.0 \
    >"$tmp/noise-compiler.out" 2>"$tmp/noise-compiler.err" &
compiler_monitor_pid=$!
compiler_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=preflight ' \
        "$tmp/noise-compiler.out" "$compiler_monitor_pid"; then
    compiler_ready=1
    sleep 0.05
    mkdir "$noise_proc/4242"
    printf '%s\n' 'clang++' >"$noise_proc/4242/comm"
    sleep 0.12
    mv "$noise_proc/4242" "$tmp/compiler-finished"
fi
wait "$compiler_monitor_pid" || compiler_monitor_rc=$?
stop_stat_updater
if [ "$compiler_ready" -eq 1 ] && [ "$compiler_monitor_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-compiler.out" &&
   ! grep -q '^NOISE_EVENT ' "$tmp/noise-compiler.out"; then
    ok "off-pool compiler bursts do not invalidate a locally quiet CPU pool"
else
    not_ok "off-pool compiler bursts do not invalidate a locally quiet CPU pool"
fi

rm -rf -- "$tmp/compiler-finished"
start_stat_updater
mkdir "$noise_proc/6161"
printf '%s\n' fio >"$noise_proc/6161/comm"
preflight_fio_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-fio.out" 2>"$tmp/noise-fio.err" || preflight_fio_rc=$?
mv "$noise_proc/6161" "$tmp/fio-finished"
if [ "$preflight_fio_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-fio.out"; then
    ok "off-pool fio does not invalidate a locally quiet CPU pool"
else
    not_ok "off-pool fio does not invalidate a locally quiet CPU pool"
fi

printf '%s\n' '9.00 0.00 1/1 1' >"$noise_proc/loadavg"
preflight_load1_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-load1.out" 2>"$tmp/noise-load1.err" || preflight_load1_rc=$?
printf '%s\n' '0.00 9.00 1/1 1' >"$noise_proc/loadavg"
preflight_load5_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 260 20 100 15.0 \
    >"$tmp/noise-load5.out" 2>"$tmp/noise-load5.err" || preflight_load5_rc=$?
printf '%s\n' '0.00 0.00 1/1 1' >"$noise_proc/loadavg"
stop_stat_updater
if [ "$preflight_load1_rc" -eq 0 ] && [ "$preflight_load5_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-load1.out" &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-load5.out"; then
    ok "global load1 and load5 do not invalidate a locally quiet CPU pool"
else
    not_ok "global load1 and load5 do not invalidate a locally quiet CPU pool"
fi

start_stat_updater
sibling_monitor_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    preflight "$noise_proc" "$noise_cpu" 2 440 20 100 15.0 \
    >"$tmp/noise-sibling.out" 2>"$tmp/noise-sibling.err" &
sibling_monitor_pid=$!
sibling_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=preflight ' \
        "$tmp/noise-sibling.out" "$sibling_monitor_pid"; then
    sibling_ready=1
    sleep 0.06
    : >"$noise_busy"
    sleep 0.16
    rm -f -- "$noise_busy"
fi
wait "$sibling_monitor_pid" || sibling_monitor_rc=$?
stop_stat_updater
if [ "$sibling_ready" -eq 1 ] && [ "$sibling_monitor_rc" -eq 0 ] &&
   grep -Eq 'CPU_UTIL_REFERENCE_EXCEEDED mode=preflight .*cpu=6([^0-9]|$)' \
        "$tmp/noise-sibling.out" &&
   grep -q '^NOISE_MONITOR_OK mode=preflight ' "$tmp/noise-sibling.out"; then
    ok "continuous preflight records activity on an SMT sibling without rejecting"
else
    not_ok "continuous preflight records activity on an SMT sibling without rejecting"
fi

blocked_monitor_rc=0
start_stat_updater
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/noise-blocked.out" 2>"$tmp/noise-blocked.err" &
blocked_monitor_pid=$!
blocked_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-blocked.out" "$blocked_monitor_pid"; then
    blocked_ready=1
    blocked_monitor_child=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-blocked.out" | head -1)
    : >"$noise_busy.blocked"
    sleep 0.08
    rm -f -- "$noise_busy.blocked"
    kill -TERM "$blocked_monitor_child" 2>/dev/null || true
fi
wait "$blocked_monitor_pid" || blocked_monitor_rc=$?
stop_stat_updater
if [ "$blocked_ready" -eq 1 ] && [ "$blocked_monitor_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-blocked.out" &&
   ! grep -q '^NOISE_EVENT ' "$tmp/noise-blocked.out"; then
    ok "global procs_blocked does not invalidate local per-CPU evidence"
else
    not_ok "global procs_blocked does not invalidate local per-CPU evidence"
fi

start_stat_updater
mkdir "$noise_proc/7171"
printf '%s\n' fio >"$noise_proc/7171/comm"
printf '%s\n' '99.00 99.00 1/1 1' >"$noise_proc/loadavg"
cell_scope_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/noise-cell-scope.out" 2>"$tmp/noise-cell-scope.err" &
cell_scope_pid=$!
cell_scope_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-cell-scope.out" "$cell_scope_pid"; then
    cell_scope_ready=1
    cell_scope_monitor_pid=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-scope.out" | head -1)
    : >"$noise_busy.selected"
    sleep 0.16
    rm -f -- "$noise_busy.selected"
    kill -TERM "$cell_scope_monitor_pid" 2>/dev/null || true
fi
wait "$cell_scope_pid" || cell_scope_rc=$?
stop_stat_updater
mv "$noise_proc/7171" "$tmp/cell-fio-finished"
printf '%s\n' '0.00 0.00 1/1 1' >"$noise_proc/loadavg"
if [ "$cell_scope_ready" -eq 1 ] && [ "$cell_scope_rc" -eq 0 ] &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-scope.out" &&
   grep -q 'CPU_UTIL_WINDOW mode=cell .*cpu=2 role=selected ' \
        "$tmp/noise-cell-scope.out" &&
   ! grep -q '^NOISE_EVENT ' "$tmp/noise-cell-scope.out"; then
    ok "cell records selected CPU load but ignores global fio/load activity"
else
    not_ok "cell records selected CPU load but ignores global fio/load activity"
fi

start_stat_updater
cell_sibling_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 100 15.0 \
    >"$tmp/noise-cell-sibling.out" 2>"$tmp/noise-cell-sibling.err" &
cell_sibling_runner=$!
cell_sibling_ready=0
cell_sibling_monitor=
cell_sibling_state=
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-cell-sibling.out" "$cell_sibling_runner"; then
    cell_sibling_ready=1
    cell_sibling_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-sibling.out" | head -1)
    : >"$noise_busy"
    sleep 0.16
    rm -f -- "$noise_busy"
    for _ in $(seq 1 100); do
        grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=cell .*cpu=6 role=sibling' \
            "$tmp/noise-cell-sibling.out" 2>/dev/null && break
        sleep 0.01
    done
    cell_sibling_state=$(ps -o stat= -p "$cell_sibling_monitor" 2>/dev/null |
        tr -d '[:space:]')
    kill -TERM "$cell_sibling_monitor" 2>/dev/null || true
fi
wait "$cell_sibling_runner" || cell_sibling_rc=$?
stop_stat_updater
if [ "$cell_sibling_ready" -eq 1 ] && [ "$cell_sibling_rc" -eq 0 ] &&
   [[ $cell_sibling_state != *Z* && $cell_sibling_state != *X* ]] &&
   grep -q 'CPU_UTIL_WINDOW mode=cell .*cpu=6 role=sibling ' \
        "$tmp/noise-cell-sibling.out" &&
   grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=cell .*cpu=6 role=sibling' \
        "$tmp/noise-cell-sibling.out" &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-sibling.out"; then
    ok "cell sentinel records utilization on the SMT sibling without rejecting"
else
    [ -z "${cell_sibling_monitor:-}" ] || \
        kill -KILL "$cell_sibling_monitor" 2>/dev/null || true
    not_ok "cell sentinel records utilization on the SMT sibling without rejecting"
fi

cat >"$tmp/fake-cell-io" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' "$$" >"$FAKE_IO_PID_FILE"
sleep "${FAKE_IO_SLEEP:-0.35}"
printf '%s\n' complete >"$FAKE_IO_DONE"
EOF
chmod 0700 "$tmp/fake-cell-io"

# Positive control proves the test-only route really reaches the shared cell
# sentinel orchestration instead of passing through usage or an always-fail
# stub.
rm -f -- "$tmp/cell-clean.done" "$tmp/cell-clean.post" \
    "$tmp/cell-clean.summary" "$tmp/cell-clean.pid"
start_stat_updater
cell_clean_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    FAKE_IO_PID_FILE="$tmp/cell-clean.pid" \
    FAKE_IO_DONE="$tmp/cell-clean.done" FAKE_IO_SLEEP=0.15 \
    bash "$script" --test-cell-noise "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-clean.sentinel" "$tmp/cell-clean.post" \
    "$tmp/cell-clean.summary" "$tmp/fake-cell-io" \
    >"$tmp/cell-clean.out" 2>"$tmp/cell-clean.err" || cell_clean_rc=$?
stop_stat_updater
if [ "$cell_clean_rc" -eq 0 ] && [ -f "$tmp/cell-clean.done" ] &&
   [ -f "$tmp/cell-clean.post" ] && [ -f "$tmp/cell-clean.summary" ] &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/cell-clean.sentinel"; then
    ok "clean timed-cell sentinel reaches command, post gates, and summary"
else
    not_ok "clean timed-cell sentinel reaches command, post gates, and summary"
fi

# A syntactically valid but decreasing CPU snapshot reaches observe_window()
# and must latch cpu_counter_invalid.  Keep the contaminated monitor as the
# live direct child until an authorized test-mode stop; otherwise its saved PID
# can be reaped and reused while fio would still be active.
start_stat_updater
cell_counter_rc=0
env EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-monitor \
    cell "$noise_proc" "$noise_cpu" 2 0 20 200 15.0 \
    >"$tmp/noise-cell-counter.out" 2>"$tmp/noise-cell-counter.err" &
cell_counter_runner=$!
cell_counter_ready=0
cell_counter_latched=0
cell_counter_still_live=0
cell_counter_ticks_delta=999999
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/noise-cell-counter.out" "$cell_counter_runner"; then
    cell_counter_ready=1
    cell_counter_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/noise-cell-counter.out" | head -1)
    cell_counter_parent=$(ps -o ppid= -p "$cell_counter_monitor" 2>/dev/null |
        tr -d '[:space:]')
    stop_stat_updater
    {
        printf '%s\n' 'cpu  0 0 0 0 0 0 0 0 0 0'
        printf '%s\n' 'cpu2 0 0 0 0 0 0 0 0 0 0'
        printf '%s\n' 'cpu6 0 0 0 0 0 0 0 0 0 0'
        printf '%s\n' 'procs_running 1' 'procs_blocked 0'
    } >"$noise_proc/stat.invalid"
    mv -f -- "$noise_proc/stat.invalid" "$noise_proc/stat"
    if wait_for_marker 'NOISE_EVENT kind=cpu_counter_invalid ' \
            "$tmp/noise-cell-counter.out" "$cell_counter_runner"; then
        cell_counter_latched=1
        if process_is_live "$cell_counter_monitor"; then
            cell_counter_ticks_before=$(awk '{print $14+$15}' \
                "/proc/$cell_counter_monitor/stat")
            sleep 0.20
            if process_is_live "$cell_counter_monitor"; then
                cell_counter_still_live=1
                cell_counter_ticks_after=$(awk '{print $14+$15}' \
                    "/proc/$cell_counter_monitor/stat")
                cell_counter_ticks_delta=$((cell_counter_ticks_after - cell_counter_ticks_before))
                kill -TERM "$cell_counter_monitor" 2>/dev/null || true
            fi
        fi
    fi
else
    stop_stat_updater
fi
wait "$cell_counter_runner" || cell_counter_rc=$?
if [ "$cell_counter_ready" -eq 1 ] && [ "$cell_counter_latched" -eq 1 ] &&
   [ "$cell_counter_parent" = "$cell_counter_runner" ] &&
   [ "$cell_counter_still_live" -eq 1 ] &&
   [ "$cell_counter_ticks_delta" -le 3 ] && [ "$cell_counter_rc" -ne 0 ] &&
   ! grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/noise-cell-counter.out"; then
    ok "counter-integrity errors remain owned and low-rate until authorized stop"
else
    [ -z "${cell_counter_monitor:-}" ] || \
        kill -KILL "$cell_counter_monitor" 2>/dev/null || true
    kill -KILL "$cell_counter_runner" 2>/dev/null || true
    not_ok "counter-integrity errors remain owned and low-rate until authorized stop"
fi

# The production orchestration must retain ownership of a sentinel that has
# latched a blind interval until fio ends.  It must then keep raw/post evidence
# and refuse the summary.  An observer that exits at the first event leaves a
# reusable stale PID while the timed command is still running.
rm -f -- "$tmp/cell-gap.done" "$tmp/cell-gap.post" \
    "$tmp/cell-gap.summary" "$tmp/cell-gap.pid" "$tmp/cell-gap.sentinel"
start_stat_updater
cell_gap_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    EXITOS_SCALE_CPU_UTIL_WINDOW_MS=100 \
    EXITOS_SCALE_TEST_SAMPLE_DELAY_MS=1100 \
    FAKE_IO_PID_FILE="$tmp/cell-gap.pid" \
    FAKE_IO_DONE="$tmp/cell-gap.done" FAKE_IO_SLEEP=1.55 \
    bash "$script" --test-cell-noise "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-gap.sentinel" "$tmp/cell-gap.post" \
    "$tmp/cell-gap.summary" "$tmp/fake-cell-io" \
    >"$tmp/cell-gap.out" 2>"$tmp/cell-gap.err" &
cell_gap_runner=$!
cell_gap_latched=0
cell_gap_live_after_latch=0
cell_gap_parent=
if wait_for_marker 'NOISE_EVENT kind=monitor_gap gap_ms=' \
        "$tmp/cell-gap.sentinel" "$cell_gap_runner"; then
    cell_gap_latched=1
    cell_gap_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/cell-gap.sentinel" | head -1)
    cell_gap_parent=$(ps -o ppid= -p "$cell_gap_monitor" 2>/dev/null |
        tr -d '[:space:]')
    process_is_live "$cell_gap_monitor" && cell_gap_live_after_latch=1
fi
wait "$cell_gap_runner" || cell_gap_rc=$?
stop_stat_updater
if [ "$cell_gap_latched" -eq 1 ] && [ "$cell_gap_live_after_latch" -eq 1 ] &&
   [ "$cell_gap_parent" = "$cell_gap_runner" ] && [ "$cell_gap_rc" -ne 0 ] &&
   [ -f "$tmp/cell-gap.done" ] && [ -f "$tmp/cell-gap.post" ] &&
   [ ! -e "$tmp/cell-gap.summary" ] &&
   grep -q 'NOISE_EVENT kind=monitor_gap gap_ms=' "$tmp/cell-gap.sentinel" &&
   grep -q 'timed_cell_noise_detected cell=test-cell' "$tmp/cell-gap.err" &&
   ! process_is_live "${cell_gap_monitor:-}"; then
    ok "timed cell keeps a gapped sentinel owned, then rejects before summary"
else
    [ -z "${cell_gap_monitor:-}" ] || \
        kill -KILL "$cell_gap_monitor" 2>/dev/null || true
    kill -KILL "$cell_gap_runner" 2>/dev/null || true
    not_ok "timed cell keeps a gapped sentinel owned, then rejects before summary"
fi

# An external TERM to the sentinel before the direct-child command completes
# is not the runner's authorized end-of-cell handshake.  Raw I/O and post-gate
# evidence are retained, but the incomplete observation must not yield a row.
rm -f -- "$tmp/cell-early.done" "$tmp/cell-early.post" \
    "$tmp/cell-early.summary" "$tmp/cell-early.pid"
start_stat_updater
cell_early_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    EXITOS_SCALE_TEST_PAUSE_AFTER_SIGNAL_SCAN=1 \
    FAKE_IO_PID_FILE="$tmp/cell-early.pid" \
    FAKE_IO_DONE="$tmp/cell-early.done" FAKE_IO_SLEEP=0.70 \
    bash "$script" --test-cell-noise "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-early.sentinel" "$tmp/cell-early.post" \
    "$tmp/cell-early.summary" "$tmp/fake-cell-io" \
    >"$tmp/cell-early.out" 2>"$tmp/cell-early.err" &
cell_early_runner=$!
cell_early_ready=0
cell_early_latched=0
cell_early_still_live=0
cell_early_ticks_delta=999999
for _ in $(seq 1 300); do
    if grep -q '^NOISE_MONITOR_READY mode=cell ' \
            "$tmp/cell-early.sentinel" 2>/dev/null &&
       grep -q '^NOISE_SIGNAL_SCAN_PASSED mode=cell ' \
            "$tmp/cell-early.sentinel" 2>/dev/null &&
       [ -s "$tmp/cell-early.pid" ]; then
        cell_early_ready=1
        break
    fi
    kill -0 "$cell_early_runner" 2>/dev/null || break
    sleep 0.01
done
if [ "$cell_early_ready" -eq 1 ]; then
    cell_early_monitor=$(sed -n \
        's/^NOISE_MONITOR_READY mode=cell pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/cell-early.sentinel" | head -1)
    cell_early_io=$(cat "$tmp/cell-early.pid")
    cell_early_parent=$(ps -o ppid= -p "$cell_early_monitor" 2>/dev/null |
        tr -d '[:space:]')
    kill -TERM "$cell_early_monitor" 2>/dev/null || true
    kill -CONT "$cell_early_monitor" 2>/dev/null || true
    if wait_for_marker '^NOISE_EVENT kind=unauthorized_stop mode=cell ' \
            "$tmp/cell-early.sentinel" "$cell_early_runner" &&
       process_is_live "$cell_early_monitor"; then
        cell_early_latched=1
        cell_early_ticks_before=$(awk '{print $14+$15}' \
            "/proc/$cell_early_monitor/stat")
        sleep 0.20
        if process_is_live "$cell_early_monitor" &&
           process_is_live "$cell_early_io"; then
            cell_early_still_live=1
            cell_early_ticks_after=$(awk '{print $14+$15}' \
                "/proc/$cell_early_monitor/stat")
            cell_early_ticks_delta=$((cell_early_ticks_after - cell_early_ticks_before))
        fi
    fi
fi
wait "$cell_early_runner" || cell_early_rc=$?
stop_stat_updater
if [ "$cell_early_ready" -eq 1 ] && [ "$cell_early_latched" -eq 1 ] &&
   [ "$cell_early_still_live" -eq 1 ] &&
   [ "$cell_early_parent" = "$cell_early_runner" ] &&
   [ "$cell_early_ticks_delta" -le 3 ] && [ "$cell_early_rc" -ne 0 ] &&
   [ -f "$tmp/cell-early.done" ] && [ -f "$tmp/cell-early.post" ] &&
   [ ! -e "$tmp/cell-early.summary" ] &&
   grep -q '^NOISE_EVENT kind=unauthorized_stop mode=cell ' \
        "$tmp/cell-early.sentinel" &&
   { [ -z "${cell_early_monitor:-}" ] || \
       ! kill -0 "$cell_early_monitor" 2>/dev/null; }; then
    ok "unauthorized stop remains owned and low-rate until post-gate refusal"
else
    [ -z "${cell_early_monitor:-}" ] || \
        kill -KILL "$cell_early_monitor" 2>/dev/null || true
    kill -KILL "$cell_early_runner" 2>/dev/null || true
    not_ok "unauthorized stop remains owned and low-rate until post-gate refusal"
fi

rm -f -- "$tmp/cell-noisy.done" "$tmp/cell-noisy.post" \
    "$tmp/cell-noisy.summary" "$tmp/cell-noisy.pid"
start_stat_updater
cell_noisy_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    EXITOS_SCALE_CPU_UTIL_WINDOW_MS=100 \
    FAKE_IO_PID_FILE="$tmp/cell-noisy.pid" \
    FAKE_IO_DONE="$tmp/cell-noisy.done" FAKE_IO_SLEEP=0.60 \
    bash "$script" --test-cell-noise "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-noisy.sentinel" "$tmp/cell-noisy.post" \
    "$tmp/cell-noisy.summary" "$tmp/fake-cell-io" \
    >"$tmp/cell-noisy.out" 2>"$tmp/cell-noisy.err" &
cell_noisy_pid=$!
cell_noisy_ready=0
if wait_for_marker '^NOISE_MONITOR_READY mode=cell ' \
        "$tmp/cell-noisy.sentinel" "$cell_noisy_pid"; then
    cell_noisy_ready=1
    : >"$noise_busy"
    sleep 0.16
    rm -f -- "$noise_busy"
fi
cell_sentinel_pid=$(sed -n 's/^NOISE_MONITOR_READY .* pid=\([0-9][0-9]*\) .*/\1/p' \
    "$tmp/cell-noisy.sentinel" | head -1)
cell_noisy_io_pid=$(cat "$tmp/cell-noisy.pid" 2>/dev/null || true)
for _ in $(seq 1 100); do
    grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=cell .*cpu=6 role=sibling' \
        "$tmp/cell-noisy.sentinel" 2>/dev/null && break
    sleep 0.01
done
cell_sentinel_state=$(ps -o stat= -p "$cell_sentinel_pid" 2>/dev/null |
    tr -d '[:space:]')
cell_sentinel_parent=$(ps -o ppid= -p "$cell_sentinel_pid" 2>/dev/null |
    tr -d '[:space:]')
wait "$cell_noisy_pid" || cell_noisy_rc=$?
stop_stat_updater
if [ "$cell_noisy_ready" -eq 1 ] && [ "$cell_noisy_rc" -eq 0 ] &&
   [ "$cell_sentinel_parent" = "$cell_noisy_pid" ] &&
   [[ $cell_sentinel_state != *Z* && $cell_sentinel_state != *X* ]] &&
   [ -f "$tmp/cell-noisy.done" ] && [ -f "$tmp/cell-noisy.post" ] &&
   [ -f "$tmp/cell-noisy.summary" ] &&
   grep -q 'CPU_UTIL_REFERENCE_EXCEEDED mode=cell .*cpu=6 role=sibling' \
        "$tmp/cell-noisy.sentinel" &&
   grep -q '^NOISE_MONITOR_OK mode=cell ' "$tmp/cell-noisy.sentinel" &&
   ! grep -q '^NOISE_EVENT kind=cpu_busy ' "$tmp/cell-noisy.sentinel" &&
   { [ -z "$cell_sentinel_pid" ] || ! kill -0 "$cell_sentinel_pid" 2>/dev/null; }; then
    ok "cell utilization waits for I/O and post gates, preserves evidence, and permits summary"
else
    not_ok "cell utilization waits for I/O and post gates, preserves evidence, and permits summary"
fi

# SIGTERM must reap both the monitored command and the sentinel.  The command
# execs sleep so the recorded PID is the process that cleanup must terminate.
cat >"$tmp/fake-long-cell-io" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' "$$" >"$FAKE_IO_PID_FILE"
exec sleep 30
EOF
chmod 0700 "$tmp/fake-long-cell-io"
cat >"$tmp/reset-signal-launcher" <<'EOF'
#!/usr/bin/env python3
import os
import signal
import sys

for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(signum, signal.SIG_DFL)
os.execvpe(sys.argv[1], sys.argv[1:], os.environ)
EOF
chmod 0700 "$tmp/reset-signal-launcher"
rm -f -- "$tmp/cell-signal.pid" "$tmp/cell-signal.post" \
    "$tmp/cell-signal.summary"
start_stat_updater
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    FAKE_IO_PID_FILE="$tmp/cell-signal.pid" \
    FAKE_IO_DONE="$tmp/cell-signal.done" \
    "$tmp/reset-signal-launcher" bash "$script" --test-cell-noise \
    "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-signal.sentinel" "$tmp/cell-signal.post" \
    "$tmp/cell-signal.summary" "$tmp/fake-long-cell-io" \
    >"$tmp/cell-signal.out" 2>"$tmp/cell-signal.err" &
cell_signal_runner=$!
cell_signal_ready=0
for _ in $(seq 1 300); do
    if grep -q '^NOISE_MONITOR_READY mode=cell ' \
            "$tmp/cell-signal.sentinel" 2>/dev/null &&
       [ -s "$tmp/cell-signal.pid" ]; then
        cell_signal_ready=1
        break
    fi
    kill -0 "$cell_signal_runner" 2>/dev/null || break
    sleep 0.01
done
signal_sentinel_pid=$(sed -n 's/^NOISE_MONITOR_READY .* pid=\([0-9][0-9]*\) .*/\1/p' \
    "$tmp/cell-signal.sentinel" 2>/dev/null | head -1)
signal_sentinel_parent=$(ps -o ppid= -p "$signal_sentinel_pid" 2>/dev/null |
    tr -d '[:space:]')
signal_io_pid=$(cat "$tmp/cell-signal.pid" 2>/dev/null || true)
signal_io_parent=$(ps -o ppid= -p "$signal_io_pid" 2>/dev/null |
    tr -d '[:space:]')
cell_signal_rc=0
if [ "$cell_signal_ready" -eq 1 ]; then
    kill -TERM "$cell_signal_runner" 2>/dev/null || true
fi
wait "$cell_signal_runner" || cell_signal_rc=$?
stop_stat_updater
if [ "$cell_signal_ready" -eq 1 ] && [ "$cell_signal_rc" -eq 143 ] &&
   [ -n "$signal_io_pid" ] && [ -n "$signal_sentinel_pid" ] &&
   [ "$signal_io_parent" = "$cell_signal_runner" ] &&
   [ "$signal_sentinel_parent" = "$cell_signal_runner" ] &&
   ! kill -0 "$signal_io_pid" 2>/dev/null &&
   ! kill -0 "$signal_sentinel_pid" 2>/dev/null; then
    ok "SIGTERM reaps timed command and noise sentinel without orphans"
else
    [ -z "$signal_io_pid" ] || kill -TERM "$signal_io_pid" 2>/dev/null || true
    [ -z "$signal_sentinel_pid" ] || kill -TERM "$signal_sentinel_pid" 2>/dev/null || true
    kill -TERM "$cell_signal_runner" 2>/dev/null || true
    not_ok "SIGTERM reaps timed command and noise sentinel without orphans"
fi

extra_signals_ok=1
for signal_case in HUP:129 INT:130; do
    signal_name=${signal_case%%:*}
    signal_expected=${signal_case##*:}
    signal_tag=$(tr '[:upper:]' '[:lower:]' <<<"$signal_name")
    rm -f -- "$tmp/cell-$signal_tag.pid" "$tmp/cell-$signal_tag.post" \
        "$tmp/cell-$signal_tag.summary" "$tmp/cell-$signal_tag.sentinel"
    start_stat_updater
    env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
        FAKE_IO_PID_FILE="$tmp/cell-$signal_tag.pid" \
        FAKE_IO_DONE="$tmp/cell-$signal_tag.done" \
        "$tmp/reset-signal-launcher" bash "$script" --test-cell-noise \
        "$noise_proc" "$noise_cpu" 2 \
        "$tmp/cell-$signal_tag.sentinel" "$tmp/cell-$signal_tag.post" \
        "$tmp/cell-$signal_tag.summary" "$tmp/fake-long-cell-io" \
        >"$tmp/cell-$signal_tag.out" 2>"$tmp/cell-$signal_tag.err" &
    extra_runner=$!
    extra_ready=0
    for _ in $(seq 1 300); do
        if grep -q '^NOISE_MONITOR_READY mode=cell ' \
                "$tmp/cell-$signal_tag.sentinel" 2>/dev/null &&
           [ -s "$tmp/cell-$signal_tag.pid" ]; then
            extra_ready=1
            break
        fi
        kill -0 "$extra_runner" 2>/dev/null || break
        sleep 0.01
    done
    extra_sentinel=$(sed -n \
        's/^NOISE_MONITOR_READY .* pid=\([0-9][0-9]*\) .*/\1/p' \
        "$tmp/cell-$signal_tag.sentinel" 2>/dev/null | head -1)
    extra_io=$(cat "$tmp/cell-$signal_tag.pid" 2>/dev/null || true)
    extra_sentinel_parent=$(ps -o ppid= -p "$extra_sentinel" 2>/dev/null |
        tr -d '[:space:]')
    extra_io_parent=$(ps -o ppid= -p "$extra_io" 2>/dev/null |
        tr -d '[:space:]')
    extra_rc=0
    [ "$extra_ready" -ne 1 ] || kill -"$signal_name" "$extra_runner" 2>/dev/null || true
    wait "$extra_runner" || extra_rc=$?
    stop_stat_updater
    for _ in $(seq 1 200); do
        ! kill -0 "$extra_sentinel" 2>/dev/null &&
            ! kill -0 "$extra_io" 2>/dev/null && break
        sleep 0.01
    done
    if [ "$extra_ready" -ne 1 ] || [ "$extra_rc" -ne "$signal_expected" ] ||
       [ "$extra_sentinel_parent" != "$extra_runner" ] ||
       [ "$extra_io_parent" != "$extra_runner" ] ||
       kill -0 "$extra_sentinel" 2>/dev/null || kill -0 "$extra_io" 2>/dev/null; then
        extra_signals_ok=0
        [ -z "$extra_sentinel" ] || kill -KILL "$extra_sentinel" 2>/dev/null || true
        [ -z "$extra_io" ] || kill -KILL "$extra_io" 2>/dev/null || true
        kill -KILL "$extra_runner" 2>/dev/null || true
    fi
done
if [ "$extra_signals_ok" -eq 1 ]; then
    ok "HUP and INT reap direct-child timed command and sentinel"
else
    not_ok "HUP and INT reap direct-child timed command and sentinel"
fi

# fio process mode can form a descendant tree.  Cleanup must own the entire
# private process group, not merely the direct master PID.  The worker and leaf
# below ignore TERM so only the runner's group-wide KILL fallback can stop them.
cat >"$tmp/fake-forking-cell-io" <<'EOF'
#!/usr/bin/env python3
import os
import signal

def record(path, value):
    with open(path, "w", encoding="ascii") as output:
        output.write("%d\n" % value)

record(os.environ["FAKE_IO_PID_FILE"], os.getpid())
worker = os.fork()
if worker == 0:
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, signal.SIG_IGN)
    leaf = os.fork()
    if leaf == 0:
        record(os.environ["FAKE_IO_LEAF_PID_FILE"], os.getpid())
        while True:
            signal.pause()
    record(os.environ["FAKE_IO_WORKER_PID_FILE"], os.getpid())
    while True:
        signal.pause()
while True:
    signal.pause()
EOF
chmod 0700 "$tmp/fake-forking-cell-io"
rm -f -- "$tmp/cell-fork.pid" "$tmp/cell-fork.worker" "$tmp/cell-fork.leaf" \
    "$tmp/cell-fork.post" "$tmp/cell-fork.summary"
start_stat_updater
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_NOISE_POLL_MS=20 \
    FAKE_IO_PID_FILE="$tmp/cell-fork.pid" \
    FAKE_IO_WORKER_PID_FILE="$tmp/cell-fork.worker" \
    FAKE_IO_LEAF_PID_FILE="$tmp/cell-fork.leaf" \
    FAKE_IO_DONE="$tmp/cell-fork.done" \
    "$tmp/reset-signal-launcher" bash "$script" --test-cell-noise \
    "$noise_proc" "$noise_cpu" 2 \
    "$tmp/cell-fork.sentinel" "$tmp/cell-fork.post" \
    "$tmp/cell-fork.summary" "$tmp/fake-forking-cell-io" \
    >"$tmp/cell-fork.out" 2>"$tmp/cell-fork.err" &
cell_fork_runner=$!
cell_fork_ready=0
for _ in $(seq 1 300); do
    if grep -q '^NOISE_MONITOR_READY mode=cell ' \
            "$tmp/cell-fork.sentinel" 2>/dev/null &&
       [ -s "$tmp/cell-fork.pid" ] && [ -s "$tmp/cell-fork.worker" ] &&
       [ -s "$tmp/cell-fork.leaf" ]; then
        cell_fork_ready=1
        break
    fi
    kill -0 "$cell_fork_runner" 2>/dev/null || break
    sleep 0.01
done
cell_fork_direct=$(cat "$tmp/cell-fork.pid" 2>/dev/null || true)
cell_fork_worker=$(cat "$tmp/cell-fork.worker" 2>/dev/null || true)
cell_fork_leaf=$(cat "$tmp/cell-fork.leaf" 2>/dev/null || true)
cell_fork_direct_parent=$(ps -o ppid= -p "$cell_fork_direct" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_worker_parent=$(ps -o ppid= -p "$cell_fork_worker" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_leaf_parent=$(ps -o ppid= -p "$cell_fork_leaf" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_direct_pgid=$(ps -o pgid= -p "$cell_fork_direct" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_worker_pgid=$(ps -o pgid= -p "$cell_fork_worker" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_leaf_pgid=$(ps -o pgid= -p "$cell_fork_leaf" 2>/dev/null |
    tr -d '[:space:]')
cell_fork_rc=0
[ "$cell_fork_ready" -ne 1 ] || kill -TERM "$cell_fork_runner" 2>/dev/null || true
wait "$cell_fork_runner" || cell_fork_rc=$?
stop_stat_updater
for _ in $(seq 1 300); do
    ! process_is_live "$cell_fork_direct" &&
        ! process_is_live "$cell_fork_worker" &&
        ! process_is_live "$cell_fork_leaf" && break
    sleep 0.01
done
if [ "$cell_fork_ready" -eq 1 ] && [ "$cell_fork_rc" -eq 143 ] &&
   [ "$cell_fork_direct_parent" = "$cell_fork_runner" ] &&
   [ "$cell_fork_worker_parent" = "$cell_fork_direct" ] &&
   [ "$cell_fork_leaf_parent" = "$cell_fork_worker" ] &&
   [ "$cell_fork_direct_pgid" = "$cell_fork_direct" ] &&
   [ "$cell_fork_worker_pgid" = "$cell_fork_direct" ] &&
   [ "$cell_fork_leaf_pgid" = "$cell_fork_direct" ] &&
   ! process_is_live "$cell_fork_direct" &&
   ! process_is_live "$cell_fork_worker" &&
   ! process_is_live "$cell_fork_leaf"; then
    ok "private process-group cleanup kills a stubborn process-mode descendant tree"
else
    [ -z "$cell_fork_direct" ] || \
        kill -KILL -- "-$cell_fork_direct" 2>/dev/null || true
    [ -z "$cell_fork_direct" ] || kill -KILL "$cell_fork_direct" 2>/dev/null || true
    [ -z "$cell_fork_worker" ] || kill -KILL "$cell_fork_worker" 2>/dev/null || true
    [ -z "$cell_fork_leaf" ] || kill -KILL "$cell_fork_leaf" 2>/dev/null || true
    kill -KILL "$cell_fork_runner" 2>/dev/null || true
    not_ok "private process-group cleanup kills a stubborn process-mode descendant tree"
fi

# Kill the runner while its bound child is deliberately stopped before it can
# arm PR_SET_PDEATHSIG.  The expected-parent token must make the child refuse
# after resume rather than accepting init/subreaper as its new parent.
cat >"$tmp/fake-parent-death-command" <<'EOF'
#!/bin/bash
exec sleep 30
EOF
chmod 0700 "$tmp/fake-parent-death-command"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_BEFORE_PDEATHSIG=1 \
    bash "$script" --test-bound-parent-death "$tmp/fake-parent-death-command" \
    >"$tmp/bound-parent-death.out" 2>"$tmp/bound-parent-death.err" &
bound_parent_runner=$!
bound_prearm_ready=0
for _ in $(seq 1 300); do
    if grep -q '^BOUND_PRE_PDEATHSIG_READY pid=[0-9][0-9]* expected_parent=[0-9][0-9]*$' \
            "$tmp/bound-parent-death.out" 2>/dev/null; then
        bound_prearm_ready=1
        break
    fi
    kill -0 "$bound_parent_runner" 2>/dev/null || break
    sleep 0.01
done
bound_prearm_child=$(sed -n \
    's/^BOUND_PRE_PDEATHSIG_READY pid=\([0-9][0-9]*\) expected_parent=.*/\1/p' \
    "$tmp/bound-parent-death.out" 2>/dev/null | head -1)
bound_prearm_expected=$(sed -n \
    's/^BOUND_PRE_PDEATHSIG_READY .* expected_parent=\([0-9][0-9]*\)$/\1/p' \
    "$tmp/bound-parent-death.out" 2>/dev/null | head -1)
bound_prearm_parent=$(ps -o ppid= -p "$bound_prearm_child" 2>/dev/null |
    tr -d '[:space:]')
bound_parent_rc=0
if [ "$bound_prearm_ready" -eq 1 ]; then
    kill -KILL "$bound_parent_runner" 2>/dev/null || true
fi
wait "$bound_parent_runner" 2>/dev/null || bound_parent_rc=$?
[ -z "$bound_prearm_child" ] || kill -CONT "$bound_prearm_child" 2>/dev/null || true
bound_child_live=0
if [ -n "$bound_prearm_child" ]; then
    bound_child_live=1
    for _ in $(seq 1 300); do
        if [ ! -r "/proc/$bound_prearm_child/stat" ]; then
            bound_child_live=0
            break
        fi
        bound_child_state=$(awk '{print $3}' "/proc/$bound_prearm_child/stat" 2>/dev/null || true)
        case "$bound_child_state" in Z|X|'') bound_child_live=0; break;; esac
        sleep 0.01
    done
fi
if [ "$bound_prearm_ready" -eq 1 ] && [ "$bound_parent_rc" -eq 137 ] &&
   [ "$bound_prearm_expected" = "$bound_parent_runner" ] &&
   [ "$bound_prearm_parent" = "$bound_parent_runner" ] &&
   [ "$bound_child_live" -eq 0 ]; then
    ok "bound child refuses when expected parent dies before PDEATHSIG is armed"
else
    [ -z "$bound_prearm_child" ] || kill -KILL "$bound_prearm_child" 2>/dev/null || true
    kill -KILL "$bound_parent_runner" 2>/dev/null || true
    not_ok "bound child refuses when expected parent dies before PDEATHSIG is armed"
fi

cat >"$tmp/fake-bound-snapshot-command" <<'EOF'
#!/bin/bash
printf '%s\n' executed >"$BOUND_SNAPSHOT_MARKER"
EOF
chmod 0700 "$tmp/fake-bound-snapshot-command"
bound_bad_parent=99999999
bound_bad_parent_rc=0
env EXITOS_SCALE_TEST_MODE=1 \
    EXITOS_SCALE_TEST_EXPECTED_PARENT_OVERRIDE="$bound_bad_parent" \
    EXITOS_SCALE_TEST_PAUSE_BEFORE_PDEATHSIG=1 \
    BOUND_SNAPSHOT_MARKER="$tmp/bound-bad-parent.executed" \
    bash "$script" --test-bound-parent-death "$tmp/fake-bound-snapshot-command" \
    >"$tmp/bound-bad-parent.out" 2>"$tmp/bound-bad-parent.err" &
bound_bad_parent_runner=$!
bound_bad_parent_ready=0
if wait_for_marker '^BOUND_PRE_PDEATHSIG_READY ' \
        "$tmp/bound-bad-parent.out" "$bound_bad_parent_runner"; then
    bound_bad_parent_ready=1
fi
bound_bad_parent_child=$(sed -n \
    's/^BOUND_PRE_PDEATHSIG_READY pid=\([0-9][0-9]*\) expected_parent=.*/\1/p' \
    "$tmp/bound-bad-parent.out" | head -1)
bound_bad_parent_seen=$(sed -n \
    's/^BOUND_PRE_PDEATHSIG_READY .* expected_parent=\([0-9][0-9]*\)$/\1/p' \
    "$tmp/bound-bad-parent.out" | head -1)
bound_bad_parent_actual=$(ps -o ppid= -p "$bound_bad_parent_child" \
    2>/dev/null | tr -d '[:space:]')
[ -z "$bound_bad_parent_child" ] || \
    kill -CONT "$bound_bad_parent_child" 2>/dev/null || true
wait "$bound_bad_parent_runner" || bound_bad_parent_rc=$?
if [ "$bound_bad_parent_ready" -eq 1 ] &&
   [ "$bound_bad_parent_seen" = "$bound_bad_parent" ] &&
   [ "$bound_bad_parent_actual" = "$bound_bad_parent_runner" ] &&
   [ "$bound_bad_parent_rc" -eq 143 ] &&
   [ ! -e "$tmp/bound-bad-parent.executed" ]; then
    ok "bound child dynamically rejects a mutated external PPID snapshot"
else
    [ -z "$bound_bad_parent_child" ] || \
        kill -KILL "$bound_bad_parent_child" 2>/dev/null || true
    kill -KILL "$bound_bad_parent_runner" 2>/dev/null || true
    not_ok "bound child dynamically rejects a mutated external PPID snapshot"
fi

expect_ok "bound launcher validates expected parent before every object open" \
    bash -c '
        body=$(sed -n '\''/^exec_bound_objects()/,/^}$/p'\'' "$1")
        before=$(grep -n "parent_before_prctl" <<<"$body" | head -1 | cut -d: -f1)
        prctl=$(grep -n "libc.prctl" <<<"$body" | head -1 | cut -d: -f1)
        after=$(grep -n "parent_after_prctl" <<<"$body" | head -1 | cut -d: -f1)
        first_open=$(grep -n "os.open" <<<"$body" | head -1 | cut -d: -f1)
        test -n "$before" -a -n "$prctl" -a -n "$after" -a -n "$first_open" &&
        test "$before" -lt "$prctl" -a "$prctl" -lt "$after" -a "$after" -lt "$first_open"
    ' bash "$script"
expect_ok "noise monitor validates expected parent before READY and sampling" \
    bash -c '
        body=$(sed -n '\''/^continuous_noise_monitor()/,/^}$/p'\'' "$1")
        before=$(grep -n "monitor_parent_before_prctl" <<<"$body" | head -1 | cut -d: -f1)
        prctl=$(grep -n "libc.prctl" <<<"$body" | head -1 | cut -d: -f1)
        after=$(grep -n "monitor_parent_after_prctl" <<<"$body" | head -1 | cut -d: -f1)
        sample=$(grep -n "initial_event, baseline" <<<"$body" | head -1 | cut -d: -f1)
        ready=$(grep -n "NOISE_MONITOR_READY" <<<"$body" | head -1 | cut -d: -f1)
        test -n "$before" -a -n "$prctl" -a -n "$after" -a -n "$sample" -a -n "$ready" &&
        test "$before" -lt "$prctl" -a "$prctl" -lt "$after" -a \
             "$after" -lt "$sample" -a "$sample" -lt "$ready"
    ' bash "$script"

cat >"$tmp/fake-priority-command" <<'EOF'
#!/bin/bash
exec python3 - "$PRIORITY_COMMAND_OBS" <<'PY'
import os,sys
with open("/proc/self/stat","r",encoding="ascii") as source:
    value=source.read()
tail=value[value.rfind(")")+2:].split()
nice=int(tail[16])
affinity=",".join(str(cpu) for cpu in sorted(os.sched_getaffinity(0)))
with open(sys.argv[1],"w",encoding="ascii") as output:
    output.write("pid=%d ppid=%d pgid=%d sid=%d policy=%d nice=%d affinity=%s\n" %
                 (os.getpid(),os.getppid(),os.getpgrp(),os.getsid(0),
                  os.sched_getscheduler(0),nice,affinity))
PY
EOF
chmod 0700 "$tmp/fake-priority-command"
priority_rc=0
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_DELAY_AFTER_BOUND_READY_MS=200 \
    PRIORITY_COMMAND_OBS="$tmp/priority-command.obs" \
    chrt --batch 0 bash "$script" --test-priority-launch 2 "$tmp/priority-child.log" \
    "$tmp/fake-priority-command" >"$tmp/priority-parent.out" \
    2>"$tmp/priority-parent.err" || priority_rc=$?
priority_child=$(sed -n \
    's/^IO_PRIORITY_VERIFIED pid=\([0-9][0-9]*\) .*/\1/p' \
    "$tmp/priority-parent.out" | head -1)
if [ "$priority_rc" -eq 0 ] && [ -n "$priority_child" ] &&
   grep -q "^IO_PRIORITY_VERIFIED pid=$priority_child ppid=[0-9][0-9]* pgid=$priority_child sid=$priority_child policy=0 nice=-20 affinity=2$" \
        "$tmp/priority-parent.out" &&
   grep -q "^BOUND_IO_READY pid=$priority_child expected_parent=[0-9][0-9]* pgid=$priority_child sid=$priority_child policy=0 nice=-20 affinity=2$" \
        "$tmp/priority-child.log" &&
   grep -q "^pid=$priority_child ppid=[0-9][0-9]* pgid=$priority_child sid=$priority_child policy=0 nice=-20 affinity=2$" \
        "$tmp/priority-command.obs"; then
    ok "READY proves private process group, direct child, SCHED_OTHER, nice -20, and affinity"
else
    not_ok "READY proves private process group, direct child, SCHED_OTHER, nice -20, and affinity"
fi

expect_ok "production verifies priority via proc after READY before continuing I/O" \
    bash -c '
        cell=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        prepare=$(sed -n '\''/^prepare_files()/,/^}$/p'\'' "$1")
        verify=$(sed -n '\''/^verify_bound_io_ready()/,/^}$/p'\'' "$1")
        grep -q "start_bound_io" <<<"$cell" && grep -q "start_bound_io" <<<"$prepare" &&
        grep -q "/proc/.*stat" <<<"$verify" && grep -q "/proc/.*sched" <<<"$verify" &&
        grep -q "/proc/.*status" <<<"$verify" &&
        ! grep -q "nice -n" <<<"$cell$prepare"
    ' bash "$script"
expect_ok "READY verifier waits for the direct child to enter stopped state" \
    bash -c '
        verify=$(sed -n '\''/^verify_bound_io_ready()/,/^}$/p'\'' "$1")
        grep -q "BOUND_IO_READY" <<<"$verify" &&
        grep -q "State:" <<<"$verify" &&
        grep -Eq "T.*t|t.*T" <<<"$verify" &&
        grep -q "pgrp != pid" <<<"$verify" && grep -q "session != pid" <<<"$verify"
    ' bash "$script"
if rg -q 'SCHED_(FIFO|RR)' "$script"; then
    not_ok "runner never selects realtime FIFO or RR scheduling"
else
    ok "runner never selects realtime FIFO or RR scheduling"
fi

if sed -n '/^continuous_noise_monitor()/,/^}$/p' "$script" |
        rg -q 'foreign_build|foreign_fio|load1|load5|procs_blocked'; then
    not_ok "continuous gate ignores global process, load, and blocked signals"
else
    ok "continuous gate ignores global process, load, and blocked signals"
fi
has 'NOISE_POLL_MS' "production noise polling interval is explicit and bounded"
noise_defaults=$(env -u EXITOS_SCALE_QUIET_SAMPLES \
    -u EXITOS_SCALE_QUIET_INTERVAL -u EXITOS_SCALE_NOISE_POLL_MS \
    EXITOS_SCALE_TEST_MODE=1 bash "$script" --test-noise-defaults 2>/dev/null || true)
expect_ok "production defaults provide exactly 600 seconds with at most one-second blindness" \
    test "$noise_defaults" = \
    'NOISE_DEFAULTS duration_ms=600000 poll_ms=500 max_blind_ms=1000 idle_window_ms=5000 util_max=15.0'
expect_ok "production preflight and cell use continuous noise monitoring" \
    bash -c '
        quiet=$(sed -n '\''/^quiet_window()/,/^}$/p'\'' "$1")
        cell=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        grep -q "continuous_noise_monitor preflight" <<<"$quiet" &&
        grep -q "start_cell_noise_sentinel" <<<"$cell"
    ' bash "$script"
expect_ok "production verifies the cell monitor after wait and post gates, before summary" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        wait_line=$(grep -n '\''wait "$CELL_IO_PID"'\'' <<<"$body" | head -1 | cut -d: -f1)
        workset=$(grep -n "verify_workset post-cell" <<<"$body" | head -1 | cut -d: -f1)
        variants=$(grep -n "assert_scale_variants" <<<"$body" | tail -1 | cut -d: -f1)
        identity=$(grep -n "resolve_identity" <<<"$body" | tail -1 | cut -d: -f1)
        generation=$(grep -n "assert_campaign_generation post-cell" <<<"$body" | head -1 | cut -d: -f1)
        monitor=$(grep -n "verify_cell_monitor" <<<"$body" | head -1 | cut -d: -f1)
        summary=$(grep -n "summarize_cell" <<<"$body" | head -1 | cut -d: -f1)
        test -n "$wait_line" -a -n "$workset" -a -n "$variants" -a \
             -n "$identity" -a -n "$generation" -a \
             -n "$monitor" -a -n "$summary" &&
        test "$wait_line" -lt "$workset" -a "$workset" -lt "$variants" -a \
             "$variants" -lt "$identity" -a \
             "$identity" -lt "$generation" -a "$generation" -lt "$monitor" -a \
             "$monitor" -lt "$summary"
    ' bash "$script"
expect_ok "timed bound launcher execs directly so signal cleanup owns the real fio PID" \
    bash -c '
        bound=$(sed -n '\''/^exec_bound_objects()/,/^}$/p'\'' "$1")
        prepare=$(sed -n '\''/^prepare_files()/,/^}$/p'\'' "$1")
        cell=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        grep -q "exec python3" <<<"$bound" &&
        grep -q "start_bound_io" <<<"$prepare" &&
        grep -q "start_bound_io" <<<"$cell"
    ' bash "$script"

# Authoritative per-cell evidence is exercised only against synthetic procfs
# and sysfs trees.  The test entry runs an inert helper in place of fio; no
# device, mount, or real fio command is reachable from these fixtures.
make_cell_evidence_fixture()
{
    local name=$1
    evidence_root=$tmp/evidence-$name
    evidence_cell=$evidence_root/cell
    evidence_proc=$evidence_root/proc
    evidence_sys=$evidence_root/sys
    evidence_cpu=$evidence_root/cpu
    evidence_ctrl=$evidence_root/controller/nvme-test
    mkdir -m 0700 -p "$evidence_cell" "$evidence_proc/sys/kernel/random" \
        "$evidence_sys/block/fake0/mq/0" "$evidence_sys/block/fake0/mq/1" \
        "$evidence_sys/block/fake0/queue" \
        "$evidence_sys/module/nvme/parameters" \
        "$evidence_root/pci/0000:aa:00.0" "$evidence_ctrl" "$evidence_cpu"
    ln -s "$evidence_root/pci/0000:aa:00.0" "$evidence_ctrl/device"
    printf '%s\n' '249:0' >"$evidence_ctrl/dev"
    printf '%s\n' 11111111-2222-3333-4444-555555555555 \
        >"$evidence_proc/sys/kernel/random/boot_id"
    printf '%s\n' '259 0 fake0 10 0 20 30 40 0 50 60 0 0 0 0 0 0 0 0 0' \
        >"$evidence_proc/diskstats"
    printf '%s\n' '259:0' >"$evidence_sys/block/fake0/dev"
    printf '%s\n' '77' >"$evidence_sys/block/fake0/diskseq"
    printf '%s\n' '2,6' >"$evidence_sys/block/fake0/mq/0/cpu_list"
    printf '%s\n' '0-1' >"$evidence_sys/block/fake0/mq/1/cpu_list"
    printf '%s\n' 4 >"$evidence_sys/module/nvme/parameters/poll_queues"
    printf '%s\n' 1 >"$evidence_sys/block/fake0/queue/io_poll"
}

run_cell_evidence_fixture()
{
    env EXITOS_SCALE_TEST_MODE=1 \
        EVIDENCE_PROC="$evidence_proc" EVIDENCE_SYS="$evidence_sys" \
        EVIDENCE_CPU="$evidence_cpu" EVIDENCE_CELL="$evidence_cell" \
        EVIDENCE_SUCCESS_COMMAND="${EVIDENCE_SUCCESS_COMMAND:-}" \
        EVIDENCE_SUMMARY_MODE="${EVIDENCE_SUMMARY_MODE:-}" \
        EVIDENCE_GLOBAL_SUMMARY="${EVIDENCE_GLOBAL_SUMMARY:-}" \
        bash "$script" --test-cell-evidence "$evidence_cell" \
        "$evidence_proc" "$evidence_sys" "$evidence_cpu" "$evidence_ctrl" \
        fake0 nvme-test 0000:aa:00.0 249:0 2 "$1"
}

cat >"$tmp/evidence-success-command" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' '259 0 fake0 11 0 22 33 44 0 55 66 0 0 0 0 0 0 0 0 0' \
    >"$EVIDENCE_PROC/diskstats"
EOF
chmod 0700 "$tmp/evidence-success-command"

make_cell_evidence_fixture clean
evidence_clean_out=$tmp/evidence-clean.out
evidence_clean_rc=0
run_cell_evidence_fixture "$tmp/evidence-success-command" \
    >"$evidence_clean_out" 2>"$tmp/evidence-clean.err" || evidence_clean_rc=$?
expect_ok "clean cell captures authoritative evidence and seals once" \
    bash -c '
        test "$1" -eq 0 || exit 1
        for leaf in diskstats-pre.txt diskstats-post.txt poll-pre.txt poll-post.txt cell-summary.txt cell-result.txt; do
            meta=$(stat -c "%F:%u:%h" "$2/$leaf") || exit 1
            test "$meta" = "regular file:$(id -u):1" || exit 1
        done
        grep -qx "fio_rc=0" "$2/cell-result.txt" &&
        grep -qx "summary_verified=exact" "$2/cell-result.txt" &&
        test "$(grep -cx "status=CELL_OK" "$2/cell-result.txt")" -eq 1 &&
        grep -qx "CELL_OK arm=uringpoll mode=thread jobs=1 rep=1 variant=ss pair_us=1.000000" "$2/cell-summary.txt" &&
        grep -q "boot_id=11111111-2222-3333-4444-555555555555" "$2/diskstats-pre.txt" &&
        grep -q "diskseq=77" "$2/diskstats-post.txt" &&
        grep -q "raw_fields=17" "$2/diskstats-post.txt" &&
        grep -q "poll_queues=4" "$2/poll-pre.txt" &&
        grep -q "io_poll=1" "$2/poll-post.txt" &&
        grep -q "mq_cpu_list=0|2,6" "$2/poll-post.txt" &&
        grep -q "mq_cpu_list=1|0-1" "$2/poll-post.txt" &&
        ! grep -q "sha256" "$2/poll-post.txt"
    ' bash "$evidence_clean_rc" "$evidence_cell"
expect_ok "fio rc is persisted before post validation and success sealing" \
    bash -c '
        rc=$(grep -n "CELL_EVIDENCE_RC_RECORDED" "$1" | cut -d: -f1)
        post=$(grep -n "CELL_EVIDENCE_POST_VALIDATED" "$1" | cut -d: -f1)
        seal=$(grep -n "CELL_EVIDENCE_SEALED" "$1" | cut -d: -f1)
        test -n "$rc" -a -n "$post" -a -n "$seal" -a "$rc" -lt "$post" -a "$post" -lt "$seal"
    ' bash "$evidence_clean_out"
expect_ok "production persists retained cell metrics before sealing and only then appends the rebuildable index" \
    bash -c '
        body=$(sed -n '\''/^run_cell()/,/^}$/p'\'' "$1")
        persist=$(grep -n "persist_cell_summary" <<<"$body" | head -1 | cut -d: -f1)
        seal=$(grep -n "seal_cell_evidence_success" <<<"$body" | head -1 | cut -d: -f1)
        index=$(grep -n '\''RESULT_DIR/summary.txt'\'' <<<"$body" | tail -1 | cut -d: -f1)
        test -n "$persist" -a -n "$seal" -a -n "$index" &&
        test "$persist" -lt "$seal" -a "$seal" -lt "$index"
    ' bash "$script"

for summary_attack in empty mismatch replace; do
    make_cell_evidence_fixture "summary-$summary_attack"
    summary_attack_rc=0
    EVIDENCE_SUMMARY_MODE=$summary_attack \
        run_cell_evidence_fixture "$tmp/evidence-success-command" \
        >"$tmp/summary-$summary_attack.out" \
        2>"$tmp/summary-$summary_attack.err" || summary_attack_rc=$?
    expect_ok "cell summary $summary_attack cannot seal CELL_OK" \
        bash -c '
            test "$1" -ne 0 && ! grep -q "status=CELL_OK" "$2/cell-result.txt" &&
            grep -Eq "cell_summary_(empty|content_mismatch)|evidence_leaf_identity_changed=cell-summary.txt" "$3"
        ' bash "$summary_attack_rc" "$evidence_cell" \
            "$tmp/summary-$summary_attack.err"
done

make_cell_evidence_fixture global-index-fail
global_index_rc=0
EVIDENCE_GLOBAL_SUMMARY=/dev/full \
    run_cell_evidence_fixture "$tmp/evidence-success-command" \
    >"$tmp/global-index-fail.out" 2>"$tmp/global-index-fail.err" || global_index_rc=$?
expect_ok "global index append failure fails the run but retained CELL_OK remains reconstructible" \
    bash -c '
        test "$1" -ne 0 && grep -q "global_summary_append_failed" "$3" &&
        grep -qx "status=CELL_OK" "$2/cell-result.txt" &&
        test -s "$2/cell-summary.txt"
    ' bash "$global_index_rc" "$evidence_cell" "$tmp/global-index-fail.err"

make_cell_evidence_fixture fd-lifecycle
lifecycle_out=$(env EXITOS_SCALE_TEST_MODE=1 EVIDENCE_PROC="$evidence_proc" \
    bash "$script" --test-cell-evidence-lifecycle "$evidence_root/lifecycle" \
    "$evidence_proc" "$evidence_sys" "$evidence_cpu" "$evidence_ctrl" \
    fake0 nvme-test 0000:aa:00.0 249:0 2 \
    "$tmp/evidence-success-command" 40 2>"$tmp/evidence-lifecycle.err" | \
    tail -n 1 || true)
expect_ok "forty same-shell evidence lifecycles close and clear the retained directory FD" \
    test "$lifecycle_out" = \
    'CELL_EVIDENCE_LIFECYCLE_OK cells=40 fd_delta=0 active=0 dir_fd=none fd_path=none identities=none'

cat >"$tmp/evidence-nonzero-command" <<'EOF'
#!/bin/bash
"$EVIDENCE_SUCCESS_COMMAND"
exit 7
EOF
chmod 0700 "$tmp/evidence-nonzero-command"
make_cell_evidence_fixture nonzero
evidence_nonzero_rc=0
EVIDENCE_SUCCESS_COMMAND="$tmp/evidence-success-command" \
    run_cell_evidence_fixture "$tmp/evidence-nonzero-command" \
    >"$tmp/evidence-nonzero.out" 2>"$tmp/evidence-nonzero.err" || evidence_nonzero_rc=$?
expect_ok "nonzero fio retains pre/post evidence and rc without CELL_OK" \
    bash -c '
        test "$1" -ne 0 && grep -qx "fio_rc=7" "$2/cell-result.txt" &&
        ! grep -q CELL_OK "$2/cell-result.txt" &&
        test ! -s "$2/cell-summary.txt" &&
        test -s "$2/diskstats-pre.txt" -a -s "$2/diskstats-post.txt" -a \
             -s "$2/poll-pre.txt" -a -s "$2/poll-post.txt"
    ' bash "$evidence_nonzero_rc" "$evidence_cell"

cat >"$tmp/evidence-malformed-disk-command" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' '259 0 fake0 11 0 22' >"$EVIDENCE_PROC/diskstats"
EOF
chmod 0700 "$tmp/evidence-malformed-disk-command"
make_cell_evidence_fixture malformed-disk
expect_fail_with "malformed post diskstats is retained and refused" \
    'diskstats_fields_short' "$tmp/evidence-malformed-disk" \
    run_cell_evidence_fixture "$tmp/evidence-malformed-disk-command"
expect_ok "malformed diskstats cannot manufacture cell success" \
    bash -c 'grep -qx "fio_rc=0" "$1/cell-result.txt" && ! grep -q CELL_OK "$1/cell-result.txt" && test -s "$1/diskstats-post.txt"' \
        bash "$evidence_cell"

cat >"$tmp/evidence-regress-disk-command" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' '259 0 fake0 9 0 20 30 40 0 50 60 0 0 0 0 0 0 0 0 0' \
    >"$EVIDENCE_PROC/diskstats"
EOF
chmod 0700 "$tmp/evidence-regress-disk-command"
make_cell_evidence_fixture regress-disk
expect_fail_with "regressing diskstats counter is refused" \
    'diskstats_counter_regression' "$tmp/evidence-regress-disk" \
    run_cell_evidence_fixture "$tmp/evidence-regress-disk-command"

for bad_poll in zero malformed; do
    make_cell_evidence_fixture "poll-$bad_poll"
    case "$bad_poll" in
        zero) printf '%s\n' 0 >"$evidence_sys/module/nvme/parameters/poll_queues" ;;
        malformed) printf '%s\n' bad >"$evidence_sys/module/nvme/parameters/poll_queues" ;;
    esac
    expect_fail_with "poll_queues $bad_poll fails closed before launch" \
        'poll_queues_invalid' "$tmp/evidence-poll-$bad_poll" \
        run_cell_evidence_fixture "$tmp/evidence-success-command"
done
make_cell_evidence_fixture poll-missing
rm "$evidence_sys/module/nvme/parameters/poll_queues"
expect_fail_with "missing poll_queues fails closed before launch" \
    'poll_queues_read_failed' "$tmp/evidence-poll-missing" \
    run_cell_evidence_fixture "$tmp/evidence-success-command"

make_cell_evidence_fixture io-poll-zero
printf '%s\n' 0 >"$evidence_sys/block/fake0/queue/io_poll"
expect_fail_with "namespace io_poll zero fails closed before launch" \
    'io_poll_invalid' "$tmp/evidence-io-poll-zero" \
    run_cell_evidence_fixture "$tmp/evidence-success-command"

cat >"$tmp/evidence-poll-drift-command" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' 5 >"$EVIDENCE_SYS/module/nvme/parameters/poll_queues"
EOF
chmod 0700 "$tmp/evidence-poll-drift-command"
make_cell_evidence_fixture poll-drift
expect_fail_with "poll_queues drift is retained and refused" \
    'poll_state_drift' "$tmp/evidence-poll-drift" \
    run_cell_evidence_fixture "$tmp/evidence-poll-drift-command"

cat >"$tmp/evidence-mq-drift-command" <<'EOF'
#!/bin/bash
set -eu
printf '%s\n' 3 >"$EVIDENCE_SYS/block/fake0/mq/0/cpu_list"
EOF
chmod 0700 "$tmp/evidence-mq-drift-command"
make_cell_evidence_fixture mq-drift
expect_fail_with "complete mq cpu_list map drift is retained and refused" \
    'poll_state_drift' "$tmp/evidence-mq-drift" \
    run_cell_evidence_fixture "$tmp/evidence-mq-drift-command"

expect_ok "main cell evidence has no IRQ/qid/affinity authority" \
    bash -c '
        body=$(sed -n '\''/^cell_evidence_python()/,/^}/p'\'' "$1")
        ! grep -Eq "irq_snapshot|irq_mapping|qid|effective_affinity|interrupts" <<<"$body"
    ' bash "$script"

for unsafe_kind in preexisting symlink hardlink; do
    make_cell_evidence_fixture "$unsafe_kind"
    case "$unsafe_kind" in
        preexisting) : >"$evidence_cell/diskstats-pre.txt" ;;
        symlink) ln -s "$sentinel" "$evidence_cell/diskstats-pre.txt" ;;
        hardlink) ln "$sentinel" "$evidence_cell/diskstats-pre.txt" ;;
    esac
    expect_fail_with "evidence initializer refuses $unsafe_kind leaf" \
        'pre-existing evidence object: diskstats-pre.txt' \
        "$tmp/evidence-unsafe-$unsafe_kind" \
        run_cell_evidence_fixture "$tmp/evidence-success-command"
done
if [ "$(id -u)" -eq 0 ]; then
    make_cell_evidence_fixture nonowned
    : >"$evidence_cell/diskstats-pre.txt"
    chown 65534:65534 "$evidence_cell/diskstats-pre.txt"
    expect_fail_with "evidence initializer refuses non-owned leaf" \
        'pre-existing evidence object: diskstats-pre.txt' \
        "$tmp/evidence-unsafe-nonowned" \
        run_cell_evidence_fixture "$tmp/evidence-success-command"
else
    ok "evidence initializer non-owner case requires root fixture (skipped)"
fi

cat >"$tmp/evidence-leaf-replace-command" <<'EOF'
#!/bin/bash
set -eu
mv "$EVIDENCE_CELL/diskstats-post.txt" "$EVIDENCE_CELL/diskstats-post.old"
: >"$EVIDENCE_CELL/diskstats-post.txt"
chmod 0600 "$EVIDENCE_CELL/diskstats-post.txt"
EOF
chmod 0700 "$tmp/evidence-leaf-replace-command"
make_cell_evidence_fixture leaf-replace
expect_fail_with "post evidence refuses a replaced leaf inode" \
    'evidence_leaf_identity_changed=diskstats-post.txt' \
    "$tmp/evidence-leaf-replace" \
    run_cell_evidence_fixture "$tmp/evidence-leaf-replace-command"

cat >"$tmp/evidence-dir-replace-command" <<'EOF'
#!/bin/bash
set -eu
mv "$EVIDENCE_CELL" "$EVIDENCE_CELL.retained"
mkdir -m 0700 "$EVIDENCE_CELL"
EOF
chmod 0700 "$tmp/evidence-dir-replace-command"
make_cell_evidence_fixture dir-replace
expect_fail_with "post evidence refuses cell pathname/retained-FD replacement" \
    'evidence_directory_identity_changed' "$tmp/evidence-dir-replace" \
    run_cell_evidence_fixture "$tmp/evidence-dir-replace-command"

cat >"$tmp/evidence-signal-command" <<'EOF'
#!/bin/bash
set -eu
kill -TERM "$PPID"
sleep 2
EOF
chmod 0700 "$tmp/evidence-signal-command"
make_cell_evidence_fixture signal
evidence_signal_rc=0
run_cell_evidence_fixture "$tmp/evidence-signal-command" \
    >"$tmp/evidence-signal.out" 2>"$tmp/evidence-signal.err" || evidence_signal_rc=$?
expect_ok "signal termination persists signal rc and never seals CELL_OK" \
    bash -c '
        test "$1" -eq 143 && grep -qx "fio_rc=143" "$2/cell-result.txt" &&
        ! grep -q CELL_OK "$2/cell-result.txt" &&
        test ! -s "$2/cell-summary.txt" &&
        test -s "$2/diskstats-pre.txt" -a -s "$2/diskstats-post.txt" -a \
             -s "$2/poll-pre.txt" -a -s "$2/poll-post.txt"
    ' bash "$evidence_signal_rc" "$evidence_cell"

echo "1..$n  ($bad failed)"
exit "$bad"
