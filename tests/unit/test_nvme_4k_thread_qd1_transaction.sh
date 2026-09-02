#!/bin/bash
# Device-free contract for the NVMe 4 KiB/QD1 thread transaction.
# Only the read-only plan path is executed.  Destructive behavior is checked
# structurally so this test can never touch host PCI or block-device state.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
DRIVER=$ROOT/tools/nvme-4k-thread-qd1-transaction.sh
TMP=$(mktemp -d /tmp/exitos-nvme-4k-thread-q1-transaction.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'ok %d - %s\n' "$pass" "$1"; }
bad() { fail=$((fail + 1)); printf 'not ok - %s\n' "$1" >&2; }
contains()
{
    local label=$1 file=$2 needle=$3
    if grep -Fq -- "$needle" "$file"; then ok "$label"; else
        bad "$label (missing: $needle)"
    fi
}
static_contains()
{
    local label=$1 needle=$2
    if [ -f "$DRIVER" ] && grep -Fq -- "$needle" "$DRIVER"; then
        ok "$label"
    else
        bad "$label (missing: $needle)"
    fi
}
line_before()
{
    local label=$1 first=$2 second=$3 first_line second_line
    first_line=$([ -f "$DRIVER" ] && grep -Fn -- "$first" "$DRIVER" |
        head -1 | cut -d: -f1 || true)
    second_line=$([ -f "$DRIVER" ] && grep -Fn -- "$second" "$DRIVER" |
        head -1 | cut -d: -f1 || true)
    if [ -n "$first_line" ] && [ -n "$second_line" ] &&
       [ "$first_line" -lt "$second_line" ]; then
        ok "$label"
    else
        bad "$label (first=$first_line second=$second_line)"
    fi
}

mkdir -p -- "$TMP/mount"
RESULT=/root/exitos-artifacts/unit-contract-must-not-exist
PLAN=$TMP/plan.txt

if [ -x "$DRIVER" ]; then
    ok "transaction driver is executable"
else
    bad "transaction driver is executable"
fi

if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" --result-dir "$RESULT" \
    --active-so /reviewed/libexitos_bpftime_active.so \
    --transformer /reviewed/libbpftime-agent-transformer.so >"$PLAN"; then
    ok "default plan succeeds without device access"
else
    bad "default plan succeeds without device access"
    : >"$PLAN"
fi

contains "plan identifies controller serial" "$PLAN" \
    'expected_serial=EXAMPLESERIAL0001'
contains "plan identifies namespace WWID" "$PLAN" \
    'expected_wwid=eui.00000000000000000000000000000000'
contains "plan identifies PCI DSN" "$PLAN" \
    'expected_pci_dsn=00-00-00-00-00-00-00-00'
contains "plan resolves names and BDF at run time" "$PLAN" \
    'identity_resolution=serial+PCI-DSN+namespace-WWID:no-device-name-or-BDF-cache'
contains "plan targets eight poll queues" "$PLAN" 'target_nvme_poll_queues=8'
contains "plan freezes the reviewed entry state" "$PLAN" \
    'expected_entry=driver:nvme,override:(null),nvme_poll_queues:0'
contains "plan fixes 4 KiB per-worker QD1 thread gate" "$PLAN" \
    'matrix=threads:4/8,bs:4k,per-worker-qd:1,backend:uringpoll'
contains "plan delegates scheduler proof to runner" "$PLAN" \
    'runner_scheduler=SCHED_OTHER,nice:-20,scheduler-policy-pilots=4T+8T,parent=verified-per-cell'
contains "plan chooses physical cores on target NUMA node" "$PLAN" \
    'cpu_selection=post-bind-poll-mq,target-NUMA,distinct-hctx+physical-core,count:8'
contains "plan records strict remote result directory" "$PLAN" \
    "result_dir=$RESULT"
contains "plan preserves entry state" "$PLAN" \
    'restore=entry-driver+driver-override+poll-queues+mount-state'
contains "plan proves namespace is unused" "$PLAN" \
    'unused_gate=not-system+unmounted+no-holder+no-dm+no-md+no-swap+no-openers+no-inflight'
contains "plan creates initialized ext4 without discard" "$PLAN" \
    'filesystem=temporary-partition,ext4:lazy_itable_init=0,lazy_journal_init=0,mkfs:nodiscard,mount:nodiscard+noatime'
contains "plan reports the selected default workload runner" "$PLAN" \
    "runner=$ROOT/tools/run-4k-thread-qd1-matrix.sh"
contains "plan promises no mutation or artifacts" "$PLAN" \
    'PLAN_OK read_only=yes device_action=no result=none artifacts=none'
[ ! -e "$RESULT" ] && ok "plan creates no result directory" ||
    bad "plan creates no result directory"

CUSTOM_PLAN=$TMP/custom-plan.txt
CUSTOM_RUNNER_ARG=$ROOT/tools/../tools/nvme-4k-thread-qd1-transaction.sh
if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" --result-dir "$RESULT" \
    --active-so /reviewed/libexitos_bpftime_active.so \
    --transformer /reviewed/libbpftime-agent-transformer.so \
    --workload-runner "$CUSTOM_RUNNER_ARG" >"$CUSTOM_PLAN"; then
    ok "plan accepts an executable workload runner resolving inside repo"
else
    bad "plan accepts an executable workload runner resolving inside repo"
    : >"$CUSTOM_PLAN"
fi
contains "plan reports the canonical selected custom workload runner" \
    "$CUSTOM_PLAN" "runner=$DRIVER"
contains "custom plan delegates workload semantics to selected runner" \
    "$CUSTOM_PLAN" 'workload_contract=runner-owned:inspect-runner-plan'

OUTSIDE_RUNNER_ERR=$TMP/outside-runner.err
if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" --result-dir "$RESULT" \
    --active-so /reviewed/active.so --transformer /reviewed/transformer.so \
    --workload-runner /bin/true >/dev/null 2>"$OUTSIDE_RUNNER_ERR"; then
    bad "workload runner resolving outside canonical repo is rejected"
elif grep -Fq 'workload_runner_outside_repo=' "$OUTSIDE_RUNNER_ERR"; then
    ok "workload runner resolving outside canonical repo is rejected"
else
    bad "workload runner resolving outside canonical repo is rejected (wrong failure)"
fi
NONEXEC_RUNNER_ERR=$TMP/nonexec-runner.err
if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" --result-dir "$RESULT" \
    --active-so /reviewed/active.so --transformer /reviewed/transformer.so \
    --workload-runner "$ROOT/Makefile" >/dev/null 2>"$NONEXEC_RUNNER_ERR"; then
    bad "non-executable workload runner inside repo is rejected"
elif grep -Fq 'workload_runner_not_executable=' "$NONEXEC_RUNNER_ERR"; then
    ok "non-executable workload runner inside repo is rejected"
else
    bad "non-executable workload runner inside repo is rejected (wrong failure)"
fi

BAD_RESULT=$TMP/not-remote-artifacts/run
if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" --result-dir "$BAD_RESULT" \
    --active-so /reviewed/active.so --transformer /reviewed/transformer.so \
    >/dev/null 2>&1; then
    bad "transaction rejects result paths outside the dated remote root"
else
    ok "transaction rejects result paths outside the dated remote root"
fi
if [ -x "$DRIVER" ] && "$DRIVER" --plan --repo "$ROOT" \
    --mount "$TMP/mount" \
    --result-dir /root/exitos-artifacts/nested/run \
    --active-so /reviewed/active.so --transformer /reviewed/transformer.so \
    >/dev/null 2>&1; then
    bad "transaction requires a direct unique-run child of the dated root"
else
    ok "transaction requires a direct unique-run child of the dated root"
fi

if [ -f "$DRIVER" ] && bash -n "$DRIVER"; then
    ok "transaction parses as bash"
else
    bad "transaction parses as bash"
fi

static_contains "run requires exact serial confirmation" '--confirm-serial'
static_contains "run requires exact WWID confirmation" '--confirm-wwid'
static_contains "run requires exact PCI DSN confirmation" '--confirm-pci-dsn'
static_contains "run requires captured boot confirmation" '--confirm-boot'
static_contains "PCI is dynamically resolved by DSN" 'resolve_unique_pci_by_dsn'
static_contains "controller is dynamically resolved by serial" 'resolve_controller_serial'
static_contains "namespace is dynamically resolved by WWID" 'resolve_namespace_wwid'
static_contains "namespace block node rdev is bound to sysfs devt" \
    'require_block_devt "$WHOLE" "$namespace_devt" namespace'
static_contains "system backing tree is rejected" 'namespace_backs_system_storage'
static_contains "block holders are rejected" 'namespace_has_holder'
static_contains "device-mapper membership is rejected" 'namespace_is_dm_member'
static_contains "md membership is rejected" 'namespace_is_md_member'
static_contains "swap membership is rejected" 'namespace_is_swap'
static_contains "block and controller openers are rejected" \
    'require_no_openers "$node" namespace'
static_contains "controller character node rdev is bound to sysfs devt" \
    'require_char_devt "$DEV_ROOT/$ctrl" "$ctrl_devt" controller'
static_contains "generic character node rdev is bound to sysfs devt" \
    'require_char_devt "$node" "$generic_devt" generic_node'
static_contains "in-flight I/O is rejected" 'namespace_has_inflight'
static_contains "entry driver is captured" 'ORIG_DRIVER=$(driver_for "$PCI")'
static_contains "entry override is captured" 'ORIG_OVERRIDE=$(read_override "$PCI")'
static_contains "entry poll setting is captured" 'ORIG_POLL=$(read_one "$NVME_POLL_PATH")'
static_contains "run gates the exact reviewed entry driver" \
    'require_equal entry_driver "$EXPECTED_ENTRY_DRIVER" "$ORIG_DRIVER"'
static_contains "run gates the exact reviewed entry override" \
    'require_equal entry_override "$EXPECTED_ENTRY_OVERRIDE" "$ORIG_OVERRIDE"'
static_contains "restore can bind a custom original driver with an empty override" \
    'restore_probe_override'
static_contains "run gates the exact reviewed entry poll setting" \
    'require_equal entry_poll_queues "$EXPECTED_ENTRY_POLL" "$ORIG_POLL"'
static_contains "entry mount state is captured" 'capture_mount_state'
static_contains "optional findmnt probes distinguish no-match from errors" \
    'findmnt_optional'
static_contains "unconditional exit cleanup exists" 'trap cleanup EXIT'
static_contains "only resolved target is unbound" 'unbind_exact_driver "$PCI" "$ORIG_DRIVER"'
static_contains "poll queues are set to eight" 'write_sysfs "$NVME_POLL_PATH" "$POLL_QUEUES"'
static_contains "post-bind polling is required" 'require_equal io_poll 1'
static_contains "post-bind write-through cache policy is required" \
    'require_equal write_cache "write through"'
static_contains "physical CPUs are selected dynamically" 'select_numa_physical_cpus'
static_contains "poll CPUs are selected from distinct hardware contexts" \
    'select_poll_hctx_physical_cpus'
static_contains "all mq cpu lists are captured" 'capture_mq_snapshot'
static_contains "active namespace devt is retained" 'ACTIVE_DEVT='
static_contains "active generation is checked before the runner" \
    'verify_active_generation before-runner'
static_contains "active generation is checked after the runner" \
    'verify_active_generation after-runner'
static_contains "temporary partition has no persistent table" \
    'addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"'
static_contains "addpart attempt enters ambiguous ownership state" \
    'PART_STATE=ambiguous'
static_contains "successful addpart establishes cleanup ownership" \
    'PART_STATE=created'
static_contains "addpart nonzero records an ambiguous exit status" \
    'die "addpart_failed_state_ambiguous_rc=$ADDPART_RC"'
static_contains "cleanup resolves attempted partition state from sysfs" \
    'resolve_temporary_partition_state'
if sed -n '/if ! addpart /,/^[[:space:]]*fi$/p' "$DRIVER" |
   grep -Fq 'PART_CREATED=0'; then
    bad "addpart nonzero never clears unknown-state cleanup ownership"
else
    ok "addpart nonzero never clears unknown-state cleanup ownership"
fi
static_contains "temporary partition rdev is bound to sysfs devt" \
    'require_block_devt "$PART" "$PART_DEVT" partition'
static_contains "mkfs disables discard and lazy initialization" \
    'mkfs.ext4 -F -q -E nodiscard,lazy_itable_init=0,lazy_journal_init=0'
static_contains "mount disables discard and atime" \
    'mount -t ext4 -o nodiscard,noatime -- "$PART" "$mount_dir"'
static_contains "mount policy validates discard is absent" \
    'require_mount_nodiscard_noatime "$MOUNT_OPTIONS"'
static_contains "fixed runner receives dynamic CPUs" '--cpus "$CPUS_CSV"'
static_contains "fixed runner receives exact partition" '--device "$PART"'
static_contains "fixed runner receives namespace identity" \
    '--expect-identity "$EXPECTED_WWID"'
static_contains "inner manifest is disabled" '--manifest-mode outer'
static_contains "runner receives whole namespace through transaction environment" \
    'EXITOS_TRANSACTION_WHOLE="$WHOLE"'
static_contains "runner receives generic nodes through transaction environment" \
    'EXITOS_TRANSACTION_GENERIC="$TARGET_GENERIC_NODES"'
static_contains "runner receives partition start through transaction environment" \
    'EXITOS_TRANSACTION_PART_START_SECTORS="$PART_START_SECTORS"'
static_contains "runner receives partition size through transaction environment" \
    'EXITOS_TRANSACTION_PART_SIZE_SECTORS="$PART_SIZE_SECTORS"'
static_contains "runner receives whole size through transaction environment" \
    'EXITOS_TRANSACTION_WHOLE_SECTORS="$WHOLE_SECTORS"'
static_contains "transaction allows one final manifest" 'final-manifest.sha256'
static_contains "final manifest includes the selected runner support directory" \
    'find "$(dirname -- "$RUNNER")" -maxdepth 1 -type f -print0'
static_contains "poll restoration failure cannot abort the cleanup body" \
    'if (write_sysfs "$NVME_POLL_PATH" "$ORIG_POLL"); then'
static_contains "probe failure cannot abort the cleanup body" \
    'if ! (probe_pci "$PCI"); then'
static_contains "driver wait failure cannot abort the cleanup body" \
    'elif ! (wait_for_driver "$PCI" "$ORIG_DRIVER"); then'
static_contains "boot verification failure cannot abort the cleanup body" \
    '(require_boot_stable) || cleanup_rc=1'
static_contains "cleanup ignores a second termination signal" \
    "trap '' INT TERM HUP"
static_contains "partition mount aliases are checked by devt" \
    'require_devt_unmounted "$PART_DEVT"'
static_contains "active DSO freshness is checked from source inputs" \
    'require_active_dso_fresh'
static_contains "restore record call contains no saved-value fallback" \
    'write_restore_record "$RESTORED_DRIVER" "$RESTORED_OVERRIDE"'
static_contains "stdout tee is waited independently" \
    'wait "$STDOUT_TEE_PID" || cleanup_rc=1'
static_contains "stderr tee is waited independently" \
    'wait "$STDERR_TEE_PID" || cleanup_rc=1'
if grep -Fq 'wait "$STDOUT_TEE_PID" "$STDERR_TEE_PID"' "$DRIVER"; then
    bad "tee statuses are never collapsed into one multi-PID wait"
else
    ok "tee statuses are never collapsed into one multi-PID wait"
fi

line_before "identity is resolved before entry driver capture" \
    'PCI=$(resolve_unique_pci_by_dsn "$EXPECTED_PCI_DSN")' \
    'ORIG_DRIVER=$(driver_for "$PCI")'
line_before "mount state is captured before restore is armed" \
    'capture_mount_state' 'RESTORE_ARMED=1'
line_before "restore is armed before poll queues change" \
    'RESTORE_ARMED=1' 'write_sysfs "$NVME_POLL_PATH" "$POLL_QUEUES"'
line_before "poll queues change before target-only unbind" \
    'write_sysfs "$NVME_POLL_PATH" "$POLL_QUEUES"' \
    'unbind_exact_driver "$PCI" "$ORIG_DRIVER"'
line_before "partition identity is armed before addpart" \
    'PART=$(partition_node_for "$WHOLE" "$PART_NUMBER")' \
    'addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"'
line_before "partition cleanup is armed before addpart" \
    'PART_CREATED=1' \
    'addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"'
line_before "partition ownership is ambiguous before addpart" \
    'PART_STATE=ambiguous' \
    'addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"'
line_before "partition ownership becomes created only after addpart succeeds" \
    'addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"' \
    'PART_STATE=created'
line_before "mount cleanup is armed before mount" \
    'MOUNTED_BY_US=1' \
    'mount -t ext4 -o nodiscard,noatime -- "$PART" "$mount_dir"'
line_before "filesystem unmount precedes partition removal" \
    'umount -- "$mount_dir"' 'delpart "$WHOLE" "$PART_NUMBER"'
line_before "temporary driver unbind precedes guarded entry restoration" \
    'unbind_if_driver "$PCI" "$WORK_DRIVER"' \
    'restore_entry_binding "$current"'

# Ownership of the PCI binding is recorded, never inferred from the driver
# name.  Inferring it breaks the moment the entry driver and the work driver
# are allowed to be the same string: cleanup would then unbind a function the
# transaction never touched.
static_contains "the transaction records that it owns the PCI binding" \
    'PCI_REBOUND_BY_US=1'
line_before "binding ownership is recorded before the binding is disturbed" \
    'PCI_REBOUND_BY_US=1' \
    'unbind_exact_driver "$PCI" "$ORIG_DRIVER"'
static_contains "cleanup unbinds only a binding this transaction owns" \
    '[ "$PCI_REBOUND_BY_US" -eq 1 ] &&'
if grep -Eq '^\s*if \[ "\$current" = nvme \]; then' "$DRIVER"; then
    bad "cleanup no longer decides ownership from the driver name"
else
    ok "cleanup no longer decides ownership from the driver name"
fi
# The unbind must survive the entry driver and the work driver being equal:
# poll_queues only reaches the device through a fresh probe.
static_contains "restoring the poll-queue setting goes through a fresh probe" \
    'probe_pci "$PCI"'

# The temporary selector is the only thing that lands the function back on its
# entry driver at the next probe or boot.  Erasing it after a rebind that did
# not happen leaves the function unbound with nothing pointing home, which is
# what an unconditional OR here used to do on every failed probe, wait or
# settle.
if grep -Fq 'if [ "$ready" -eq 1 ] || [ "$restore_probe_override" -eq 1 ]; then' "$DRIVER"; then
    bad "the temporary selector is cleared only after the entry driver is bound"
else
    ok "the temporary selector is cleared only after the entry driver is bound"
fi
static_contains "clearing the selector is gated on the observed driver" \
    '[ "$(driver_for "$PCI" 2>/dev/null)" = "$ORIG_DRIVER" ]'
static_contains "a retained selector is reported" \
    'RESTORE_SELECTOR_RETAINED'

# The entry binding is the operator's to declare; the gate still compares it
# byte-for-byte against the live state.
static_contains "the entry driver is operator-supplied" \
    'EXITOS_TARGET_ENTRY_DRIVER'
static_contains "the entry override is operator-supplied" \
    'EXITOS_TARGET_ENTRY_OVERRIDE'
static_contains "the plan discloses the driver the run happens on" \
    'work_driver=$WORK_DRIVER'
line_before "saved poll precedes saved override restoration" \
    'write_sysfs "$NVME_POLL_PATH" "$ORIG_POLL"' \
    'set_override "$PCI" "$ORIG_OVERRIDE"'

if [ -f "$DRIVER" ] &&
   grep -Eq '0000:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[0-7]' "$DRIVER"; then
    bad "transaction contains no hard-coded PCI BDF"
else
    ok "transaction contains no hard-coded PCI BDF"
fi
if [ -f "$DRIVER" ] && grep -Eq '/dev/nvme[0-9]' "$DRIVER"; then
    bad "transaction contains no hard-coded nvme node"
else
    ok "transaction contains no hard-coded nvme node"
fi
sha_calls=$([ -f "$DRIVER" ] &&
    grep -Ec '(^|[[:space:]])sha256sum[[:space:]]+--' "$DRIVER" || true)
[ "${sha_calls:-0}" -le 1 ] && ok "at most one final checksum invocation" ||
    bad "at most one final checksum invocation (got $sha_calls)"
if [ -f "$DRIVER" ] && grep -Eq '(^|[[:space:]])ssh([[:space:]]|$)' "$DRIVER"; then
    bad "host-local transaction never launches ssh"
else
    ok "host-local transaction never launches ssh"
fi

library_case()
{
    EXITOS_TARGET_LIBRARY_ONLY=1 DRIVER="$DRIVER" TEST_TMP="$TMP" \
        bash -c 'source "$DRIVER"; eval "$1"' bash "$1"
}

if library_case '
    SYS_ROOT=$TEST_TMP/clean-dm-md-sys
    mkdir -p "$SYS_ROOT/class/block/target/holders"
    require_no_dm_md target
'; then
    ok "clean dm/md topology returns success"
else
    bad "clean dm/md topology returns success"
fi

if library_case '
    findmnt() { return 1; }
    output=$(findmnt_optional unit-no-match -rn -S /dev/fake)
    [ -z "$output" ]
'; then
    ok "empty findmnt rc1 is accepted as a verified no-match"
else
    bad "empty findmnt rc1 is accepted as a verified no-match"
fi
FINDMNT_DIAG_ERR=$TMP/findmnt-diagnostic.err
if library_case '
    findmnt() { printf "%s\n" "injected findmnt failure" >&2; return 1; }
    findmnt_optional unit-diagnostic -rn -S /dev/fake
' >/dev/null 2>"$FINDMNT_DIAG_ERR"; then
    bad "findmnt rc1 with diagnostics fails closed"
elif grep -Fq 'findmnt_probe_error=unit-diagnostic' "$FINDMNT_DIAG_ERR"; then
    ok "findmnt rc1 with diagnostics fails closed"
else
    bad "findmnt rc1 with diagnostics fails closed (wrong failure)"
fi

SWAP_RESOLVE_ERR=$TMP/swap-resolve.err
if library_case '
    PROC_ROOT=$TEST_TMP/swap-resolve-proc
    mkdir -p "$PROC_ROOT"
    printf "%s\n" "Filename Type Size Used Priority" \
        "/dev/unresolved partition 1 0 -2" >"$PROC_ROOT/swaps"
    readlink() {
        local last=${!#}
        [ "$last" = /dev/target ] && { printf "%s\n" /dev/target; return 0; }
        return 1
    }
    require_no_swap /dev/target
' >/dev/null 2>"$SWAP_RESOLVE_ERR"; then
    bad "unresolvable active swap path fails closed"
elif grep -Fq 'swap_path_resolve_failed=/dev/unresolved' "$SWAP_RESOLVE_ERR"; then
    ok "unresolvable active swap path fails closed"
else
    bad "unresolvable active swap path fails closed (wrong failure)"
fi

SWAP_TREE_ERR=$TMP/swap-tree.err
if library_case '
    PROC_ROOT=$TEST_TMP/swap-tree-proc
    mkdir -p "$PROC_ROOT"
    printf "%s\n" "Filename Type Size Used Priority" \
        "/dev/swapdev partition 1 0 -2" >"$PROC_ROOT/swaps"
    readlink() { local last=${!#}; printf "%s\n" "$last"; }
    resolve_swap_backing_node() { printf "%s\n" /dev/swapdev; }
    lsblk() { printf "%s\n" "injected lsblk failure" >&2; return 2; }
    require_no_swap /dev/target
' >/dev/null 2>"$SWAP_TREE_ERR"; then
    bad "unreadable active swap backing tree fails closed"
elif grep -Fq 'swap_backing_tree_probe_failed=/dev/swapdev' "$SWAP_TREE_ERR"; then
    ok "unreadable active swap backing tree fails closed"
else
    bad "unreadable active swap backing tree fails closed (wrong failure)"
fi

if library_case '
    swapfile=$TEST_TMP/unrelated.swap
    : >"$swapfile"
    findmnt() { printf "%s\n" /dev/backing; }
    require_block_node() { [ "$1" = /dev/backing ]; }
    backing=$(resolve_swap_backing_node "$swapfile")
    [ "$backing" = /dev/backing ]
'; then
    ok "swapfile is mapped to its backing block mount before ancestry proof"
else
    bad "swapfile is mapped to its backing block mount before ancestry proof"
fi

SYSTEM_PROBE_ERR=$TMP/system-storage-probe.err
if library_case '
    readlink() { local last=${!#}; printf "%s\n" "$last"; }
    findmnt() { printf "%s\n" "injected system probe failure" >&2; return 2; }
    require_not_system_storage /dev/target
' >/dev/null 2>"$SYSTEM_PROBE_ERR"; then
    bad "system mount enumeration failure fails closed"
elif grep -Fq 'system_mount_probe_failed' "$SYSTEM_PROBE_ERR"; then
    ok "system mount enumeration failure fails closed"
else
    bad "system mount enumeration failure fails closed (wrong failure)"
fi

MOUNT_CAPTURE_ERR=$TMP/mount-capture-probe.err
if library_case '
    WHOLE=/dev/fake
    mount_dir=$TEST_TMP/mount-capture
    findmnt() { printf "%s\n" "injected mount-state failure" >&2; return 1; }
    capture_mount_state
' >/dev/null 2>"$MOUNT_CAPTURE_ERR"; then
    bad "entry mount-state probe diagnostics fail closed"
elif grep -Fq 'findmnt_probe_error=entry-target-mounts' "$MOUNT_CAPTURE_ERR"; then
    ok "entry mount-state probe diagnostics fail closed"
else
    bad "entry mount-state probe diagnostics fail closed (wrong failure)"
fi

if library_case '
    require_block_node() { return 0; }
    stat() { printf "%s\n" 103:9; }
    require_block_devt /dev/fake 259:9 namespace
'; then
    ok "block node rdev matching sysfs devt is accepted"
else
    bad "block node rdev matching sysfs devt is accepted"
fi
RDEV_MISMATCH_ERR=$TMP/rdev-mismatch.err
if library_case '
    require_block_node() { return 0; }
    stat() { printf "%s\n" 103:9; }
    require_block_devt /dev/fake 259:10 partition
' >/dev/null 2>"$RDEV_MISMATCH_ERR"; then
    bad "block node rdev mismatch is refused"
elif grep -Fq 'partition_rdev_mismatch' "$RDEV_MISMATCH_ERR"; then
    ok "block node rdev mismatch is refused"
else
    bad "block node rdev mismatch is refused (wrong failure)"
fi
CHAR_RDEV_MISMATCH_ERR=$TMP/char-rdev-mismatch.err
if library_case '
    require_char_node() { return 0; }
    stat() { printf "%s\n" f2:1; }
    require_char_devt /dev/ng-fake 242:2 generic_node
' >/dev/null 2>"$CHAR_RDEV_MISMATCH_ERR"; then
    bad "character node rdev mismatch is refused"
elif grep -Fq 'generic_node_rdev_mismatch' "$CHAR_RDEV_MISMATCH_ERR"; then
    ok "character node rdev mismatch is refused"
else
    bad "character node rdev mismatch is refused (wrong failure)"
fi

if library_case '
    SYS_ROOT=$TEST_TMP/partial-addpart-sys
    PCI=fake-pci
    PART_BASE=fakep1
    PART_CREATED=1
    PART_DEVT=
    mkdir -p "$SYS_ROOT/class/block/$PART_BASE"
    printf "%s\n" 1 >"$SYS_ROOT/class/block/$PART_BASE/partition"
    printf "%s\n" 2048 >"$SYS_ROOT/class/block/$PART_BASE/start"
    printf "%s\n" 50331648 >"$SYS_ROOT/class/block/$PART_BASE/size"
    printf "%s\n" 259:9 >"$SYS_ROOT/class/block/$PART_BASE/dev"
    path_belongs_to_pci() { return 0; }
    state=$(resolve_temporary_partition_state)
    [ "$state" = "present 259:9" ]
'; then
    ok "cleanup recognizes an exact partition after addpart outcome is unknown"
else
    bad "cleanup recognizes an exact partition after addpart outcome is unknown"
fi
if library_case '
    SYS_ROOT=$TEST_TMP/absent-addpart-sys
    PCI=fake-pci
    PART_BASE=fakep1
    mkdir -p "$SYS_ROOT/class/block"
    require_no_partition_children() { return 0; }
    state=$(resolve_temporary_partition_state)
    [ "$state" = "absent -" ]
'; then
    ok "cleanup recognizes proven absence after addpart outcome is unknown"
else
    bad "cleanup recognizes proven absence after addpart outcome is unknown"
fi

if library_case 'require_mount_nodiscard_noatime rw,noatime'; then
    ok "mount gate accepts kernel output that omits default nodiscard"
else
    bad "mount gate accepts kernel output that omits default nodiscard"
fi
MOUNT_DISCARD_ERR=$TMP/mount-discard.err
if library_case 'require_mount_nodiscard_noatime rw,noatime,discard' \
    >/dev/null 2>"$MOUNT_DISCARD_ERR"; then
    bad "mount gate refuses an enabled discard option"
elif grep -Fq 'mount_discard_enabled' "$MOUNT_DISCARD_ERR"; then
    ok "mount gate refuses an enabled discard option"
else
    bad "mount gate refuses an enabled discard option (wrong failure)"
fi

NG_OPEN_ERR=$TMP/ng-open.err
if library_case '
    SYS_ROOT=$TEST_TMP/ng-open-sys
    DEV_ROOT=$TEST_TMP/ng-open-dev
    mkdir -p "$SYS_ROOT/class/nvme-generic/ng0" "$DEV_ROOT"
    : >"$DEV_ROOT/ng0"
    printf "%s\n" 242:1 >"$SYS_ROOT/class/nvme-generic/ng0/dev"
    path_belongs_to_pci() { return 0; }
    require_char_devt() { return 0; }
    fuser() { printf "1234\n"; return 0; }
    require_target_generic_nodes_unused fake-pci
' >/dev/null 2>"$NG_OPEN_ERR"; then
    bad "target generic-node opener is refused"
elif grep -Fq 'generic_node_has_opener' "$NG_OPEN_ERR"; then
    ok "target generic-node opener is refused"
else
    bad "target generic-node opener is refused (wrong failure)"
fi

NG_PROBE_ERR=$TMP/ng-probe.err
if library_case '
    SYS_ROOT=$TEST_TMP/ng-probe-sys
    DEV_ROOT=$TEST_TMP/ng-probe-dev
    mkdir -p "$SYS_ROOT/class/nvme-generic/ng0" "$DEV_ROOT"
    : >"$DEV_ROOT/ng0"
    printf "%s\n" 242:1 >"$SYS_ROOT/class/nvme-generic/ng0/dev"
    path_belongs_to_pci() { return 0; }
    require_char_devt() { return 0; }
    fuser() { printf "fuser injected failure\n" >&2; return 1; }
    require_target_generic_nodes_unused fake-pci
' >/dev/null 2>"$NG_PROBE_ERR"; then
    bad "generic-node opener probe errors fail closed"
elif grep -Fq 'opener_probe_error' "$NG_PROBE_ERR"; then
    ok "generic-node opener probe errors fail closed"
else
    bad "generic-node opener probe errors fail closed (wrong failure)"
fi

INFLIGHT_ERR=$TMP/inflight.err
if library_case '
    SYS_ROOT=$TEST_TMP/inflight-sys
    mkdir -p "$SYS_ROOT/class/block/fake"
    require_namespace_inflight_idle fake
' >/dev/null 2>"$INFLIGHT_ERR"; then
    bad "missing namespace inflight evidence fails closed"
elif grep -Fq 'namespace_inflight_unreadable' "$INFLIGHT_ERR"; then
    ok "missing namespace inflight evidence fails closed"
else
    bad "missing namespace inflight evidence fails closed (wrong failure)"
fi

SECONDARY_ERR=$TMP/secondary-mount.err
if library_case '
    PROC_ROOT=$TEST_TMP/secondary-proc
    mkdir -p "$PROC_ROOT/self"
    printf "%s\n" "36 25 259:9 / /secondary rw,relatime - ext4 /dev/fake rw" \
        >"$PROC_ROOT/self/mountinfo"
    require_devt_unmounted 259:9
' >/dev/null 2>"$SECONDARY_ERR"; then
    bad "secondary or bind mount of partition devt blocks removal"
elif grep -Fq 'partition_devt_is_mounted' "$SECONDARY_ERR"; then
    ok "secondary or bind mount of partition devt blocks removal"
else
    bad "secondary or bind mount of partition devt blocks removal (wrong failure)"
fi

MOUNTINFO_PROBE_ERR=$TMP/mountinfo-probe.err
if library_case '
    PROC_ROOT=$TEST_TMP/mountinfo-probe-proc
    mkdir -p "$PROC_ROOT/self"
    : >"$PROC_ROOT/self/mountinfo"
    awk() { return 2; }
    require_devt_unmounted 259:9
' >/dev/null 2>"$MOUNTINFO_PROBE_ERR"; then
    bad "mountinfo parser errors fail closed"
elif grep -Fq 'mountinfo_probe_error' "$MOUNTINFO_PROBE_ERR"; then
    ok "mountinfo parser errors fail closed"
else
    bad "mountinfo parser errors fail closed (wrong failure)"
fi

RESTORE_LOG=$TMP/restore-fault.log
if library_case '
    SYS_ROOT=$TEST_TMP/restore-fault-sys
    mkdir -p "$SYS_ROOT/bus/pci/devices/fake-pci"
    ORIG_POLL=0
    ORIG_OVERRIDE=nvmex
    ORIG_DRIVER=nvmex
    PCI=fake-pci
    RESTORE_FAULT_LOG=$TEST_TMP/restore-fault.log
    : >"$RESTORE_FAULT_LOG"
    write_sysfs() { return 0; }
    read_one() { printf "%s\n" 0; }
    set_override() { return 1; }
    read_override() { return 1; }
    probe_pci() { printf "PROBE\n" >>"$RESTORE_FAULT_LOG"; return 0; }
    wait_for_driver() { return 0; }
    if restore_entry_binding ""; then exit 70; fi
    [ ! -s "$RESTORE_FAULT_LOG" ]
    [ "$RESTORED_POLL" = 0 ]
    [ "$RESTORED_OVERRIDE" = unknown ]
    [ "$RESTORED_DRIVER" = unbound ]
'; then
    ok "override restore failure forbids probe and retains actual/unknown state"
else
    bad "override restore failure forbids probe and retains actual/unknown state"
fi

POLL_RESTORE_LOG=$TMP/poll-restore-fault.log
if library_case '
    SYS_ROOT=$TEST_TMP/poll-restore-fault-sys
    mkdir -p "$SYS_ROOT/bus/pci/devices/fake-pci"
    ORIG_POLL=0
    ORIG_OVERRIDE=nvmex
    ORIG_DRIVER=nvmex
    PCI=fake-pci
    POLL_RESTORE_FAULT_LOG=$TEST_TMP/poll-restore-fault.log
    : >"$POLL_RESTORE_FAULT_LOG"
    write_sysfs() { return 1; }
    read_one() { printf "%s\n" 8; }
    set_override() { return 0; }
    read_override() { printf "%s\n" nvmex; }
    probe_pci() { printf "PROBE\n" >>"$POLL_RESTORE_FAULT_LOG"; return 0; }
    wait_for_driver() { return 0; }
    if restore_entry_binding ""; then exit 70; fi
    [ ! -s "$POLL_RESTORE_FAULT_LOG" ]
    [ "$RESTORED_POLL" = 8 ]
    [ "$RESTORED_OVERRIDE" = nvmex ]
    [ "$RESTORED_DRIVER" = unbound ]
'; then
    ok "poll restore failure forbids probe and retains actual state"
else
    bad "poll restore failure forbids probe and retains actual state"
fi

if [ -f "$DRIVER" ] && grep -Fq '${restored_driver:-$ORIG_DRIVER}' "$DRIVER"; then
    bad "restore evidence never substitutes saved state for missing actual state"
else
    ok "restore evidence never substitutes saved state for missing actual state"
fi

if library_case '
    SYS_ROOT=$TEST_TMP/missing-pci-sys
    NVME_POLL_PATH=$SYS_ROOT/module/nvme/parameters/poll_queues
    PCI=missing-pci
    mkdir -p "$(dirname "$NVME_POLL_PATH")"
    printf "%s\n" 0 >"$NVME_POLL_PATH"
    observe_restore_state
    [ "$RESTORED_DRIVER" = unknown ]
    [ "$RESTORED_OVERRIDE" = unknown ]
    [ "$RESTORED_POLL" = 0 ]
'; then
    ok "missing PCI device is recorded as unknown rather than unbound"
else
    bad "missing PCI device is recorded as unknown rather than unbound"
fi

FULL_RESTORE_RESULT=$TMP/full-cleanup-result
FULL_RESTORE_PROBE=$TMP/full-cleanup-probe.log
mkdir -p "$FULL_RESTORE_RESULT" "$TMP/full-cleanup-mount"
if library_case '
    SYS_ROOT=$TEST_TMP/full-cleanup-sys
    mkdir -p "$SYS_ROOT/bus/pci/devices/fake-pci"
    RESTORE_ARMED=1
    RESULT_CREATED=1
    PART_CREATED=0
    MOUNTED_BY_US=0
    LOGGING_ACTIVE=0
    WORK_SUCCEEDED=0
    result_dir=$TEST_TMP/full-cleanup-result
    mount_dir=$TEST_TMP/full-cleanup-mount
    PCI=fake-pci
    WHOLE=$TEST_TMP/fake-whole
    ORIG_DRIVER=nvmex
    ORIG_OVERRIDE=nvmex
    ORIG_POLL=0
    ORIG_TARGET_MOUNTS=
    ORIG_MOUNT_DIR_RECORD=
    IOMMU_GROUP=7
    FULL_PROBE_LOG=$TEST_TMP/full-cleanup-probe.log
    : >"$FULL_PROBE_LOG"
    driver_for() { return 0; }
    read_override() { return 1; }
    read_one() {
        case "$1" in
          *poll_queues) printf "%s\n" 0 ;;
          *boot_id) printf "%s\n" fake-boot ;;
          *) return 1 ;;
        esac
    }
    write_sysfs() { return 0; }
    set_override() { return 1; }
    probe_pci() { printf "PROBE\n" >>"$FULL_PROBE_LOG"; return 0; }
    wait_for_driver() { return 0; }
    mountpoint() { return 1; }
    findmnt() { return 1; }
    require_boot_stable() { return 0; }
    require_singleton_iommu() { return 0; }
    require_vfio_group_unused() { return 0; }
    write_final_manifest() { return 0; }
    cleanup
' >/dev/null 2>"$TMP/full-cleanup.err"; then
    bad "full cleanup fault injection returns failure"
elif [ ! -s "$FULL_RESTORE_PROBE" ] &&
     grep -Fxq 'driver=unbound' "$FULL_RESTORE_RESULT/restore.txt" &&
     grep -Fxq 'driver_override=unknown' "$FULL_RESTORE_RESULT/restore.txt" &&
     grep -Fxq 'nvme_poll_queues=0' "$FULL_RESTORE_RESULT/restore.txt" &&
     grep -Fxq 'cleanup_rc=1' "$FULL_RESTORE_RESULT/restore.txt"; then
    ok "full cleanup fault injection forbids probe and records only actual/unknown"
else
    bad "full cleanup fault injection forbids probe and records only actual/unknown"
fi

MOUNT_OWNER_LOG=$TMP/mount-owner-umount.log
if library_case '
    SYS_ROOT=$TEST_TMP/mount-owner-sys
    mkdir -p "$SYS_ROOT/bus/pci/devices/fake-pci"
    RESTORE_ARMED=1
    RESULT_CREATED=0
    PART_CREATED=0
    MOUNTED_BY_US=1
    LOGGING_ACTIVE=0
    mount_dir=$TEST_TMP/mount-owner-mount
    PART_DEVT=259:9
    PCI=fake-pci
    ORIG_DRIVER=nvmex
    ORIG_OVERRIDE=nvmex
    ORIG_POLL=0
    MOUNT_OWNER_UMOUNT_LOG=$TEST_TMP/mount-owner-umount.log
    mkdir -p "$mount_dir"
    mountpoint() { return 0; }
    mountpoint_devt() { printf "%s\n" 8:1; }
    umount() { printf "UMOUNT\n" >>"$MOUNT_OWNER_UMOUNT_LOG"; return 0; }
    driver_for() { printf "%s\n" nvme; }
    read_override() { printf "%s\n" nvme; }
    read_one() { printf "%s\n" 8; }
    findmnt() { return 1; }
    require_boot_stable() { return 1; }
    require_singleton_iommu() { return 1; }
    cleanup
' >/dev/null 2>"$TMP/mount-owner.err"; then
    bad "cleanup returns failure for a mount devt ownership mismatch"
elif [ ! -e "$MOUNT_OWNER_LOG" ] &&
     grep -Fq 'RESTORE_BLOCKED mount_devt=expected:259:9,actual:8:1' \
        "$TMP/mount-owner.err"; then
    ok "cleanup never unmounts a mountpoint owned by another devt"
else
    bad "cleanup never unmounts a mountpoint owned by another devt"
fi

MOUNT_UNKNOWN_RESULT=$TMP/mount-unknown-result
mkdir -p "$MOUNT_UNKNOWN_RESULT" "$TMP/mount-unknown-mount" \
    "$TMP/mount-unknown-sys/bus/pci/devices/fake-pci"
if library_case '
    SYS_ROOT=$TEST_TMP/mount-unknown-sys
    RESTORE_ARMED=1
    RESULT_CREATED=1
    PART_CREATED=0
    MOUNTED_BY_US=0
    LOGGING_ACTIVE=0
    WORK_SUCCEEDED=0
    result_dir=$TEST_TMP/mount-unknown-result
    mount_dir=$TEST_TMP/mount-unknown-mount
    PCI=fake-pci
    WHOLE=$TEST_TMP/fake-whole
    ORIG_DRIVER=nvmex
    ORIG_OVERRIDE=nvmex
    ORIG_POLL=0
    ORIG_TARGET_MOUNTS=
    ORIG_MOUNT_DIR_RECORD=
    IOMMU_GROUP=7
    driver_for() { printf "%s\n" nvmex; }
    read_override() { printf "%s\n" nvmex; }
    read_one() {
        case "$1" in
            *poll_queues) printf "%s\n" 0 ;;
            *boot_id) printf "%s\n" fake-boot ;;
            *) return 1 ;;
        esac
    }
    write_sysfs() { return 0; }
    set_override() { return 0; }
    mountpoint() { return 1; }
    findmnt() { printf "%s\n" "injected restore mount failure" >&2; return 1; }
    require_boot_stable() { return 0; }
    resolve_entry_identity() { return 0; }
    require_controller_unused() { return 0; }
    require_singleton_iommu() { return 0; }
    require_vfio_group_unused() { return 0; }
    write_final_manifest() { return 0; }
    cleanup
' >/dev/null 2>"$TMP/mount-unknown.err"; then
    bad "cleanup returns failure when restored mount state is unknowable"
elif grep -Fxq 'restored_target_mounts=unknown' \
        "$MOUNT_UNKNOWN_RESULT/restore.txt" &&
     grep -Fxq 'restored_mount_dir_record=unknown' \
        "$MOUNT_UNKNOWN_RESULT/restore.txt" &&
     grep -Fxq 'mount_present=unknown' "$MOUNT_UNKNOWN_RESULT/restore.txt" &&
     grep -Fxq 'cleanup_rc=1' "$MOUNT_UNKNOWN_RESULT/restore.txt"; then
    ok "restore evidence records unknown on mount probe failure"
else
    bad "restore evidence records unknown on mount probe failure"
fi

HCTX_OUT=$TMP/hctx.out
if library_case '
    SYS_ROOT=$TEST_TMP/hctx-sys
    mkdir -p "$SYS_ROOT/bus/pci/devices/fake-pci" \
        "$SYS_ROOT/class/block/fakens/mq"
    printf "%s\n" 0 >"$SYS_ROOT/bus/pci/devices/fake-pci/numa_node"
    for hctx in 0 1 2 3 4 5 6 7; do
        mkdir -p "$SYS_ROOT/class/block/fakens/mq/$hctx"
        printf "%s\n" "$hctx" \
            >"$SYS_ROOT/class/block/fakens/mq/$hctx/cpu_list"
    done
    lscpu() {
        printf "%s\n" "# CPU,Core,Socket,Node,Online"
        for cpu in 0 1 2 3 4 5 6 7; do
            printf "%s,%s,0,0,Y\n" "$cpu" "$cpu"
        done
    }
    select_poll_hctx_physical_cpus fake-pci fakens
    [ "$CPUS_CSV" = 0,1,2,3,4,5,6,7 ]
    [ "$HCTX_CPU_MAP" = 0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7 ]
    [ "$(printf "%s\n" "$MQ_SNAPSHOT" | wc -l)" -eq 8 ]
    printf "%s\n" "$CPUS_CSV $HCTX_CPU_MAP"
' >"$HCTX_OUT" 2>"$TMP/hctx.err"; then
    ok "poll CPU selection uses eight distinct hctx and physical cores on target NUMA"
else
    bad "poll CPU selection uses eight distinct hctx and physical cores on target NUMA"
fi

STALE_MTIME_ERR=$TMP/stale-dso-mtime.err
if library_case '
    repo=$TEST_TMP/stale-dso-mtime-repo
    mkdir -p "$repo/src" "$repo/include"
    printf "%s\n" source >"$repo/src/input.c"
    printf "%s\n" header >"$repo/include/input.h"
    printf "%s\n" target >"$repo/Makefile"
    printf "%s\n" dso >"$repo/libexitos_bpftime_active.so"
    touch -t 202001010000 "$repo/libexitos_bpftime_active.so"
    touch -t 202101010000 "$repo/src/input.c" "$repo/include/input.h" \
        "$repo/Makefile"
    ACTIVE_SO=$repo/libexitos_bpftime_active.so
    make() { return 0; }
    require_active_dso_fresh
' >/dev/null 2>"$STALE_MTIME_ERR"; then
    bad "active DSO older than transferred source inputs is refused"
elif grep -Fq 'active_dso_older_than_source' "$STALE_MTIME_ERR"; then
    ok "active DSO older than transferred source inputs is refused"
else
    bad "active DSO older than transferred source inputs is refused (wrong failure)"
fi

STALE_GRAPH_ERR=$TMP/stale-dso-build-graph.err
if library_case '
    repo=$TEST_TMP/stale-dso-build-graph-repo
    mkdir -p "$repo/src" "$repo/include"
    printf "%s\n" source >"$repo/src/input.c"
    printf "%s\n" header >"$repo/include/input.h"
    printf "%s\n" target >"$repo/Makefile"
    printf "%s\n" dso >"$repo/libexitos_bpftime_active.so"
    touch -t 202001010000 "$repo/src/input.c" "$repo/include/input.h" \
        "$repo/Makefile"
    touch -t 202101010000 "$repo/libexitos_bpftime_active.so"
    ACTIVE_SO=$repo/libexitos_bpftime_active.so
    make() { return 1; }
    require_active_dso_fresh
' >/dev/null 2>"$STALE_GRAPH_ERR"; then
    bad "active DSO rejected when the build graph reports it stale"
elif grep -Fq 'active_dso_stale' "$STALE_GRAPH_ERR"; then
    ok "active DSO rejected when the build graph reports it stale"
else
    bad "active DSO rejected when the build graph reports it stale (wrong failure)"
fi

MQ_DRIFT_ERR=$TMP/mq-generation-drift.err
if library_case '
    result_dir=$TEST_TMP/mq-generation-result
    mkdir -p "$result_dir"
    PCI=fake-pci
    BASE=fakens
    ACTIVE_DEVT=259:0
    ACTIVE_DISKSEQ=7
    MQ_SNAPSHOT=$(printf "0\t0\n")
    require_boot_stable() { return 0; }
    driver_for() { printf "%s\n" nvme; }
    resolve_entry_identity() { return 0; }
    read_one() {
        case "$1" in
            */dev) printf "%s\n" 259:0 ;;
            */diskseq) printf "%s\n" 7 ;;
            *poll_queues) printf "%s\n" 8 ;;
            */queue/io_poll) printf "%s\n" 1 ;;
            */queue/write_cache) printf "%s\n" "write through" ;;
            *) return 1 ;;
        esac
    }
    capture_mq_snapshot() { printf "0\t1\n"; }
    verify_active_generation before-runner
' >/dev/null 2>"$MQ_DRIFT_ERR"; then
    bad "mq generation drift is refused before or after the runner"
elif grep -Fq 'before-runner_mq_snapshot_mismatch' "$MQ_DRIFT_ERR"; then
    ok "mq generation drift is refused before or after the runner"
else
    bad "mq generation drift is refused before or after the runner (wrong failure)"
fi

SECONDARY_CLEANUP_LOG=$TMP/secondary-cleanup-delpart.log
mkdir -p "$TMP/secondary-cleanup-proc/self" "$TMP/secondary-cleanup-mount"
printf '%s\n' '36 25 259:9 / /secondary rw - ext4 /dev/fake rw' \
    >"$TMP/secondary-cleanup-proc/self/mountinfo"
if library_case '
    PROC_ROOT=$TEST_TMP/secondary-cleanup-proc
    RESTORE_ARMED=1
    RESULT_CREATED=0
    PART_CREATED=1
    PART_STATE=created
    MOUNTED_BY_US=0
    LOGGING_ACTIVE=0
    PART_DEVT=259:9
    PART_BASE=fakep1
    PART=$TEST_TMP/fakep1
    WHOLE=$TEST_TMP/fake
    mount_dir=$TEST_TMP/secondary-cleanup-mount
    PCI=fake-pci
    ORIG_DRIVER=nvmex
    ORIG_OVERRIDE=nvmex
    ORIG_POLL=0
    SECONDARY_DELPART_LOG=$TEST_TMP/secondary-cleanup-delpart.log
    mountpoint() { return 1; }
    driver_for() { return 0; }
    read_override() { return 1; }
    read_one() { return 1; }
    findmnt() { return 1; }
    require_boot_stable() { return 1; }
    verify_active_generation() { return 0; }
    resolve_temporary_partition_state() { printf "%s\n" "present 259:9"; }
    require_singleton_iommu() { return 1; }
    delpart() { printf "DELPART\n" >>"$SECONDARY_DELPART_LOG"; return 0; }
    cleanup
' >/dev/null 2>"$TMP/secondary-cleanup.err"; then
    bad "full cleanup refuses delpart while any devt mount remains"
elif [ ! -e "$SECONDARY_CLEANUP_LOG" ] &&
     grep -Fq 'partition_devt_is_mounted=259:9' \
        "$TMP/secondary-cleanup.err"; then
    ok "full cleanup refuses delpart while any devt mount remains"
else
    bad "full cleanup refuses delpart while any devt mount remains"
fi

AMBIGUOUS_CLEANUP_DELPART_LOG=$TMP/ambiguous-cleanup-delpart.log
AMBIGUOUS_CLEANUP_PROBE_LOG=$TMP/ambiguous-cleanup-probe.log
mkdir -p "$TMP/ambiguous-cleanup-proc/self" "$TMP/ambiguous-cleanup-mount"
: >"$TMP/ambiguous-cleanup-proc/self/mountinfo"
if library_case '
    PROC_ROOT=$TEST_TMP/ambiguous-cleanup-proc
    RESTORE_ARMED=1
    RESULT_CREATED=0
    PART_CREATED=1
    PART_STATE=ambiguous
    MOUNTED_BY_US=0
    LOGGING_ACTIVE=0
    PART_DEVT=259:9
    PART_BASE=fakep1
    PART=$TEST_TMP/fakep1
    WHOLE=$TEST_TMP/fake
    mount_dir=$TEST_TMP/ambiguous-cleanup-mount
    PCI=fake-pci
    ORIG_DRIVER=nvmex
    ORIG_OVERRIDE=nvmex
    ORIG_POLL=0
    AMBIGUOUS_DELPART_LOG=$TEST_TMP/ambiguous-cleanup-delpart.log
    AMBIGUOUS_PROBE_LOG=$TEST_TMP/ambiguous-cleanup-probe.log
    mountpoint() { return 1; }
    driver_for() { return 0; }
    read_override() { return 1; }
    read_one() { return 1; }
    findmnt() { return 1; }
    require_boot_stable() { return 1; }
    verify_active_generation() { return 0; }
    resolve_temporary_partition_state() { printf "%s\n" "present 259:9"; }
    require_singleton_iommu() { return 1; }
    delpart() { printf "DELPART\n" >>"$AMBIGUOUS_DELPART_LOG"; return 0; }
    probe_pci() { printf "PROBE\n" >>"$AMBIGUOUS_PROBE_LOG"; return 0; }
    cleanup
' >/dev/null 2>"$TMP/ambiguous-cleanup.err"; then
    bad "ambiguous addpart outcome with an exact visible partition blocks cleanup"
elif [ ! -e "$AMBIGUOUS_CLEANUP_DELPART_LOG" ] &&
     [ ! -e "$AMBIGUOUS_CLEANUP_PROBE_LOG" ] &&
     grep -Fq 'RESTORE_BLOCKED partition_ownership=ambiguous,state:present' \
        "$TMP/ambiguous-cleanup.err"; then
    ok "ambiguous addpart outcome with an exact visible partition blocks cleanup"
else
    bad "ambiguous addpart outcome with an exact visible partition blocks cleanup (wrong action)"
fi

if [ "$fail" -ne 0 ]; then
    printf 'FAIL test_nvme_4k_thread_qd1_transaction assertions=%d failures=%d\n' \
        "$((pass + fail))" "$fail" >&2
    exit 1
fi
printf 'PASS test_nvme_4k_thread_qd1_transaction assertions=%d\n' "$pass"
