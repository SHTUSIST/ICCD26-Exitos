# tools/ and attribution/ — what each thing is for

Nothing here is scratch. Every script was written during development, and each
one is kept because throwing it away once meant re-deriving it under pressure,
or because it catches a mistake that was actually made.

## Transfer and hashing rule

Do not compute SHA-256 for every file transfer or every benchmark cell. A source or binary
may be hashed once when it actually changes and is frozen at a milestone, and final artifacts
may be hashed once when the complete result is archived. Routine `scp`/`rsync`, retransfers of
unchanged files, and individual runs use the transfer command's exit status; if an integrity
check is needed, perform one check after the whole transfer batch. Hashing must never be in
the timed I/O path.

`fio-scale-pilot.sh` follows this rule in code. It validates and hashes the source snapshot,
variant DSOs, and cpufreq attestation once when the campaign milestone is frozen. Cell gates
do not rehash those files: they compare frozen device/inode/mode/size/time identities, bind the
already validated object fd before `exec`, and compare workset, poll-state, and summary records
field-for-field. The recorded milestone SHA remains an identifier; it is not recomputed per
cell.

## tools/

| script | what it answers | why it exists |
|---|---|---|
| `disk-safety.sh map` | which device name currently holds which serial | kernel NVMe names are assigned in probe order and change across reboots; a reboot once moved a spare disk onto the name the root filesystem then took |
| `disk-safety.sh scan DEV MB COUNT` | is this window really all zero, right now | a window certified earlier is not certified now — a benchmark, another user or a reboot may have written to it since |
| `disk-safety.sh holders DEV` | signatures, partitions, mounts, holders, dm/md/LVM | a disk that looks free can still be in use; refuse anything partitioned or mounted |
| `disk-safety.sh watch DEV` | is anything writing to it at this moment | distinguishes "unused" from "idle for the moment" |
| `enable-nvme-polling.sh --explain/--plan` | explain the retired polling experiment and print a read-only maintenance-window plan | the old runtime reprobe path caused a journal abort/name change and once reported that the device did not return; all run/restore modes are now hard-frozen before host discovery |

## attribution/

| program | what it measures | caveat to respect |
|---|---|---|
| `minperf.c` | per-operation latency across every backend, plus the syscall floor, the intercept decision cost, and batched writes-then-flush versus per-write FUA | refuses to touch a device without an explicit serial, a pin, and a re-verified zero window |
| `fsvsraw.sh` | filesystem versus raw LBA on the same physical device | both sides must read real data: reading never-written blocks on an SSD is answered from the mapping table without touching media and looks impossibly fast |
| `probe.c` | why the fast path is not engaging, step by step | written because a run reported success while the fast path had never executed once |

## Measurement rules these encode

1. **A correct checksum is not proof of interception.** Read the counters.
2. **Both sides of a comparison must be in the same state** — same device, same
   file state, same durability setting, same data actually present on media.
3. **Preparation is not part of the write.** Space preparation happens ahead of
   demand, off the critical path, so timing it together with the append that
   follows charges preparation to the write and understates the mechanism. Warm
   the region first and time steady state, or report preparation separately.
4. **Re-verify immediately before writing**, never from an earlier certification.

## `fio-compare.sh` — the same unmodified fio, with and without interception

Runs fio 3.41 twice on one filesystem: once plain, once under `LD_PRELOAD`, and prints
both results plus the interception counters. fio is not modified, not recompiled and not
aware of the interception, so the difference is what an application would actually get.

The counters matter as much as the IOPS. A run that intercepted nothing and a run that
helped nothing produce the same IOPS, and reading the IOPS alone once produced a
comparison that measured nothing at all.

The three patterns behave completely differently and the pattern is the first thing to
state about any number from this script: `overwrite` (a preallocated, already-written
file), `unwritten` (fallocated and never written), `append` (a file that grows).

## Environment switches, and which ones are diagnostics

The module and the benchmarks read a fair number of environment variables. They fall into three
groups, and the third group exists because of specific measurements that went wrong.

**Choosing what runs.** `EXITOS_FILES` names the files to intercept and arms the module at all.
`EXITOS_DEV` names the device the operator is authorising raw writes to; without it a real
device is refused. `EXITOS_IOPATH` is the operator's explicit backend selection, and accepts
`pwrite`, `nvme`, `uring`, `uringpoll` and `uringwritepoll`.

**The choice is static, not per-I/O adaptive.** Both `libexitos_preload.so` and the opt-in
`libexitos_bpftime_active.so` call the same `exitos_frontend_config_from_env()` loader. It
copies `EXITOS_FILES`, validates the policy flags, and constructs one context whose backend
choice is immutable. Every later fd registration uses that snapshot; no write and no
registration re-reads `EXITOS_IOPATH`, and the write length is never used to switch modes.
Changing the canonical mode implementation and rebuilding changes both frontends together.
`EXITOS_STRICT=1` (Exitos-S) is an orthogonal policy on the same context and cannot override
its I/O mode. The detailed ownership/API contract is in
`docs/shared-frontend-config-architecture.md`.

**Setup-time native-fallocate takeover.**  fio 3.41 defaults to
`fallocate=native`, not `none`.  To move ext4's first-write UNWRITTEN conversion
out of the timed path without changing fio, set
`EXITOS_PREPARE=donor-fallocate`, a private same-ext4
`EXITOS_DONOR_DIR`, and canonical `EXITOS_DONOR_FILES=N`.  The application's
own `fallocate(fd,0,0,len)` supplies the range.  Donors and targets are fully
zero-written and synced, then synchronized FIEMAP and an atomic mapping refresh
must succeed before `PREPARE_READY` is emitted.

`EXITOS_FASTPATH` decides what the timed calls do once that preparation is in
place, and both values are useful for different reasons.  With
`EXITOS_FASTPATH=0` the donor-fallocate preparation still runs, but every timed
write and fdatasync is handed back to ext4; the takeover counters stay at zero
and the run measures the preparation work alone, which is what makes it a
control rather than a result.  With `EXITOS_FASTPATH=1` the timed writes and
fdatasyncs are taken over and issued through the backend already named by
`EXITOS_IOPATH`, so ext4 is not on the timed path.  The value does not choose a
backend and cannot override `EXITOS_IOPATH`; it only decides whether the
selected backend is used for the timed calls.  The shipped fio example,
`example/fio-bpftime-e2e/run.sh`, sets `EXITOS_FASTPATH=1` on its Exitos arm,
so that arm's timed writes and syncs go through the selected backend instead of
returning to ext4.

There is no `EXITOS_PREPARE_BYTES` in active code and no per-I/O adaptation.
The exact lifecycle and failure boundary are implemented in `src/donor.c`,
`src/donor_async.c` and `src/frontend_fallocate.c`.

Both adapters hold a shared sharded admission token whenever they can borrow the config/context.
Shutdown closes and drains that gate before destroying either object. Active bpftime freezes
all hook-visible diagnostic policy before enabling admission and publishing the hook; no hook
entry can race later initialization of those ordinary variables.

`uringpoll` and `uringwritepoll` are deliberately different paths:

| value | submitted object and operation | kernel layers retained |
|---|---|---|
| `uringpoll` | paired `/dev/ng*` fd with `IORING_OP_URING_CMD` | bypasses the filesystem and the normal block layer |
| `uringwritepoll` | exact partition or whole-block fd with ordinary `IORING_OP_WRITE` and `IORING_SETUP_IOPOLL` | bypasses ext4 but retains the normal block layer |

Each of the two is requested by name, and `uringwritepoll` is never a silent fallback. If the
exact block object, direct-I/O mode, native block range, or polling capability cannot be
proved, an explicit `uringwritepoll` request is refused.

When an experiment specifically wants to avoid the multi-segment passthrough command shape
for a physically scattered 8 KiB payload below 32 KiB, it can supply physically contiguous
memory or explicitly select `uringwritepoll`. A multi-segment user-passthrough buffer can
force an NVMe SGL descriptor path; constructing the descriptor and fetching it at the device
can increase latency. Ordinary block I/O lets the block/NVMe stack choose its normal mapping
instead. This is a mechanism to test, not a stable performance-selection rule. Fixed-buffer
registration can remove repeated buffer-import work, but it does not guarantee physical
contiguity.

`example/fio-bpftime-e2e/run.sh` is the recommended end-to-end test.  It runs
an unchanged fio 3.41 process: arm A is plain fio, arm B is the same fio under
Exitos with the same geometry, and the default frontend is active bpftime.  The
runner fixes `EXITOS_FASTPATH=1` and `EXITOS_IOPATH=uringpoll` on arm B, so that
arm needs the poll queues the section below explains how to enable.  The cell
geometry is `direct=1`, `fdatasync=1`, `ioengine=psync` and application
`iodepth=1`, with `--bs` defaulting to 4 KiB.  The engine is synchronous, so the
worker count is the number of writes in flight and therefore the queue depth.
`--worker-mode thread`, the default, runs one fio process with N job threads
that share one address space and one set of Exitos process state;
`--worker-mode process` runs N independent fio processes, one job each, which
share neither.  `example/fio-bpftime-e2e/multi-process/run.sh` is a thin wrapper
on the same runner with `--worker-mode process` fixed; it refuses the flag
rather than let it be overridden.

```sh
example/fio-bpftime-e2e/run.sh --run \
  --mount /mnt/exitos_fs \
  --result-dir /abs/path/to/results \
  --cpus 0,1,2,3,4,5,6,7 \
  --active-so /abs/path/to/this/repo/libexitos_bpftime_active.so \
  --transformer /abs/path/to/bpftime/build/attach/text_segment_transformer/libbpftime-agent-transformer.so
```

`--plan` inspects the normalized fio command without creating any files.
`--result-dir` is required, must be absolute, must not already exist and must
lie outside the mount; everything the run produces is written there and nothing
is written back into the repository.

`tests/uring-passthrough-qd/` answers a narrower question: one application
thread submitting static batches at QD 1/2/4/8, comparing the direct
`IORING_OP_URING_CMD` IOPOLL path against the matched ext4 `IORING_OP_WRITE`
IOPOLL path.  It is narrower because it drives the ring itself rather than
running an unmodified application, and it reads `queue/io_poll` on the target
and refuses the cell if polling is off.  See
`tests/uring-passthrough-qd/README.md`.

Set `EXITOS_IOPATH` explicitly. It is resolved once, when the process or context is
initialized, and is fixed for the life of that context. Leaving it unset selects a legacy
ordered fallback that probes device capability at each fd registration; that path exists for
compatibility and is not a configuration to measure against, because which backend a given
registration ends up on is then a property of the device rather than of the run. An explicit
`EXITOS_IOPATH` is never overridden: if the capability its backend needs is absent,
registration is refused rather than quietly moved somewhere else.
Enabling poll queues on a host is a driver-probe-time setting (`nvme.poll_queues=N` on the
kernel command line). Setting the module parameter at run time is not enough on its own: the
blk-mq tag set fixes its map count when the controller is first probed, so a controller reset
picks the parameter up but still reports `queue/io_poll` 0. A PCI unbind/rebind of that one
function does force a fresh probe, and that is the procedure
`enable-nvme-polling.sh` was frozen for after it aborted a mounted ext4 journal and once lost a
device.

> **Never unbind/rebind a PCI function while any namespace on it is mounted.** That is the
> direct cause of the historical incident: the ext4 journal on the mounted filesystem was
> aborted, and on another occasion the device did not come back. Unmount first, and verify
> the target has no mounted namespace, no holders, no device-mapper or md reference and no
> swap before touching `/sys/bus/pci/drivers/nvme/unbind`.

The procedure that worked on the test host, with polling confirmed afterwards
(`queue/io_poll` 1):

1. Derive the PCI address **from the device node, by serial** — do not reuse a remembered
   address. Controller names move across reboots; on that host the same disk was `nvme3n1`
   before a reboot and `nvme2n1` after, and a stale address would have targeted a different
   disk. `basename $(readlink -f /sys/class/nvme/<ctrl>/device)` gives the address for the
   controller whose `serial` matches.
2. Confirm the target carries no mounted filesystem, while the other controllers do. On that
   host the root filesystem was on a different PCI function entirely.
3. Confirm `/sys/block/<dev>/holders/` is empty, the device is absent from `dmsetup deps`,
   `/proc/mdstat` and `/proc/swaps`.
4. `echo N > /sys/module/nvme/parameters/poll_queues`, then unbind and rebind that one
   address, then re-identify the disk **by serial again** and check `queue/io_poll`.

A persistent alternative that avoids unbinding a live device: `options nvme poll_queues=N` in
`/etc/modprobe.d/`, which takes effect at the next boot's first probe. It applies to every
NVMe controller on the host, so on a shared machine it is a change to make deliberately.
`modprobe -r nvme` is not an option when any root or system filesystem is on NVMe.

`EXITOS_STATS` names the file the counters are written to. `EXITOS_VERBOSE` turns
on the registration lines. `EXITOS_STAGE=1` (default off) keeps the existing bounce/fixed
optimization available on the `nvme`/`uring` passthrough backends: a write of more than 4 KiB
and at most 32 KiB is first copied into a 2 MiB-backed physically contiguous slot so the
passthrough command can avoid an external SGL descriptor list, and on `uring` the slot is
registered once so per-command page import is avoided. The slot is armed only when its
contiguity is proven (pagemap run count 1, or a successful `MADV_COLLAPSE` when pagemap is
unreadable); otherwise writes go out exactly as before. The copy is part of the path and must
be evaluated end to end; registration by itself does not make caller pages contiguous.
`EXITOS_STRICT=1` (default off) arms the strict mode the paper calls Exitos-S
(`docs/exitos-s-design.md`): before every taken-over write the library issues a
zero-length pwrite on the intercepted fd through the internal
real-syscall channel, so the kernel's own per-write permission gate — the `FMODE_WRITE`
check plus `security_file_permission`, the hook SELinux/AppArmor evaluate per I/O — decides;
any refusal sends the write down the ordinary path and the kernel reports the error exactly
as it would without the library. Strict mode also turns on the per-write fd identity check.
fdatasync is never probed (the vanilla fdatasync path runs no LSM file hook).

**Buffered registrations.** Registration no longer requires O_DIRECT
(the paper never conditions interception on it). A buffered fd opts into the coherence
machinery: registration settles pre-existing state with one real fdatasync plus a whole-file
`POSIX_FADV_DONTNEED`; takeover requires no outstanding kernel-side write debt (a dirty page
written back later would clobber raw data); every raw-written range is invalidated, widened
to full pages, through an asynchronous queue with an inline fallback. Shared-memory mappings
(`mmap`) of the log file stay outside the support contract.

**qdsplit measurement arms for the same question.** Arms 15-17 (`pt-8k-fixedbuf`,
`pt-8k-bounce`, `pt-8k-bounce-fixed`) A/B the two candidate mechanisms separately:
registered fixed buffer only, bounce copy only, and both. `EXITOS_PAIRED=opt` interleaves
`fs-8k-qd1`, `pt-8k-qd1` and the three new arms in one run. Bounce arms charge their staging
memcpy inside the timed window and refuse the whole run if the staging slot cannot be proven
physically contiguous before the first sample. Historical arm 1 (`pt-8k-qd1`) and arms 15-17
use the interrupt-completion qdsplit ring; they are diagnostics, not the production polling
control. The production IOPOLL comparison is explicitly opt-in with
`EXITOS_PAIRED=blockpoll`, which balances `fs-8k-qd1`, `prod-pt-8k-iopoll` and
`prod-blk-8k-iopoll` within the selected three-arm order. The latter two call the production
`iopath` QD1 backends with exclusive handles. This contract is exactly 8 KiB and refuses a
different `EXITOS_BIGSZ`. It also runs only after the production durability probe proves
write-through (`volatile_write_cache=0`) and selects `EXITOS_DUR_NONE`; probe failure,
`EXITOS_DUR_FUA`, or `EXITOS_DUR_FLUSH` refuses the matrix before file initialization.
The probe basename is derived from the retained whole-block fd, never from the mutable
`EXITOS_DEV` path. Before and after the probe qdsplit uses the production fd-bound poll
resolver to match the retained fd's `BLKGETDISKSEQ` to the sysfs parent's `diskseq` while
also binding rdev, the full `/sys/dev/block/<major>:<minor>` target, namespace identity,
whole-device status and LBS; any generation or sysfs-target drift fails closed. The campaign
layer separately freezes generation and records that outer evidence.
POST_RUN_VERIFY/readback proves byte correctness, not durability, and cannot substitute for
that setup proof. With no selector, qdsplit still runs only the
historical arm 0-17 default set; the production arms are never added implicitly.

**Safety, all of them refusals rather than permissions.** Never select a disk by a remembered
`/dev/nvme*` name. The legacy variable `EXITOS_EXPECT_SERIAL` unfortunately has two exact-match
contracts: device-level devguard/benchmark entry points expect the controller serial, while
the interception registration path expects the namespace-stable identity/WWID returned by
`exitos_devguard_identity()` (for the NVMe test device, the `eui...` value). Frontend hardware
runners must independently prove the controller-serial-to-PCI-to-namespace binding before
passing that WWID. `EXITOS_WINDOW_IN_PART` names the partition an LBA window
must lie inside, checked against the partition table. `EXITOS_PIN` is the file holding the
disk's certified fingerprint; `EXITOS_CERTIFY` writes it. `EXITOS_REUSE_WINDOW` waives the
all-zero check, and only for a window the operator certified and wrote themselves.
`EXITOS_ALLOW_PARTITION` lets the benchmark work on a partition node with the pwrite backend.
`EXITOS_VERIFY_IDENTITY` turns on the per-write fstat that catches an fd rebound by dup2.

**Diagnostics, added while chasing specific failures.** `EXITOS_BPFTIME_NOHOOK` performs the
instruction rewriting but leaves bpftime's own hook in place -- the only honest control for
"does the rewriting itself break this program", because loading the transformer alone rewrites
nothing unless `AGENT_SO` is set. `EXITOS_BPFTIME_NOCLAIM` leaves the claim table empty so the
shim forwards every syscall with a plain jump and builds no frame. `EXITOS_BPFTIME_NOOP` takes
values 1, 2 and 3 for progressively more of the hook body, which is how the crash was bisected
to a single call. `EXITOS_TRACE` prints one line per claimed syscall, from inside the
reentrancy guard -- printing before that guard recurses until the stack is gone.
`EXITOS_ONLY` restricts the benchmark to one historical arm so that diagnostic arms can run in
their own process. The two production IOPOLL arms are selected only by the balanced
`EXITOS_PAIRED=blockpoll` contract.

## The bpftime backend (backend 2): syscall rewriting must be enabled

The only reason this backend exists is to catch raw syscalls — the LD_PRELOAD frontend cannot
see a write issued from inline assembly. Therefore **using the bpftime backend at all means
taking the path with syscall rewriting enabled**, equivalent to the bpftime CLI's
`--enable-syscall-trace`; there is no configuration in which it should be turned off. Without
that switch bpftime only takes frida's uprobe path and catches not one syscall.

There are two ways to invoke it. The official path (the same usage as the FUSE benchmark in
bpftime's own README):

```bash
AGENT_SO=<bpftime>/build/runtime/agent/libbpftime-agent.so \
LD_PRELOAD=<bpftime>/build/attach/text_segment_transformer/libbpftime-agent-transformer.so \
<program>
```

Or this project's "route A": dlopen the transformer, take `_frida_cs_arch_register_x86` and
`_ZN7bpftime20setup_syscall_tracerEv`, **register Capstone's x86 architecture before calling
`setup_syscall_tracer()`**, then install your own hook with
`_ZN7bpftime13set_call_hookEPFllllllllE`. Capstone's symbols carry the `_frida_` prefix (a
consequence of frida's packaging); dlsym with the unprefixed name returns a null pointer, which
shows up as `Failed to open capstone instance: CS_ERR_ARCH`.

Two hard prerequisites, both required:

1. **Run as root.** The transformer has to map the page at address 0 executable, and
   `vm.mmap_min_addr` defaults to 65536 on distributions (65536 on the test host as well); only
   CAP_SYS_RAWIO can bypass it.
2. **The transformer must have `tools/bpftime-clone-fix.patch` applied.** Without this patch,
   upstream makes the clone/clone3 child execute a `ret` off its own brand-new stack, and any
   program that creates threads crashes — measured on the test host as a SIGSEGV with rip
   landing in libc's data segment. The patch is 18 lines; the background and the verification
   are in `docs/bpftime-clone-and-xstate.md`.

## Choosing between the two frontends: reach for LD_PRELOAD first, use bpftime only where it cannot intercept

**Reach for `libexitos_preload.so` (LD_PRELOAD symbol interposition) first.** Where it can
intercept, its interception is exactly the same as bpftime's — measured on the test host, both
frontends gave the same counters for the same unmodified fio,
`fast_write=16384 fast_sync=16383 pass=0`, with not one fallback.

**Use the bpftime backend only where LD_PRELOAD structurally cannot intercept.** That means
programs that issue syscalls without going through libc symbols: ones that write their own
inline assembly (this repository's `tests/integ/writer_rawsyscall.c` is exactly that, LD_PRELOAD
has no effect on it at all, and there is a test that proves it), the Go runtime, statically
linked binaries. bpftime rewrites the `syscall` instructions in the process image, which lets it
catch these.

**The cost is that it brings a whole extra risk surface.** All three of the following were
verified on real hardware: first, it requires running as root (it has to map the zero page, and
`vm.mmap_min_addr` defaults to 65536 on distributions); second, upstream's trampoline makes the
clone/clone3 child execute a `ret` off its own new stack, so any program that creates threads
crashes, and `tools/bpftime-clone-fix.patch` must be applied first; third, it does not save
vector state, so the moment a hook body uses a vector register it silently destroys the caller's
upper YMM bits — our own shim has fixed this with XSAVE/XRSTOR, but upstream has not.
bpftime's role is frontend interception that covers raw syscalls; it is not an I/O backend.

In one line: **reach for LD_PRELOAD first, and use bpftime as the remedy for what LD_PRELOAD
cannot intercept.**
