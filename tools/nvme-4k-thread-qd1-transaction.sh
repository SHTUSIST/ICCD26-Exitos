#!/bin/bash
# Host-local transaction for the 4 KiB/QD1 thread-scaling gate on the NVMe
# test device.
# --plan is read-only.  --run resolves every volatile hardware name from the
# immutable controller/namespace/PCI identities and restores captured state.
set -Eeuo pipefail
umask 077
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

# The target device identity.  These three defaults are placeholders that match
# no real device, so an unedited copy of this script refuses instead of writing
# somewhere unintended.  Supply the identity of your own disposable test
# namespace, either by editing these lines or through the environment.  Whatever
# they hold is still compared byte-for-byte three ways: against the live
# controller and namespace, against the PCI function resolved by DSN, and
# against the --confirm-* arguments the operator has to repeat back.
EXPECTED_SERIAL=${EXITOS_TARGET_SERIAL:-EXAMPLESERIAL0001}
EXPECTED_WWID=${EXITOS_TARGET_WWID:-eui.00000000000000000000000000000000}
EXPECTED_PCI_DSN=${EXITOS_TARGET_PCI_DSN:-00-00-00-00-00-00-00-00}
# The entry binding this transaction is willing to disturb and put back.  The
# defaults describe an NVMe function on a stock kernel: the in-tree driver and
# no driver_override.  A host running a different NVMe module, or one that pins
# the function with an explicit override, supplies its own values.  Whatever
# they hold is compared byte-for-byte against the live entry state and the
# transaction refuses rather than proceeding on a mismatch.
EXPECTED_ENTRY_DRIVER="${EXITOS_TARGET_ENTRY_DRIVER:-nvme}"
EXPECTED_ENTRY_OVERRIDE="${EXITOS_TARGET_ENTRY_OVERRIDE:-(null)}"
# The driver the measurement runs on.  It is not always the entry driver: the
# transaction moves the function onto this one for the run and puts the entry
# driver back afterwards.  When the two are the same string, nothing about the
# binding is disturbed at all.
WORK_DRIVER=nvme
EXPECTED_ENTRY_POLL=0
ARTIFACT_ROOT=/root/exitos-artifacts
POLL_QUEUES=8
PART_NUMBER=1
PART_START_SECTORS=2048
PART_SIZE_SECTORS=50331648
PARTITION_BYTES=25769803776
SYS_ROOT=/sys
DEV_ROOT=/dev
PROC_ROOT=/proc
NVME_POLL_PATH=$SYS_ROOT/module/nvme/parameters/poll_queues

SELF_DIR=$(cd "$(dirname "$0")" && pwd -P)
DEFAULT_REPO=$(cd "$SELF_DIR/.." && pwd -P)
SELF=$SELF_DIR/nvme-4k-thread-qd1-transaction.sh

mode=plan
mode_seen=0
repo=$DEFAULT_REPO
mount_dir=
result_dir=
active_so=
transformer=
fio_bin=${FIO_BIN:-fio}
workload_runner=
workload_args=()
confirm_boot=
confirm_serial=
confirm_wwid=
confirm_pci_dsn=

ENTRY_BOOT=
PCI=
CTRL=
BASE=
WHOLE=
PART=
PART_BASE=
PART_DEVT=
IOMMU_GROUP=
NUMA_NODE=
CPUS_CSV=
HCTX_CPU_MAP=
MQ_SNAPSHOT=
ORIG_DRIVER=
ORIG_OVERRIDE=
ORIG_POLL=
ORIG_TARGET_MOUNTS=
ORIG_MOUNT_DIR_RECORD=
ACTIVE_DISKSEQ=
ACTIVE_DEVT=
ACTIVE_SO=
TRANSFORMER=
RUNNER=
FIO_RESOLVED=
MATRIX_RESULT=
RESTORE_ARMED=0
PCI_REBOUND_BY_US=0
RESULT_CREATED=0
PART_CREATED=0
PART_STATE=absent
MOUNTED_BY_US=0
WORK_SUCCEEDED=0
LOGGING_ACTIVE=0
STDOUT_TEE_PID=
STDERR_TEE_PID=
RESTORED_DRIVER=unknown
RESTORED_OVERRIDE=unknown
RESTORED_POLL=unknown
RESTORED_TARGET_MOUNTS=unknown
RESTORED_MOUNT_DIR_RECORD=unknown
TARGET_GENERIC_NODES=
SOURCE_MAX_MTIME=
ACTIVE_DSO_MTIME=

usage()
{
    cat <<'EOF'
Usage: tools/nvme-4k-thread-qd1-transaction.sh [--plan | --run] OPTIONS

Required:
  --mount ABS          existing empty directory reserved for temporary ext4
  --result-dir ABS     unique per-run directory under the artifact root
  --transformer ABS    patched bpftime text-transformer DSO

Optional:
  --repo ABS           repository containing this tool
  --active-so ABS      active-bpftime frontend DSO (default: REPO build)
  --fio PATH           fio executable (default: fio from PATH)
  --workload-runner ABS
                       executable workload runner inside REPO
  --workload-arg VALUE repeatable; appended verbatim to the workload runner's
                       argument list after the fixed ones. The transaction does
                       not interpret it; the runner owns its own contract.
  --plan               print the read-only state machine (default)
  --run                execute after all exact confirmations below

--run confirmations:
  --confirm-boot CURRENT-BOOT-ID
  --confirm-serial EXAMPLESERIAL0001
  --confirm-wwid eui.00000000000000000000000000000000
  --confirm-pci-dsn 00-00-00-00-00-00-00-00

The target namespace is disposable.  The transaction creates no persistent
partition table: it registers one temporary 24 GiB partition, builds ext4,
runs the default fixed 4 KiB/QD1 4T/8T matrix or the explicitly selected
in-repository workload runner, then removes the partition and restores the
captured driver, driver_override, poll_queues, and mount state.  A custom
runner owns and prints its workload contract.
EOF
}

die()
{
    printf 'NVME_4K_THREAD_QD1_REFUSE %s\n' "$*" >&2
    exit 3
}

need_value()
{
    [ "$#" -ge 2 ] && [ -n "$2" ] || die "missing_value=$1"
}

require_absolute()
{
    [[ $2 == /* ]] || die "$1_not_absolute=$2"
    [[ $2 != *$'\n'* ]] || die "$1_contains_newline"
}

require_equal()
{
    local label=$1 expected=$2 actual=$3
    [ "$actual" = "$expected" ] ||
        die "${label}_mismatch=expected:$expected,actual:$actual"
}

read_one()
{
    local path=$1 value
    [ -r "$path" ] || die "unreadable=$path"
    IFS= read -r value <"$path" || [ -n "${value:-}" ] || die "empty=$path"
    printf '%s\n' "$value"
}

validate_result_dir()
{
    local path=$1 canonical relative
    require_absolute result_dir "$path"
    canonical=$(readlink -m -- "$path") || die result_dir_canonicalize_failed
    [ "$canonical" = "$path" ] || die "result_dir_not_canonical=$path"
    case "$path" in
        "$ARTIFACT_ROOT"/[A-Za-z0-9]* ) ;;
        *) die "result_dir_outside_dated_remote_root=$path" ;;
    esac
    relative=${path#"$ARTIFACT_ROOT"/}
    [[ $relative != */* ]] || die "result_run_id_not_direct_child=$relative"
    local leaf=${path##*/}
    [[ $leaf =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
        die "result_run_id_invalid=$leaf"
    [ "$leaf" != . ] && [ "$leaf" != .. ] || die "result_run_id_invalid=$leaf"
}

driver_for()
{
    local pci=$1 target
    [ -L "$SYS_ROOT/bus/pci/devices/$pci/driver" ] || return 0
    target=$(readlink -f -- "$SYS_ROOT/bus/pci/devices/$pci/driver") ||
        die "driver_link_unresolved=$pci"
    basename -- "$target"
}

read_override()
{
    read_one "$SYS_ROOT/bus/pci/devices/$1/driver_override"
}

write_sysfs()
{
    local path=$1 value=$2
    [ -w "$path" ] || die "sysfs_not_writable=$path"
    printf '%s\n' "$value" >"$path"
}

set_override()
{
    local pci=$1 value=$2 path=$SYS_ROOT/bus/pci/devices/$1/driver_override
    [ -w "$path" ] || return 1
    case "$value" in
        ''|'(null)') printf '\n' >"$path" ;;
        *) printf '%s\n' "$value" >"$path" ;;
    esac
}

path_belongs_to_pci()
{
    local path=$1 pci=$2 resolved
    resolved=$(readlink -f -- "$path" 2>/dev/null) || return 1
    case "$resolved/" in
        *"/$pci/"*) return 0 ;;
        *) return 1 ;;
    esac
}

resolve_unique_pci_by_dsn()
{
    local expected=$1 dev bdf class dsn
    local -a matches=()
    for dev in "$SYS_ROOT"/bus/pci/devices/*; do
        [ -r "$dev/class" ] || continue
        class=$(read_one "$dev/class")
        [ "$class" = 0x010802 ] || continue
        bdf=${dev##*/}
        dsn=$(LC_ALL=C lspci -s "$bdf" -vv 2>/dev/null |
            sed -n 's/.*Device Serial Number //p' | head -1)
        [ "$dsn" = "$expected" ] && matches+=("$bdf")
    done
    [ "${#matches[@]}" -eq 1 ] ||
        die "pci_dsn_match_count=expected:1,actual:${#matches[@]}"
    printf '%s\n' "${matches[0]}"
}

resolve_controller_serial()
{
    local pci=$1 expected=$2 entry serial
    local -a matches=()
    for entry in "$SYS_ROOT"/class/nvme/nvme*; do
        [ -r "$entry/serial" ] || continue
        path_belongs_to_pci "$entry" "$pci" || continue
        serial=$(tr -d '[:space:]' <"$entry/serial")
        [ "$serial" = "$expected" ] && matches+=("${entry##*/}")
    done
    [ "${#matches[@]}" -eq 1 ] ||
        die "controller_serial_match_count=expected:1,actual:${#matches[@]}"
    CTRL=${matches[0]}
}

resolve_namespace_wwid()
{
    local pci=$1 expected=$2 entry wwid namespace_devt
    local -a matches=() namespaces=()
    for entry in "$SYS_ROOT"/class/block/*; do
        [ -e "$entry" ] || continue
        path_belongs_to_pci "$entry" "$pci" || continue
        [ ! -r "$entry/partition" ] || continue
        [ -r "$entry/wwid" ] || continue
        namespaces+=("${entry##*/}")
        wwid=$(tr -d '[:space:]' <"$entry/wwid")
        [ "$wwid" = "$expected" ] && matches+=("${entry##*/}")
    done
    [ "${#namespaces[@]}" -eq 1 ] ||
        die "namespace_count=expected:1,actual:${#namespaces[@]}"
    [ "${#matches[@]}" -eq 1 ] ||
        die "namespace_wwid_match_count=expected:1,actual:${#matches[@]}"
    BASE=${matches[0]}
    WHOLE=$DEV_ROOT/$BASE
    namespace_devt=$(read_one "$SYS_ROOT/class/block/$BASE/dev")
    require_block_devt "$WHOLE" "$namespace_devt" namespace
}

resolve_entry_identity()
{
    resolve_controller_serial "$1" "$EXPECTED_SERIAL"
    resolve_namespace_wwid "$1" "$EXPECTED_WWID"
}

require_boot_stable()
{
    require_equal boot_id "$ENTRY_BOOT" \
        "$(read_one "$PROC_ROOT/sys/kernel/random/boot_id")"
}

require_singleton_iommu()
{
    local pci=$1 link group member
    local -a members=()
    link=$SYS_ROOT/bus/pci/devices/$pci/iommu_group
    [ -L "$link" ] || die "iommu_group_missing=$pci"
    group=$(basename -- "$(readlink -f -- "$link")")
    for member in "$SYS_ROOT"/kernel/iommu_groups/$group/devices/*; do
        [ -e "$member" ] || continue
        members+=("${member##*/}")
    done
    [ "${#members[@]}" -eq 1 ] ||
        die "iommu_group_not_singleton=group:$group,count:${#members[@]}"
    require_equal iommu_member "$pci" "${members[0]}"
    IOMMU_GROUP=$group
}

require_block_node()
{
    [ -b "$1" ] || die "block_node_missing_or_wrong_type=$1"
}

require_block_devt()
{
    local node=$1 expected=$2 label=$3 actual
    require_block_node "$node"
    [[ $expected =~ ^[0-9]+:[0-9]+$ ]] ||
        die "${label}_sysfs_devt_invalid=$expected"
    actual=$(node_rdev_decimal "$node" "$label")
    require_equal "${label}_rdev" "$expected" "$actual"
}

require_char_node()
{
    [ -c "$1" ] || die "char_node_missing_or_wrong_type=$1"
}

node_rdev_decimal()
{
    local node=$1 label=$2 encoded major_hex minor_hex extra
    encoded=$(stat -Lc '%t:%T' -- "$node") ||
        die "${label}_rdev_stat_failed=$node"
    IFS=: read -r major_hex minor_hex extra <<<"$encoded"
    [[ $major_hex =~ ^[0-9A-Fa-f]+$ &&
       $minor_hex =~ ^[0-9A-Fa-f]+$ && -z ${extra:-} ]] ||
        die "${label}_rdev_malformed=$encoded"
    printf '%s:%s\n' "$((16#$major_hex))" "$((16#$minor_hex))"
}

require_char_devt()
{
    local node=$1 expected=$2 label=$3 actual
    require_char_node "$node"
    [[ $expected =~ ^[0-9]+:[0-9]+$ ]] ||
        die "${label}_sysfs_devt_invalid=$expected"
    actual=$(node_rdev_decimal "$node" "$label")
    require_equal "${label}_rdev" "$expected" "$actual"
}

# fuser returns 0 when an opener exists and 1 when none exists.  It also uses
# 1 for several probe failures, so an allegedly clean result is accepted only
# when it emitted no diagnostic text at all.
require_no_openers()
{
    local node=$1 label=$2 output rc
    if output=$(fuser "$node" 2>&1); then
        rc=0
    else
        rc=$?
    fi
    case "$rc" in
        0) die "${label}_has_opener=node:$node,detail:${output:-present}" ;;
        1)
            [ -z "$output" ] ||
                die "opener_probe_error=label:$label,node:$node,detail:$output"
            ;;
        *) die "opener_probe_error=label:$label,node:$node,rc:$rc,detail:$output" ;;
    esac
}

require_target_generic_nodes_unused()
{
    local pci=$1 entry node generic_devt
    local -a nodes=()
    for entry in "$SYS_ROOT"/class/nvme-generic/ng*; do
        [ -e "$entry" ] || continue
        path_belongs_to_pci "$entry" "$pci" || continue
        node=$DEV_ROOT/${entry##*/}
        generic_devt=$(read_one "$entry/dev")
        require_char_devt "$node" "$generic_devt" generic_node
        require_no_openers "$node" generic_node
        nodes+=("$node")
    done
    [ "${#nodes[@]}" -eq 1 ] ||
        die "target_generic_node_count=expected:1,actual:${#nodes[@]}"
    TARGET_GENERIC_NODES=${nodes[0]}
}

require_namespace_inflight_idle()
{
    local base=$1 path inflight_read inflight_write inflight_extra
    path=$SYS_ROOT/class/block/$base/inflight
    [ -r "$path" ] || die "namespace_inflight_unreadable=$path"
    read -r inflight_read inflight_write inflight_extra <"$path" ||
        die "namespace_inflight_unreadable=$path"
    [[ $inflight_read =~ ^[0-9]+$ && $inflight_write =~ ^[0-9]+$ ]] ||
        die "namespace_inflight_malformed=read:$inflight_read,write:$inflight_write"
    [ -z "${inflight_extra:-}" ] || die namespace_inflight_extra_field
    [ "$inflight_read" -eq 0 ] && [ "$inflight_write" -eq 0 ] ||
        die "namespace_has_inflight=read:$inflight_read,write:$inflight_write"
}

require_devt_unmounted()
{
    local devt=$1 rc
    [[ $devt =~ ^[0-9]+:[0-9]+$ ]] || die "partition_devt_invalid=$devt"
    [ -r "$PROC_ROOT/self/mountinfo" ] || die mountinfo_unreadable
    if awk -v expected="$devt" '$3 == expected { found = 1 }
            END { exit !found }' "$PROC_ROOT/self/mountinfo"; then
        die "partition_devt_is_mounted=$devt"
    else
        rc=$?
    fi
    [ "$rc" -eq 1 ] || die "mountinfo_probe_error=devt:$devt,rc:$rc"
    return 0
}

require_vfio_group_unused()
{
    local group=$1 node=$DEV_ROOT/vfio/$1
    [ ! -e "$node" ] || require_no_openers "$node" vfio_group
}

require_no_partition_children()
{
    local pci=$1 entry count=0
    for entry in "$SYS_ROOT"/class/block/*; do
        [ -r "$entry/partition" ] || continue
        path_belongs_to_pci "$entry" "$pci" || continue
        count=$((count + 1))
    done
    [ "$count" -eq 0 ] ||
        die "namespace_has_partition_children=count:$count"
}

require_no_dm_md()
{
    local base=$1 holder candidate holder_name
    for holder in "$SYS_ROOT/class/block/$base/holders"/*; do
        [ -e "$holder" ] || continue
        holder_name=${holder##*/}
        if [ -d "$SYS_ROOT/class/block/$holder_name/dm" ]; then
            die "namespace_is_dm_member=holder:$holder_name"
        fi
        if [ -d "$SYS_ROOT/class/block/$holder_name/md" ]; then
            die "namespace_is_md_member=holder:$holder_name"
        fi
        die "namespace_has_holder=$holder_name"
    done
    for candidate in "$SYS_ROOT"/class/block/dm-*; do
        [ -e "$candidate/slaves/$base" ] &&
            die "namespace_is_dm_member=holder:${candidate##*/}"
    done
    for candidate in "$SYS_ROOT"/class/block/md*; do
        [ -e "$candidate/slaves/$base" ] &&
            die "namespace_is_md_member=holder:${candidate##*/}"
    done
    return 0
}

require_no_swap()
{
    local node=$1 swap rest swap_real swap_backing node_real tree line index
    local -a swap_lines=()
    node_real=$(readlink -f -- "$node") || die "node_unresolved=$node"
    [ -r "$PROC_ROOT/swaps" ] || die "swap_table_unreadable=$PROC_ROOT/swaps"
    mapfile -t swap_lines <"$PROC_ROOT/swaps" || die swap_table_read_failed
    [ "${#swap_lines[@]}" -ge 1 ] || die swap_table_empty
    read -r swap rest <<<"${swap_lines[0]}"
    [ "$swap" = Filename ] || die swap_table_header_invalid
    for ((index = 1; index < ${#swap_lines[@]}; index++)); do
        line=${swap_lines[$index]}
        [ -n "$line" ] || continue
        read -r swap rest <<<"$line"
        [ -n "$swap" ] && [ -n "$rest" ] || die swap_table_row_invalid
        swap_real=$(readlink -f -- "$swap") ||
            die "swap_path_resolve_failed=$swap"
        swap_backing=$(resolve_swap_backing_node "$swap_real")
        if ! tree=$(lsblk -snrpo NAME "$swap_backing" 2>&1); then
            die "swap_backing_tree_probe_failed=$swap_backing"
        fi
        [ -n "$tree" ] || die "swap_backing_tree_empty=$swap_backing"
        if printf '%s\n' "$tree" | grep -Fxq "$node_real"; then
            die "namespace_is_swap=$node"
        fi
    done
}

resolve_swap_backing_node()
{
    local path=$1 mount_source source_device
    if [ -b "$path" ]; then
        printf '%s\n' "$path"
        return 0
    fi
    [ -f "$path" ] || die "swap_path_not_block_or_regular=$path"
    if ! mount_source=$(findmnt -rn -e -T "$path" -o SOURCE 2>&1); then
        die "swapfile_mount_probe_failed=$path"
    fi
    [ -n "$mount_source" ] && [[ $mount_source != *$'\n'* ]] ||
        die "swapfile_mount_source_invalid=$path"
    source_device=${mount_source%%\[*}
    case "$source_device" in
        /dev/*) ;;
        *) die "swapfile_backing_source_not_block=$source_device" ;;
    esac
    require_block_node "$source_device"
    printf '%s\n' "$source_device"
}

require_not_system_storage()
{
    local node=$1 source source_device source_real tree mount_sources node_real
    node_real=$(readlink -f -- "$node") || die "node_unresolved=$node"
    if ! mount_sources=$(findmnt -rn -e -o SOURCE 2>&1); then
        die system_mount_probe_failed
    fi
    [ -n "$mount_sources" ] || die system_mount_probe_empty
    while IFS= read -r source; do
        [ -n "$source" ] || die system_mount_source_empty
        source_device=${source%%\[*}
        case "$source_device" in
            /dev/*)
                [ -b "$source_device" ] ||
                    die "system_mount_source_not_block=$source_device"
                source_real=$(readlink -f -- "$source_device") ||
                    die "system_mount_source_resolve_failed=$source_device"
                if ! tree=$(lsblk -snrpo NAME "$source_real" 2>&1); then
                    die "system_mount_tree_probe_failed=$source_real"
                fi
                [ -n "$tree" ] ||
                    die "system_mount_tree_empty=$source_real"
                if printf '%s\n' "$tree" | grep -Fxq "$node_real"; then
                    die "namespace_backs_system_storage=source:$source_real"
                fi
                ;;
            UUID=*|LABEL=*|PARTUUID=*|PARTLABEL=*)
                die "system_mount_source_unresolved=$source_device"
                ;;
        esac
    done <<<"$mount_sources"
}

require_controller_unused()
{
    local pci=$1 ctrl=$2 base=$3 node=$DEV_ROOT/$3 devt ctrl_devt mounts
    require_no_partition_children "$pci"
    require_no_dm_md "$base"
    mounts=$(findmnt_optional namespace-source -rn -S "$node" -o TARGET)
    [ -z "$mounts" ] || die "namespace_is_mounted=$node"
    devt=$(read_one "$SYS_ROOT/class/block/$base/dev")
    require_block_devt "$node" "$devt" namespace
    require_devt_unmounted "$devt"
    require_no_swap "$node"
    require_not_system_storage "$node"
    require_no_openers "$node" namespace
    ctrl_devt=$(read_one "$SYS_ROOT/class/nvme/$ctrl/dev")
    require_char_devt "$DEV_ROOT/$ctrl" "$ctrl_devt" controller
    require_no_openers "$DEV_ROOT/$ctrl" controller
    require_target_generic_nodes_unused "$pci"
    require_namespace_inflight_idle "$base"
    return 0
}

capture_mount_state()
{
    ORIG_TARGET_MOUNTS=$(findmnt_optional entry-target-mounts -rn -S "$WHOLE" \
        -o SOURCE,TARGET,FSTYPE,OPTIONS)
    ORIG_MOUNT_DIR_RECORD=$(findmnt_optional entry-mount-dir -rn -M "$mount_dir" \
        -o SOURCE,TARGET,FSTYPE,OPTIONS)
    [ -z "$ORIG_TARGET_MOUNTS" ] || die "target_entry_mounted=$ORIG_TARGET_MOUNTS"
    [ -z "$ORIG_MOUNT_DIR_RECORD" ] ||
        die "mount_dir_entry_mounted=$ORIG_MOUNT_DIR_RECORD"
}

findmnt_optional()
{
    local label=$1 output rc
    shift
    if output=$(findmnt "$@" 2>&1); then
        [ -z "$output" ] || printf '%s\n' "$output"
        return 0
    else
        rc=$?
    fi
    if [ "$rc" -eq 1 ] && [ -z "$output" ]; then
        return 0
    fi
    die "findmnt_probe_error=$label,rc:$rc,detail:${output:-none}"
}

# ext4 represents nodiscard by leaving the discard mount flag clear.  Since
# that is also the default, mountinfo/findmnt commonly omits the literal word
# "nodiscard" even when it was explicitly requested.  Prove the effective
# policy instead: discard must be absent and noatime must be present.
require_mount_nodiscard_noatime()
{
    local options=$1
    case ",$options," in
        *,discard,*|*,discard=*,*) die mount_discard_enabled ;;
    esac
    case ",$options," in
        *,noatime,*) ;;
        *) die mount_missing_noatime ;;
    esac
}

mountpoint_devt()
{
    local target=$1 record devt extra
    record=$(findmnt -rn -o MAJ:MIN -M "$target") || return 1
    read -r devt extra <<<"$record"
    [[ $devt =~ ^[0-9]+:[0-9]+$ && -z ${extra:-} ]] || return 1
    printf '%s\n' "$devt"
}

select_numa_physical_cpus()
{
    local pci=$1 cpu core socket node online key
    local -a selected=()
    declare -A seen_core=()
    NUMA_NODE=$(read_one "$SYS_ROOT/bus/pci/devices/$pci/numa_node")
    [[ $NUMA_NODE =~ ^-?[0-9]+$ ]] || die "numa_node_invalid=$NUMA_NODE"
    while IFS=, read -r cpu core socket node online; do
        [[ $cpu =~ ^[0-9]+$ && $core =~ ^[0-9]+$ &&
           $socket =~ ^[0-9]+$ && $node =~ ^-?[0-9]+$ ]] || continue
        [ "$online" = Y ] || continue
        if [ "$NUMA_NODE" -ge 0 ] && [ "$node" -ne "$NUMA_NODE" ]; then
            continue
        fi
        key=$socket:$core
        [ -z "${seen_core[$key]+x}" ] || continue
        seen_core[$key]=1
        selected+=("$cpu")
        [ "${#selected[@]}" -eq 8 ] && break
    done < <(LC_ALL=C lscpu -p=CPU,CORE,SOCKET,NODE,ONLINE)
    [ "${#selected[@]}" -eq 8 ] ||
        die "target_numa_physical_cpu_count=needed:8,actual:${#selected[@]},node:$NUMA_NODE"
    CPUS_CSV=$(IFS=,; printf '%s' "${selected[*]}")
}

capture_mq_snapshot()
{
    local base=$1 hctx cpu_list count=0
    for hctx in "$SYS_ROOT/class/block/$base/mq"/[0-9]*; do
        [ -d "$hctx" ] || continue
        cpu_list=$(read_one "$hctx/cpu_list")
        cpu_list=${cpu_list//[[:space:]]/}
        [[ $cpu_list =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] ||
            die "mq_cpu_list_invalid=hctx:${hctx##*/},value:$cpu_list"
        printf '%s\t%s\n' "${hctx##*/}" "$cpu_list"
        count=$((count + 1))
    done
    [ "$count" -ge "$POLL_QUEUES" ] ||
        die "mq_hctx_count=needed:$POLL_QUEUES,actual:$count"
}

expand_cpu_list()
{
    local list=$1 token first last cpu
    IFS=, read -r -a tokens <<<"$list"
    for token in "${tokens[@]}"; do
        case "$token" in
            *-*)
                first=${token%-*}
                last=${token#*-}
                [[ $first =~ ^[0-9]+$ && $last =~ ^[0-9]+$ &&
                   $first -le $last ]] || die "cpu_list_range_invalid=$token"
                for ((cpu = first; cpu <= last; cpu++)); do
                    printf '%s\n' "$cpu"
                done
                ;;
            *)
                [[ $token =~ ^[0-9]+$ ]] || die "cpu_list_token_invalid=$token"
                printf '%s\n' "$token"
                ;;
        esac
    done
}

select_poll_hctx_physical_cpus()
{
    local pci=$1 base=$2 cpu core socket node online key hctx cpu_list chosen
    local -a selected=() mappings=()
    declare -A cpu_node=() cpu_core=() cpu_online=() seen_core=()

    NUMA_NODE=$(read_one "$SYS_ROOT/bus/pci/devices/$pci/numa_node")
    [[ $NUMA_NODE =~ ^[0-9]+$ ]] || die "numa_node_not_provable=$NUMA_NODE"
    while IFS=, read -r cpu core socket node online; do
        [[ $cpu =~ ^[0-9]+$ && $core =~ ^[0-9]+$ &&
           $socket =~ ^[0-9]+$ && $node =~ ^[0-9]+$ ]] || continue
        cpu_node[$cpu]=$node
        cpu_core[$cpu]=$socket:$core
        cpu_online[$cpu]=$online
    done < <(LC_ALL=C lscpu -p=CPU,CORE,SOCKET,NODE,ONLINE)

    MQ_SNAPSHOT=$(capture_mq_snapshot "$base")
    while IFS=$'\t' read -r hctx cpu_list; do
        [ -n "$hctx" ] || continue
        chosen=
        while IFS= read -r cpu; do
            [ "${cpu_online[$cpu]-}" = Y ] || continue
            [ "${cpu_node[$cpu]-}" = "$NUMA_NODE" ] || continue
            key=${cpu_core[$cpu]-}
            [ -n "$key" ] || continue
            [ -z "${seen_core[$key]+x}" ] || continue
            chosen=$cpu
            seen_core[$key]=1
            break
        done < <(expand_cpu_list "$cpu_list")
        [ -n "$chosen" ] || continue
        selected+=("$chosen")
        mappings+=("$hctx:$chosen")
        [ "${#selected[@]}" -eq "$POLL_QUEUES" ] && break
    done <<<"$MQ_SNAPSHOT"
    [ "${#selected[@]}" -eq "$POLL_QUEUES" ] ||
        die "distinct_poll_hctx_numa_physical_cpu_count=needed:$POLL_QUEUES,actual:${#selected[@]}"
    CPUS_CSV=$(IFS=,; printf '%s' "${selected[*]}")
    HCTX_CPU_MAP=$(IFS=,; printf '%s' "${mappings[*]}")
}

verify_active_generation()
{
    local stage=$1 current_mq current_devt current_diskseq
    require_boot_stable
    require_equal "${stage}_driver" nvme "$(driver_for "$PCI")"
    resolve_entry_identity "$PCI"
    current_devt=$(read_one "$SYS_ROOT/class/block/$BASE/dev")
    current_diskseq=$(read_one "$SYS_ROOT/class/block/$BASE/diskseq")
    current_mq=$(capture_mq_snapshot "$BASE")
    require_equal "${stage}_devt" "$ACTIVE_DEVT" "$current_devt"
    require_equal "${stage}_diskseq" "$ACTIVE_DISKSEQ" "$current_diskseq"
    require_equal "${stage}_mq_snapshot" "$MQ_SNAPSHOT" "$current_mq"
    require_equal "${stage}_poll_queues" "$POLL_QUEUES" \
        "$(read_one "$NVME_POLL_PATH")"
    require_equal "${stage}_io_poll" 1 \
        "$(read_one "$SYS_ROOT/class/block/$BASE/queue/io_poll")"
    require_equal "${stage}_write_cache" "write through" \
        "$(read_one "$SYS_ROOT/class/block/$BASE/queue/write_cache")"
    printf '%s\n' "$current_mq" >"$result_dir/mq-${stage}.txt"
}

wait_for_driver()
{
    local pci=$1 expected=$2 current tries
    for tries in $(seq 1 200); do
        current=$(driver_for "$pci")
        [ "$current" = "$expected" ] && return 0
        sleep 0.1
    done
    die "driver_wait_timeout=expected:$expected,actual:${current:-none}"
}

wait_for_block()
{
    local node=$1 tries
    for tries in $(seq 1 200); do
        [ -b "$node" ] && return 0
        sleep 0.1
    done
    die "block_wait_timeout=$node"
}

unbind_exact_driver()
{
    local pci=$1 expected=$2 current path
    current=$(driver_for "$pci")
    require_equal pre_unbind_driver "$expected" "$current"
    path=$SYS_ROOT/bus/pci/drivers/$expected/unbind
    write_sysfs "$path" "$pci"
    [ -z "$(driver_for "$pci")" ] || die "driver_still_bound=$pci"
}

unbind_if_driver()
{
    local pci=$1 expected=$2 current path
    current=$(driver_for "$pci")
    [ "$current" = "$expected" ] || return 0
    path=$SYS_ROOT/bus/pci/drivers/$expected/unbind
    [ -w "$path" ] || return 1
    printf '%s\n' "$pci" >"$path" || return 1
    [ -z "$(driver_for "$pci")" ]
}

probe_pci()
{
    write_sysfs "$SYS_ROOT/bus/pci/drivers_probe" "$1"
}

partition_node_for()
{
    local whole=$1 number=$2
    case "$whole" in
        *[0-9]) printf '%sp%s\n' "$whole" "$number" ;;
        *) printf '%s%s\n' "$whole" "$number" ;;
    esac
}

# addpart's ioctl is atomic on its normal return path, but the helper process
# can be killed after the kernel committed the partition and before the parent
# observed success.  Resolve that outcome from the still-pinned controller
# generation; never infer absence merely from addpart's exit status.
resolve_temporary_partition_state()
{
    local part_dir number start size devt
    [ -n "$PART_BASE" ] || die temporary_partition_name_missing
    part_dir=$SYS_ROOT/class/block/$PART_BASE
    if [ -L "$part_dir" ] && [ ! -e "$part_dir" ]; then
        die "temporary_partition_sysfs_unresolved=$PART_BASE"
    fi
    if [ -e "$part_dir" ]; then
        path_belongs_to_pci "$part_dir" "$PCI" ||
            die "temporary_partition_wrong_pci=$PART_BASE"
        number=$(read_one "$part_dir/partition")
        start=$(read_one "$part_dir/start")
        size=$(read_one "$part_dir/size")
        devt=$(read_one "$part_dir/dev")
        require_equal temporary_partition_number "$PART_NUMBER" "$number"
        require_equal temporary_partition_start "$PART_START_SECTORS" "$start"
        require_equal temporary_partition_size "$PART_SIZE_SECTORS" "$size"
        [[ $devt =~ ^[0-9]+:[0-9]+$ ]] ||
            die "temporary_partition_devt_invalid=$devt"
        printf 'present %s\n' "$devt"
        return 0
    fi
    require_no_partition_children "$PCI"
    printf 'absent -\n'
}

require_active_dso_fresh()
{
    local input input_mtime count=0 max_mtime=0
    local -a inputs=("$repo"/src/*.c "$repo"/include/*.h "$repo"/Makefile)
    require_equal active_campaign_dso "$repo/libexitos_bpftime_active.so" \
        "$ACTIVE_SO"
    ACTIVE_DSO_MTIME=$(stat -Lc '%Y' "$ACTIVE_SO") ||
        die active_dso_stat_failed
    [[ $ACTIVE_DSO_MTIME =~ ^[0-9]+$ ]] || die active_dso_mtime_invalid
    for input in "${inputs[@]}"; do
        [ -f "$input" ] || continue
        input_mtime=$(stat -Lc '%Y' "$input") || die "source_input_stat_failed=$input"
        [[ $input_mtime =~ ^[0-9]+$ ]] || die "source_input_mtime_invalid=$input"
        [ "$input_mtime" -le "$ACTIVE_DSO_MTIME" ] ||
            die "active_dso_older_than_source=input:$input,input_mtime:$input_mtime,dso_mtime:$ACTIVE_DSO_MTIME"
        [ "$input_mtime" -le "$max_mtime" ] || max_mtime=$input_mtime
        count=$((count + 1))
    done
    [ "$count" -gt 1 ] || die "source_input_count_too_small=$count"
    SOURCE_MAX_MTIME=$max_mtime
    make -q -C "$repo" libexitos_bpftime_active.so ||
        die "active_dso_stale=run:make_-B_libexitos_bpftime_active.so"
}

observe_restore_state()
{
    local value pci_path=$SYS_ROOT/bus/pci/devices/$PCI
    RESTORED_DRIVER=unknown
    RESTORED_OVERRIDE=unknown
    RESTORED_POLL=unknown
    # An empty driver is evidence of an actually unbound function only while
    # the target PCI function itself is still observable.  If the device path
    # disappeared, do not turn absence of evidence into a successful state.
    if [ -n "$PCI" ] && [ -d "$pci_path" ] &&
       value=$(driver_for "$PCI" 2>/dev/null); then
        RESTORED_DRIVER=${value:-unbound}
    fi
    if value=$(read_override "$PCI" 2>/dev/null); then
        RESTORED_OVERRIDE=$value
    fi
    if value=$(read_one "$NVME_POLL_PATH" 2>/dev/null); then
        RESTORED_POLL=$value
    fi
}

# Restore both module/override prerequisites with readback before any probe.
# An unbound target stays unbound if either write or either readback fails.
restore_entry_binding()
{
    local current=$1 actual ready=1 restore_probe_override=0
    RESTORED_DRIVER=${current:-unbound}
    RESTORED_OVERRIDE=unknown
    RESTORED_POLL=unknown

    if (write_sysfs "$NVME_POLL_PATH" "$ORIG_POLL"); then
        if actual=$(read_one "$NVME_POLL_PATH" 2>/dev/null); then
            RESTORED_POLL=$actual
            [ "$actual" = "$ORIG_POLL" ] || ready=0
        else
            ready=0
        fi
    else
        actual=$(read_one "$NVME_POLL_PATH" 2>/dev/null) &&
            RESTORED_POLL=$actual
        ready=0
    fi

    if [ -n "$current" ] && [ "$current" != "$ORIG_DRIVER" ]; then
        ready=0
    fi
    if [ -z "$current" ] && [ "$ready" -eq 1 ]; then
        # The captured override can be empty while the captured driver is one
        # the kernel would not pick on its own.  Probe with a temporary exact
        # selector naming the entry driver; only after that driver is actually
        # bound is the empty override put back.  (The driver-name conjunct that
        # used to guard this branch was inert: the entry gate already forces
        # ORIG_DRIVER to one value, and the run dies earlier if it is empty.)
        if [ "$ORIG_OVERRIDE" = '(null)' ]; then
            if (set_override "$PCI" "$ORIG_DRIVER"); then
                restore_probe_override=1
            else
                ready=0
            fi
        elif ! (set_override "$PCI" "$ORIG_OVERRIDE"); then
            ready=0
        fi
        if [ "$ready" -eq 1 ]; then
            if ! (probe_pci "$PCI"); then
                ready=0
            elif ! (wait_for_driver "$PCI" "$ORIG_DRIVER"); then
                ready=0
            elif ! udevadm settle --timeout=30; then
                ready=0
            fi
        fi
    fi
    # Whether the temporary selector may be removed is decided from the
    # hardware, not from how the steps above returned.  The selector is the only
    # thing that will land this function back on its entry driver at the next
    # probe or the next boot, so erasing it after a rebind that did not happen
    # would leave the function unbound with nothing pointing home.  That is what
    # an unconditional `||` here used to do on every failed probe, wait or
    # settle.
    local clear_override=0
    if [ "$restore_probe_override" -eq 1 ]; then
        if [ "$(driver_for "$PCI" 2>/dev/null)" = "$ORIG_DRIVER" ]; then
            clear_override=1
        else
            printf 'RESTORE_SELECTOR_RETAINED driver_override=%s reason=%s\n' \
                "$ORIG_DRIVER" "entry_driver_not_bound" >&2
            ready=0
        fi
    elif [ "$ready" -eq 1 ]; then
        clear_override=1
    fi
    if [ "$clear_override" -eq 1 ]; then
        if (set_override "$PCI" "$ORIG_OVERRIDE"); then
            if actual=$(read_override "$PCI" 2>/dev/null); then
                RESTORED_OVERRIDE=$actual
                [ "$actual" = "$ORIG_OVERRIDE" ] || ready=0
            else
                ready=0
            fi
        else
            actual=$(read_override "$PCI" 2>/dev/null) &&
                RESTORED_OVERRIDE=$actual
            ready=0
        fi
    else
        actual=$(read_override "$PCI" 2>/dev/null) &&
            RESTORED_OVERRIDE=$actual
    fi
    observe_restore_state
    [ "$ready" -eq 1 ] &&
        [ "$RESTORED_DRIVER" = "$ORIG_DRIVER" ] &&
        [ "$RESTORED_OVERRIDE" = "$ORIG_OVERRIDE" ] &&
        [ "$RESTORED_POLL" = "$ORIG_POLL" ]
}

write_restore_record()
{
    local driver=$1 override=$2 poll=$3 cleanup_status=$4 boot=unknown actual
    local mount_present=unknown partition_present=unknown
    [ "$RESULT_CREATED" -eq 1 ] || return 0
    if actual=$(read_one "$PROC_ROOT/sys/kernel/random/boot_id" 2>/dev/null); then
        boot=$actual
    fi
    case "$RESTORED_MOUNT_DIR_RECORD" in
        unknown) ;;
        '') mount_present=no ;;
        *) mount_present=yes ;;
    esac
    if [ -z "$PART" ] || [ ! -e "$PART" ]; then
        partition_present=no
    elif [ -b "$PART" ]; then
        partition_present=yes
    fi
    {
        printf 'restore_time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'boot_id=%s\n' "$boot"
        printf 'pci=%s\n' "$PCI"
        printf 'driver=%s\n' "$driver"
        printf 'driver_override=%s\n' "$override"
        printf 'nvme_poll_queues=%s\n' "$poll"
        printf 'entry_target_mounts=%q\n' "$ORIG_TARGET_MOUNTS"
        printf 'entry_mount_dir_record=%q\n' "$ORIG_MOUNT_DIR_RECORD"
        printf 'restored_target_mounts=%q\n' "$RESTORED_TARGET_MOUNTS"
        printf 'restored_mount_dir_record=%q\n' "$RESTORED_MOUNT_DIR_RECORD"
        printf 'mount_present=%s\n' "$mount_present"
        printf 'partition_present=%s\n' "$partition_present"
        printf 'work_succeeded=%s\ncleanup_rc=%s\n' \
            "$WORK_SUCCEEDED" "$cleanup_status"
        printf 'manifest_owner=outer\n'
    } >"$result_dir/restore.txt"
}

write_final_manifest()
{
    [ "$RESULT_CREATED" -eq 1 ] || return 0
    [ ! -e "$result_dir/final-manifest.sha256" ] || return 1
    (
        cd "$result_dir" || exit 1
        {
            find . -type f ! -path './final-manifest.sha256' -print0
            find "$repo/src" "$repo/include" -maxdepth 1 -type f \
                \( -name '*.c' -o -name '*.h' \) -print0
            find "$(dirname -- "$RUNNER")" -maxdepth 1 -type f -print0
            printf '%s\0' "$SELF" "$RUNNER" "$ACTIVE_SO" "$TRANSFORMER" \
                "$FIO_RESOLVED" "$repo/Makefile"
        } | LC_ALL=C sort -zu | xargs -0 -r sha256sum --
    ) >"$result_dir/final-manifest.sha256" || return 1
    [ -s "$result_dir/final-manifest.sha256" ]
}

cleanup()
{
    local run_rc=$? cleanup_rc=0 current actual_mount_state
    local mounted_devt partition_state_record partition_state partition_state_devt
    local partition_state_extra
    local restore_can_rebind=1 current_known=1
    trap - EXIT
    # Cleanup is already the last recovery boundary.  A second signal must not
    # interrupt it between unbind, prerequisite restoration, and re-probe.
    trap '' INT TERM HUP
    set +e
    observe_restore_state

    if [ "$RESTORE_ARMED" -eq 1 ]; then
        printf 'RESTORE_BEGIN original_rc=%s\n' "$run_rc"
        if [ "$MOUNTED_BY_US" -eq 1 ] && mountpoint -q -- "$mount_dir"; then
            mounted_devt=$(mountpoint_devt "$mount_dir" 2>/dev/null || true)
            if [ -z "$PART_DEVT" ] || [ "$mounted_devt" != "$PART_DEVT" ]; then
                printf 'RESTORE_BLOCKED mount_devt=expected:%s,actual:%s\n' \
                    "${PART_DEVT:-unknown}" "${mounted_devt:-unknown}" >&2
                cleanup_rc=1
                restore_can_rebind=0
            else
                sync -f "$mount_dir" || cleanup_rc=1
                umount -- "$mount_dir" || cleanup_rc=1
            fi
        fi
        if mountpoint -q -- "$mount_dir"; then
            printf 'RESTORE_BLOCKED mount_still_active=%s\n' "$mount_dir" >&2
            cleanup_rc=1
            restore_can_rebind=0
        else
            MOUNTED_BY_US=0
            if [ "$PART_CREATED" -eq 1 ]; then
                if ! (verify_active_generation cleanup-pre-delpart); then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif partition_state_record=$(resolve_temporary_partition_state); then
                    read -r partition_state partition_state_devt \
                        partition_state_extra <<<"$partition_state_record"
                    case "$partition_state:$partition_state_devt:${partition_state_extra:-}" in
                        present:*:)
                            PART_CREATED=1
                            PART_DEVT=$partition_state_devt
                            if [ "$PART_STATE" != created ]; then
                                printf 'RESTORE_BLOCKED partition_ownership=%s,state:present\n' \
                                    "$PART_STATE" >&2
                                cleanup_rc=1
                                restore_can_rebind=0
                            fi
                            ;;
                        absent:-:)
                            PART_CREATED=0
                            PART_STATE=absent
                            PART_DEVT=
                            ;;
                        *)
                            cleanup_rc=1
                            restore_can_rebind=0
                            ;;
                    esac
                else
                    cleanup_rc=1
                    restore_can_rebind=0
                fi
                if [ "$restore_can_rebind" -eq 0 ]; then
                    :
                elif [ "$PART_CREATED" -eq 0 ]; then
                    :
                elif [ -z "$PART_DEVT" ] ||
                   ! (require_devt_unmounted "$PART_DEVT"); then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif ! (require_block_devt "$WHOLE" "$ACTIVE_DEVT" \
                         active_namespace); then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif [ -n "$PART" ] && [ -e "$PART" ] &&
                     ! (require_block_devt "$PART" "$PART_DEVT" partition); then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif [ -n "$PART" ] && [ -e "$PART" ] &&
                     ! (require_no_openers "$PART" partition); then
                    cleanup_rc=1
                    restore_can_rebind=0
                else
                    delpart "$WHOLE" "$PART_NUMBER" || cleanup_rc=1
                    udevadm settle --timeout=30 || cleanup_rc=1
                    if ! partition_state_record=$(resolve_temporary_partition_state); then
                        cleanup_rc=1
                        restore_can_rebind=0
                    elif [ "$partition_state_record" != 'absent -' ] ||
                         { [ -n "$PART" ] && [ -b "$PART" ]; }; then
                        cleanup_rc=1
                        restore_can_rebind=0
                    else
                        PART_CREATED=0
                        PART_STATE=absent
                    fi
                fi
            fi
        fi

        if current=$(driver_for "$PCI"); then
            :
        else
            current=
            current_known=0
            cleanup_rc=1
            restore_can_rebind=0
        fi
        # Acceptable drivers here are the entry driver, and the work driver
        # only when this transaction is the one that put it there.
        if [ -n "$current" ] && [ "$current" != "$ORIG_DRIVER" ] &&
           { [ "$PCI_REBOUND_BY_US" -ne 1 ] ||
             [ "$current" != "$WORK_DRIVER" ]; }; then
            printf 'RESTORE_BLOCKED unexpected_driver=%s\n' "$current" >&2
            cleanup_rc=1
            restore_can_rebind=0
        fi
        if [ "$restore_can_rebind" -eq 1 ] && [ -n "$current" ]; then
            (resolve_entry_identity "$PCI" &&
                require_controller_unused "$PCI" "$CTRL" "$BASE") || {
                    cleanup_rc=1
                    restore_can_rebind=0
                }
        fi
        if [ "$restore_can_rebind" -eq 1 ]; then
            # Unbind only what this transaction bound.  The unbind happens even
            # when the entry driver and the work driver are the same string:
            # poll_queues is a module parameter that the blk-mq tag set only
            # reads at probe time, so the restored value does not reach the
            # device until the function is probed again.  Skipping the unbind
            # here would leave the module parameter reading 0 while the device
            # still had its poll queues, and every restore check would pass.
            if [ "$PCI_REBOUND_BY_US" -eq 1 ] &&
               [ "$current" = "$WORK_DRIVER" ]; then
                if ! unbind_if_driver "$PCI" "$WORK_DRIVER"; then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif ! udevadm settle --timeout=30; then
                    cleanup_rc=1
                    restore_can_rebind=0
                elif ! current=$(driver_for "$PCI"); then
                    cleanup_rc=1
                    restore_can_rebind=0
                    current_known=0
                fi
            fi
            if [ -n "$current" ] && [ "$current" != "$ORIG_DRIVER" ]; then
                cleanup_rc=1
                restore_can_rebind=0
            fi
            if [ "$restore_can_rebind" -eq 1 ] &&
               ! restore_entry_binding "$current"; then
                cleanup_rc=1
                restore_can_rebind=0
            fi
        else
            printf 'RESTORE_BLOCKED partition_mount_or_identity_gate\n' >&2
        fi

        observe_restore_state
        [ "$RESTORED_DRIVER" = "$ORIG_DRIVER" ] || cleanup_rc=1
        [ "$RESTORED_OVERRIDE" = "$ORIG_OVERRIDE" ] || cleanup_rc=1
        [ "$RESTORED_POLL" = "$ORIG_POLL" ] || cleanup_rc=1
        (require_boot_stable) || cleanup_rc=1
        if [ "$RESTORED_DRIVER" = "$ORIG_DRIVER" ]; then
            (resolve_entry_identity "$PCI" &&
                require_controller_unused "$PCI" "$CTRL" "$BASE") || cleanup_rc=1
        fi
        RESTORED_TARGET_MOUNTS=unknown
        if actual_mount_state=$(findmnt_optional restored-target-mounts -rn \
            -S "$WHOLE" -o SOURCE,TARGET,FSTYPE,OPTIONS); then
            RESTORED_TARGET_MOUNTS=$actual_mount_state
            [ "$RESTORED_TARGET_MOUNTS" = "$ORIG_TARGET_MOUNTS" ] ||
                cleanup_rc=1
        else
            cleanup_rc=1
        fi
        RESTORED_MOUNT_DIR_RECORD=unknown
        if actual_mount_state=$(findmnt_optional restored-mount-dir -rn \
            -M "$mount_dir" -o SOURCE,TARGET,FSTYPE,OPTIONS); then
            RESTORED_MOUNT_DIR_RECORD=$actual_mount_state
            [ "$RESTORED_MOUNT_DIR_RECORD" = "$ORIG_MOUNT_DIR_RECORD" ] ||
                cleanup_rc=1
        else
            cleanup_rc=1
        fi
        (require_singleton_iommu "$PCI" &&
            require_vfio_group_unused "$IOMMU_GROUP") || cleanup_rc=1
        printf 'RESTORE_END cleanup_rc=%s\n' "$cleanup_rc"
    fi

    if [ "$LOGGING_ACTIVE" -eq 1 ]; then
        exec 1>&3 2>&4
        wait "$STDOUT_TEE_PID" || cleanup_rc=1
        wait "$STDERR_TEE_PID" || cleanup_rc=1
        LOGGING_ACTIVE=0
    fi
    if [ "$RESULT_CREATED" -eq 1 ]; then
        write_restore_record "$RESTORED_DRIVER" "$RESTORED_OVERRIDE" \
            "$RESTORED_POLL" "$cleanup_rc" || cleanup_rc=1
        if ! write_final_manifest; then
            cleanup_rc=1
            rm -f -- "$result_dir/final-manifest.sha256"
            write_restore_record "$RESTORED_DRIVER" "$RESTORED_OVERRIDE" \
                "$RESTORED_POLL" "$cleanup_rc" || true
        fi
    fi
    printf 'NVME_4K_THREAD_QD1_END original_rc=%s cleanup_rc=%s\n' \
        "$run_rc" "$cleanup_rc"
    [ "$run_rc" -eq 0 ] || exit "$run_rc"
    [ "$cleanup_rc" -eq 0 ] || exit 97
    exit 0
}

# Unit tests source the real safety/restore functions and replace only their
# external boundaries.  Library-only mode returns before argument parsing and
# can never perform a transaction or device mutation.
if [ "${EXITOS_TARGET_LIBRARY_ONLY:-0}" = 1 ]; then
    [ "${BASH_SOURCE[0]}" != "$0" ] || die library_only_requires_source
    return 0
fi

trap cleanup EXIT

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --plan)
            [ "$mode_seen" -eq 0 ] || die mode_selected_more_than_once
            mode=plan; mode_seen=1; shift
            ;;
        --run)
            [ "$mode_seen" -eq 0 ] || die mode_selected_more_than_once
            mode=run; mode_seen=1; shift
            ;;
        --repo|--mount|--result-dir|--active-so|--transformer|--fio|--workload-runner|--confirm-boot|--confirm-serial|--confirm-wwid|--confirm-pci-dsn)
            need_value "$@"
            key=${1#--}
            value=$2
            case "$key" in
                repo) repo=$value ;;
                mount) mount_dir=$value ;;
                result-dir) result_dir=$value ;;
                active-so) active_so=$value ;;
                transformer) transformer=$value ;;
                fio) fio_bin=$value ;;
                workload-runner) workload_runner=$value ;;
                confirm-boot) confirm_boot=$value ;;
                confirm-serial) confirm_serial=$value ;;
                confirm-wwid) confirm_wwid=$value ;;
                confirm-pci-dsn) confirm_pci_dsn=$value ;;
            esac
            shift 2
            ;;
        --workload-arg)
            [ "$#" -ge 2 ] || die "missing_value=$1"
            workload_args+=("$2")
            shift 2
            ;;
        *) die "unknown_option=$1" ;;
    esac
done

[ -n "$mount_dir" ] || die mount_missing
[ -n "$result_dir" ] || die result_dir_missing
[ -n "$transformer" ] || die transformer_missing
require_absolute repo "$repo"
require_absolute mount "$mount_dir"
require_absolute transformer "$transformer"
validate_result_dir "$result_dir"
[ -d "$repo" ] || die "repo_not_directory=$repo"
repo=$(readlink -f -- "$repo") || die repo_canonicalize_failed
[ -d "$mount_dir" ] || die "mount_not_directory=$mount_dir"
mount_dir=$(readlink -f -- "$mount_dir") || die mount_canonicalize_failed
[ -n "$active_so" ] || active_so=$repo/libexitos_bpftime_active.so
require_absolute active_so "$active_so"
if [ -n "$workload_runner" ]; then
    require_absolute workload_runner "$workload_runner"
else
    workload_runner=$repo/tools/run-4k-thread-qd1-matrix.sh
fi
RUNNER=$(readlink -f -- "$workload_runner") ||
    die "workload_runner_canonicalize_failed=$workload_runner"
case "$RUNNER" in
    "$repo"/*) ;;
    *) die "workload_runner_outside_repo=$RUNNER" ;;
esac
[ -x "$RUNNER" ] || die "workload_runner_not_executable=$RUNNER"
if [ "$RUNNER" = "$(readlink -f -- "$repo/tools/run-4k-thread-qd1-matrix.sh")" ]; then
    WORKLOAD_PLAN_LINES=$'workload_contract=fixed-4k-thread-qd1\nmatrix=threads:4/8,bs:4k,per-worker-qd:1,backend:uringpoll\nrunner_scheduler=SCHED_OTHER,nice:-20,scheduler-policy-pilots=4T+8T,parent=verified-per-cell'
    WORKLOAD_PLAN_STEP='run-fixed-4K-QD1-4T-8T-runner-with-dynamic-CPUs'
else
    WORKLOAD_PLAN_LINES='workload_contract=runner-owned:inspect-runner-plan'
    WORKLOAD_PLAN_STEP='run-selected-workload-runner-with-dynamic-CPUs'
fi
ACTIVE_SO=$active_so
TRANSFORMER=$transformer
MATRIX_RESULT=$result_dir/matrix
ENTRY_BOOT=$(read_one "$PROC_ROOT/sys/kernel/random/boot_id")

cat <<EOF
READ_ONLY_PLAN mode=$mode
expected_boot=$ENTRY_BOOT
expected_serial=$EXPECTED_SERIAL
expected_wwid=$EXPECTED_WWID
expected_pci_dsn=$EXPECTED_PCI_DSN
expected_entry=driver:$EXPECTED_ENTRY_DRIVER,override:$EXPECTED_ENTRY_OVERRIDE,nvme_poll_queues:$EXPECTED_ENTRY_POLL
work_driver=$WORK_DRIVER:the-function-is-moved-to-this-driver-for-the-run-and-put-back-after
identity_resolution=serial+PCI-DSN+namespace-WWID:no-device-name-or-BDF-cache
target_nvme_poll_queues=$POLL_QUEUES
partition_bytes=$PARTITION_BYTES
$WORKLOAD_PLAN_LINES
cpu_selection=post-bind-poll-mq,target-NUMA,distinct-hctx+physical-core,count:8
result_dir=$result_dir
matrix_result_dir=$MATRIX_RESULT
restore=entry-driver+driver-override+poll-queues+mount-state
unused_gate=not-system+unmounted+no-holder+no-dm+no-md+no-swap+no-openers+no-inflight
filesystem=temporary-partition,ext4:lazy_itable_init=0,lazy_journal_init=0,mkfs:nodiscard,mount:nodiscard+noatime
runner=$RUNNER
PLAN_STEP 01 resolve-PCI-by-DSN-controller-by-serial-namespace-by-WWID
PLAN_STEP 02 prove-target-not-system-unmounted-unheld-no-dm-md-swap-openers-or-inflight
PLAN_STEP 03 capture-and-gate-reviewed-entry-driver-override-poll0
PLAN_STEP 04 capture-driver-override-poll-and-empty-mount-state
PLAN_STEP 05 arm-unconditional-restore-before-first-mutation
PLAN_STEP 06 set-poll8-and-unbind-probe-only-resolved-target-PCI-function
PLAN_STEP 07 re-resolve-identity-require-io_poll1-and-select-eight-distinct-poll-hctx-cores
PLAN_STEP 08 add-temporary-partition-and-initialized-ext4-nodiscard
PLAN_STEP 09 $WORKLOAD_PLAN_STEP
PLAN_STEP 10 unmount-delpart-and-restore-exact-entry-state
EOF

if [ "$mode" = plan ]; then
    printf 'PLAN_OK read_only=yes device_action=no result=none artifacts=none\n'
    exit 0
fi
printf 'PLAN_REVIEW_COMPLETE mode=run device_action=pending-confirmations\n'

require_equal confirm_boot "$ENTRY_BOOT" "$confirm_boot"
require_equal confirm_serial "$EXPECTED_SERIAL" "$confirm_serial"
require_equal confirm_wwid "$EXPECTED_WWID" "$confirm_wwid"
require_equal confirm_pci_dsn "$EXPECTED_PCI_DSN" "$confirm_pci_dsn"
[ "$EUID" -eq 0 ] || die run_requires_root

for command in addpart awk basename blockdev chrt date delpart dirname env find \
    dd findmnt fuser grep head jq lscpu lsblk lspci make mkdir mkfs.ext4 mount \
    mountpoint nice nvme ps python3 readlink sed seq sha256sum sleep sort stat sync \
    taskset tee timeout tr udevadm umount xargs; do
    command -v -- "$command" >/dev/null || die "command_missing=$command"
done
[ -x "$RUNNER" ] || die "runner_not_executable=$RUNNER"
if [[ $fio_bin == */* ]]; then
    require_absolute fio "$fio_bin"
    FIO_RESOLVED=$(readlink -f -- "$fio_bin") || die "fio_not_found=$fio_bin"
else
    FIO_RESOLVED=$(command -v -- "$fio_bin") || die "fio_not_found=$fio_bin"
fi
[ -x "$FIO_RESOLVED" ] || die "fio_not_executable=$FIO_RESOLVED"
[ -r "$ACTIVE_SO" ] || die "active_so_unreadable=$ACTIVE_SO"
[ -r "$TRANSFORMER" ] || die "transformer_unreadable=$TRANSFORMER"
ACTIVE_SO=$(readlink -f -- "$ACTIVE_SO") || die active_so_canonicalize_failed
TRANSFORMER=$(readlink -f -- "$TRANSFORMER") || die transformer_canonicalize_failed
require_active_dso_fresh
clean_unsets=()
while IFS= read -r variable; do
    case "$variable" in
        EXITOS_*|LD_PRELOAD|LD_AUDIT) clean_unsets+=(-u "$variable") ;;
    esac
done < <(compgen -e)
[ -d "$ARTIFACT_ROOT" ] || die "artifact_root_missing=$ARTIFACT_ROOT"
[ ! -e "$result_dir" ] || die "result_dir_exists=$result_dir"
mountpoint -q -- "$mount_dir" && die "mount_already_active=$mount_dir"
[ -z "$(find "$mount_dir" -mindepth 1 -maxdepth 1 -print -quit)" ] ||
    die "mount_directory_not_empty=$mount_dir"

require_boot_stable
PCI=$(resolve_unique_pci_by_dsn "$EXPECTED_PCI_DSN")
resolve_entry_identity "$PCI"
require_singleton_iommu "$PCI"
require_vfio_group_unused "$IOMMU_GROUP"
require_controller_unused "$PCI" "$CTRL" "$BASE"
capture_mount_state
ORIG_DRIVER=$(driver_for "$PCI")
[ -n "$ORIG_DRIVER" ] || die entry_driver_unbound
ORIG_OVERRIDE=$(read_override "$PCI")
ORIG_POLL=$(read_one "$NVME_POLL_PATH")
[[ $ORIG_POLL =~ ^[0-9]+$ ]] || die "entry_poll_queues_invalid=$ORIG_POLL"
require_equal entry_driver "$EXPECTED_ENTRY_DRIVER" "$ORIG_DRIVER"
require_equal entry_override "$EXPECTED_ENTRY_OVERRIDE" "$ORIG_OVERRIDE"
require_equal entry_poll_queues "$EXPECTED_ENTRY_POLL" "$ORIG_POLL"

mkdir -m 0700 -- "$result_dir"
RESULT_CREATED=1
exec 3>&1 4>&2
exec > >(tee -a "$result_dir/transaction.stdout" >&3)
STDOUT_TEE_PID=$!
exec 2> >(tee -a "$result_dir/transaction.stderr" >&4)
STDERR_TEE_PID=$!
LOGGING_ACTIVE=1
{
    printf 'capture_time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'boot_id=%s\npci=%s\npci_dsn=%s\n' \
        "$ENTRY_BOOT" "$PCI" "$EXPECTED_PCI_DSN"
    printf 'controller=%s\nserial=%s\nwhole=%s\nwwid=%s\n' \
        "$CTRL" "$EXPECTED_SERIAL" "$WHOLE" "$EXPECTED_WWID"
    printf 'entry_driver=%s\nentry_override=%s\nentry_poll_queues=%s\n' \
        "$ORIG_DRIVER" "$ORIG_OVERRIDE" "$ORIG_POLL"
    printf 'entry_target_mounts=%q\nentry_mount_dir_record=%q\n' \
        "$ORIG_TARGET_MOUNTS" "$ORIG_MOUNT_DIR_RECORD"
    printf 'iommu_group=%s\ntarget_generic_nodes=%s\n' \
        "$IOMMU_GROUP" "$TARGET_GENERIC_NODES"
} >"$result_dir/entry-state.txt"
{
    printf 'freshness=make-q+source-input-mtime\n'
    printf 'active_dso_mtime=%s\nsource_max_mtime=%s\n' \
        "$ACTIVE_DSO_MTIME" "$SOURCE_MAX_MTIME"
    stat -Lc 'path=%n size=%s mtime=%Y inode=%i' \
        "$SELF" "$RUNNER" "$ACTIVE_SO" "$TRANSFORMER" "$FIO_RESOLVED"
    find "$repo/src" "$repo/include" -maxdepth 1 -type f \
        \( -name '*.c' -o -name '*.h' \) -exec \
        stat -Lc 'source_input=%n size=%s mtime=%Y inode=%i' {} +
    stat -Lc 'source_input=%n size=%s mtime=%Y inode=%i' "$repo/Makefile"
    lscpu -p=CPU,CORE,SOCKET,NODE,ONLINE
} >"$result_dir/build-and-cpu-provenance.txt"

# Repeat all identity and unused-state checks immediately before arming the
# cleanup transaction and performing the first mutable sysfs write.
require_boot_stable
require_equal pre_mutation_driver "$ORIG_DRIVER" "$(driver_for "$PCI")"
require_equal pre_mutation_override "$ORIG_OVERRIDE" "$(read_override "$PCI")"
require_equal pre_mutation_poll "$ORIG_POLL" "$(read_one "$NVME_POLL_PATH")"
resolve_entry_identity "$PCI"
require_singleton_iommu "$PCI"
require_vfio_group_unused "$IOMMU_GROUP"
require_controller_unused "$PCI" "$CTRL" "$BASE"
capture_mount_state

RESTORE_ARMED=1
trap 'exit 130' INT TERM HUP
write_sysfs "$NVME_POLL_PATH" "$POLL_QUEUES"
require_equal temporary_poll_queues "$POLL_QUEUES" \
    "$(read_one "$NVME_POLL_PATH")"
# From here the transaction owns the PCI binding.  Recorded as a flag rather
# than inferred later from the driver name: once the entry driver and the work
# driver are allowed to be the same string, the name cannot tell "we put it
# there" from "it was already there", and cleanup would unbind a function it
# never touched.
PCI_REBOUND_BY_US=1
set_override "$PCI" "$WORK_DRIVER"
require_equal temporary_driver_override "$WORK_DRIVER" "$(read_override "$PCI")"
unbind_exact_driver "$PCI" "$ORIG_DRIVER"
probe_pci "$PCI"
wait_for_driver "$PCI" "$WORK_DRIVER"
udevadm settle --timeout=30

require_boot_stable
require_equal pci_dsn "$EXPECTED_PCI_DSN" \
    "$(LC_ALL=C lspci -s "$PCI" -vv | sed -n 's/.*Device Serial Number //p' | head -1)"
require_equal post_probe_driver "$WORK_DRIVER" "$(driver_for "$PCI")"
resolve_entry_identity "$PCI"
require_singleton_iommu "$PCI"
require_vfio_group_unused "$IOMMU_GROUP"
require_controller_unused "$PCI" "$CTRL" "$BASE"
require_equal poll_queues "$POLL_QUEUES" "$(read_one "$NVME_POLL_PATH")"
require_equal io_poll 1 "$(read_one "$SYS_ROOT/class/block/$BASE/queue/io_poll")"
require_equal write_cache "write through" \
    "$(read_one "$SYS_ROOT/class/block/$BASE/queue/write_cache")"
ACTIVE_DISKSEQ=$(read_one "$SYS_ROOT/class/block/$BASE/diskseq")
ACTIVE_DEVT=$(read_one "$SYS_ROOT/class/block/$BASE/dev")
[[ $ACTIVE_DEVT =~ ^[0-9]+:[0-9]+$ ]] || die "active_devt_invalid=$ACTIVE_DEVT"
require_block_devt "$WHOLE" "$ACTIVE_DEVT" active_namespace
select_poll_hctx_physical_cpus "$PCI" "$BASE"
printf '%s\n' "$MQ_SNAPSHOT" >"$result_dir/mq-active-snapshot.txt"
printf 'hctx_cpu_map=%s\ncpus=%s\nnuma_node=%s\n' \
    "$HCTX_CPU_MAP" "$CPUS_CSV" "$NUMA_NODE" \
    >"$result_dir/mq-cpu-selection.txt"

require_block_devt "$WHOLE" "$ACTIVE_DEVT" active_namespace
WHOLE_SECTORS=$(blockdev --getsz "$WHOLE")
[[ $WHOLE_SECTORS =~ ^[1-9][0-9]*$ ]] || die "whole_size_invalid=$WHOLE_SECTORS"
[ "$WHOLE_SECTORS" -gt $((PART_START_SECTORS + PART_SIZE_SECTORS)) ] ||
    die "whole_too_small=sectors:$WHOLE_SECTORS"
PART=$(partition_node_for "$WHOLE" "$PART_NUMBER")
PART_BASE=${PART##*/}
PART_STATE=ambiguous
PART_CREATED=1
if addpart "$WHOLE" "$PART_NUMBER" "$PART_START_SECTORS" "$PART_SIZE_SECTORS"; then
    PART_STATE=created
else
    ADDPART_RC=$?
    die "addpart_failed_state_ambiguous_rc=$ADDPART_RC"
fi
udevadm settle --timeout=30
wait_for_block "$PART"
PART_STATE_RECORD=$(resolve_temporary_partition_state)
read -r PART_STATUS PART_DEVT PART_STATE_EXTRA <<<"$PART_STATE_RECORD"
[ "$PART_STATUS" = present ] && [ -z "${PART_STATE_EXTRA:-}" ] ||
    die "addpart_success_but_partition_not_exact=$PART_STATE_RECORD"
require_block_devt "$PART" "$PART_DEVT" partition
require_devt_unmounted "$PART_DEVT"
require_no_openers "$PART" partition

mkfs.ext4 -F -q -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 \
    -L EXITOS_4K_QD1 "$PART"
MOUNTED_BY_US=1
if ! mount -t ext4 -o nodiscard,noatime -- "$PART" "$mount_dir"; then
    MOUNTED_BY_US=0
    die mount_failed
fi
require_equal mounted_source "$(readlink -f -- "$PART")" \
    "$(readlink -f -- "$(findmnt -rn -o SOURCE -M "$mount_dir")")"
require_equal mounted_fstype ext4 \
    "$(findmnt -rn -o FSTYPE -M "$mount_dir")"
MOUNT_OPTIONS=$(findmnt -rn -o OPTIONS -M "$mount_dir")
require_mount_nodiscard_noatime "$MOUNT_OPTIONS"
sync -f "$mount_dir"

{
    printf 'active_time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'pci=%s\ncontroller=%s\nwhole=%s\npartition=%s\n' \
        "$PCI" "$CTRL" "$WHOLE" "$PART"
    printf 'serial=%s\nwwid=%s\ndiskseq=%s\ndevt=%s\n' \
        "$EXPECTED_SERIAL" "$EXPECTED_WWID" "$ACTIVE_DISKSEQ" "$ACTIVE_DEVT"
    printf 'driver=%s\ndriver_override=%s\n' \
        "$(driver_for "$PCI")" "$(read_override "$PCI")"
    printf 'poll_queues=%s\nio_poll=%s\nwrite_cache=%s\n' \
        "$(read_one "$NVME_POLL_PATH")" \
        "$(read_one "$SYS_ROOT/class/block/$BASE/queue/io_poll")" \
        "$(read_one "$SYS_ROOT/class/block/$BASE/queue/write_cache")"
    printf 'numa_node=%s\nselected_physical_cpus=%s\nhctx_cpu_map=%s\n' \
        "$NUMA_NODE" "$CPUS_CSV" "$HCTX_CPU_MAP"
    findmnt -rn -o SOURCE,TARGET,FSTYPE,OPTIONS -M "$mount_dir"
} >"$result_dir/active-state.txt"

verify_active_generation before-runner
env "${clean_unsets[@]}" \
    EXITOS_TRANSACTION_WHOLE="$WHOLE" \
    EXITOS_TRANSACTION_GENERIC="$TARGET_GENERIC_NODES" \
    EXITOS_TRANSACTION_PART_START_SECTORS="$PART_START_SECTORS" \
    EXITOS_TRANSACTION_PART_SIZE_SECTORS="$PART_SIZE_SECTORS" \
    EXITOS_TRANSACTION_WHOLE_SECTORS="$WHOLE_SECTORS" \
    "$RUNNER" --run \
    --mount "$mount_dir" \
    --result-dir "$MATRIX_RESULT" \
    --cpus "$CPUS_CSV" \
    --active-so "$ACTIVE_SO" \
    --transformer "$TRANSFORMER" \
    --fio "$FIO_RESOLVED" \
    --device "$PART" \
    --expect-identity "$EXPECTED_WWID" \
    --manifest-mode outer ${workload_args[@]+"${workload_args[@]}"}

[ ! -e "$MATRIX_RESULT/final-manifest.sha256" ] ||
    die inner_manifest_must_be_absent
verify_active_generation after-runner
WORK_SUCCEEDED=1
printf 'TRANSACTION_WORK_OK matrix=%s cleanup=pending\n' "$MATRIX_RESULT"
