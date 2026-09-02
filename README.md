# Exitos

Exitos is an all-in-software shortcut for file writes.

An application that appends to a file — a database writing its log, a service
writing records — pays for the file system on every write: the in-file offset
has to be translated to a device block number, the allocation state has to be
consulted, and the journal has to be updated. Applications already preallocate
their files to make that structure stable, and modern SSDs already protect
their write buffers with a supercapacitor, yet the software cost stays.

Exitos takes the stable structure that preallocation produces, holds the
offset-to-block mapping in memory, intercepts `write` and `fdatasync` on the
files the operator selects, and delivers the data to the device directly. The
file system and most of the block layer are left out of the write path.
Anything Exitos cannot handle — an unmapped range, a file it was not told
about, an operation outside its contract — falls through to the ordinary
syscall, unchanged.

Nothing here is a kernel module and nothing here patches the application. The
application binary is not modified, not recompiled and not relinked.

## What is in this repository

This is the complete implementation: both interception frontends, the shared
core, every device backend, the correctness suite, the measurement tooling, and
a runnable end-to-end example. They live together in one tree — there is no
second branch holding a different variant.

### Two frontends

Both frontends load into the application process and reach the same shared
core through the same immutable configuration object.

| frontend | how it intercepts | what it can reach |
|---|---|---|
| `libexitos_preload.so` | symbol interposition through `LD_PRELOAD` | any program whose file operations go through libc |
| `libexitos_bpftime_active.so` | rewrites `syscall` instructions in the process image and dispatches them to a user-space hook | additionally reaches programs that issue syscalls without libc: hand-written inline assembly, the Go runtime, statically linked binaries |

The instruction-rewriting frontend is the one to use when a program does not go
through libc symbols. `tests/integ/writer_rawsyscall.c` is such a program, and
the test suite demonstrates that symbol interposition cannot see it. That
frontend needs root, because its transformer maps the zero page executable and
`vm.mmap_min_addr` normally forbids that, and it needs the transformer patch in
`tools/bpftime-clone-fix.patch` before any program that creates threads will
survive. Both requirements, and the vector-state handling the hook adds, are
written up in `docs/bpftime-clone-and-xstate.md`.

### One shared core

Path selection, backend selection, strict mode and identity policy all come
from one object built once at process start. See
`docs/shared-frontend-config-architecture.md`.

* `src/intercept.c` — fd lifecycle, registration table, the takeover decision, and the fallthrough boundary.
* `src/maco.c` — the in-memory offset-to-block structure.
* `src/geom.c`, `src/extent.c` — device geometry and extent mapping.
* `src/iopath.c` — the device backends.
* `src/devguard.c` — device identity and window authorization; every check is a refusal, never a permission.
* `src/durability.c` — where the durability point is placed, given what the device reports about its write cache.
* `src/donor.c`, `src/donor_async.c`, `src/frontend_fallocate.c` — preparation for files that are not already backed by written extents, using ext4 move-extent.
* `src/frontend_config.c`, `src/frontend_admission.c` — the shared configuration object and the admission gate that keeps its lifetime safe while hooks are live.

### Five device backends

`EXITOS_IOPATH` selects one of these. The choice is made once, when the process
or context is initialized, and is fixed for the life of that context: no write
and no registration re-reads it, and the size of an individual write never
changes it.

| value | what is submitted | kernel layers still in the path |
|---|---|---|
| `pwrite` | `pwrite()` to the block device at the mapped LBA | the whole block layer; works on a loop device, so LBA arithmetic can be validated with no hardware risk |
| `nvme` | `NVME_IOCTL_IO_CMD` passthrough | needs a real NVMe controller |
| `uring` | `IORING_OP_URING_CMD` on the NVMe character device | needs Linux 5.19 or newer |
| `uringpoll` | the same command on a ring set up with `IORING_SETUP_IOPOLL` | requires the `nvme` driver to have been given poll queues at probe time |
| `uringwritepoll` | an ordinary `IORING_OP_WRITE` to the exact `O_DIRECT` block object, on an IOPOLL ring | bypasses ext4 but retains the normal block layer |

`uringpoll` and `uringwritepoll` are genuinely different paths, not two names
for one thing, and the tree keeps both so they can be compared.

### Strict mode

`EXITOS_STRICT=1` selects Exitos-S. Before every write it takes over, the
library issues a zero-length write on the intercepted descriptor through its
internal real-syscall channel, so the kernel's own per-write permission gate —
the `FMODE_WRITE` check plus `security_file_permission`, the hook that SELinux
and AppArmor evaluate per I/O — makes the decision. A refusal sends the write
down the ordinary path and the kernel reports the error exactly as it would
without the library. Strict mode also turns on the per-write descriptor
identity check. The design is in `docs/exitos-s-design.md`.

Strict mode is a policy on the same context as everything else. It is not a
third frontend and it does not carry its own backend.

## Repository layout

```
include/     public headers; each one states the contract its module keeps
src/         the implementation
tests/       the correctness suite in three tiers, plus the queue-depth device measurement
tools/       host setup, device safety, tracing and measurement drivers
attribution/ microbenchmarks that attribute cost to individual mechanisms
example/     the runnable end-to-end example
docs/        design documents
```

`tools/README.md` explains the host-setup and device-safety scripts, the
microbenchmarks the cost attribution rests on, and the environment switches
they read.

## Requirements

* Linux. `uring` and the two polled backends need 5.19 or newer.
* An ext4 filesystem on the target namespace.
* gcc, GNU make, `python3` for the summarizers, and `ripgrep` (`rg`), which five
  of the tier-0 script tests use to read back what a script emitted.
* For the fio example: fio 3.41.
* For the instruction-rewriting frontend: root, and a bpftime transformer built with `tools/bpftime-clone-fix.patch` applied.
* For the polled backends, and therefore for the end-to-end example: the `nvme` driver must have poll queues, which is a driver-probe-time setting. `tools/README.md` gives the procedure and its safety conditions.

## Build

```sh
make            # static library, both frontend DSOs, all test binaries
make attribution # the cost-attribution microbenchmarks
make examples   # tests/uring-passthrough-qd/qd_bench, the queue-depth benchmark
```

The instruction-rewriting frontend is built separately and on purpose:

```sh
make libexitos_bpftime_active.so
```

It is not part of `make` because arming it requires a transformer carrying the
clone patch, which upstream has not taken. The frontend DSO built by default
claims no syscalls.

## Tests

Three tiers, separated by what they are allowed to touch.

```sh
make unit     # tier 0: pure logic. No device, no root, no loop device.
make integ    # tier 1: loop device plus an ext4 image. Never a physical disk.
make device   # tier 2: a real NVMe namespace. Refuses to run unless EXITOS_DEV is set.
make test     # tiers 0 and 1
```

**The scratch directory has to be on an extent-based filesystem.** The tests
create their fixtures under `/tmp`, and the donor-exchange tests need
`FIEMAP` and `EXT4_IOC_MOVE_EXT` on those fixtures. On a host where `/tmp` is
tmpfs — the default on several distributions — `filefrag` reports
`FIBMAP/FIEMAP unsupported` there and every donor fixture fails to build, which
shows up as a batch of `not ok ... fixture created` lines. Check with
`stat -f -c %T /tmp`; if it says `tmpfs`, give the suite a disk-backed `/tmp`
for the duration of the run:

```sh
mkdir -p /var/tmp/exitos-scratch
unshare -m bash -c 'mount --bind /var/tmp/exitos-scratch /tmp && make unit'
```

The bind mount lives only inside that mount namespace, so nothing outside the
command sees it.

Tier 0 and tier 1 are the ones to run routinely. Every module has both
positive tests and refusal tests: a large part of the suite exists to prove
that the library declines the cases it must decline — an unmapped range, a
descriptor rebound by `dup2`, a window outside the certified region, a device
whose identity no longer matches, a write crossing two physically
discontiguous extents, a partition boundary, an arithmetic overflow. Those
boundary tests are the reason it is safe to point this at a device at all.

Tier 2 writes raw LBAs to whatever device it is given. Read `SAFETY.md` first.

`tests/uring-passthrough-qd/` is a device measurement rather than one of the
three tiers, and it is not an application example: its timed program is the
`qd_bench` binary in that directory, which `make examples` builds. It answers a
narrow question — one application thread, 4 KiB commands, and static queue
depths of 1, 2, 4 and 8, with both arms submitting on an IOPOLL ring, one
issuing `IORING_OP_WRITE` with `O_DIRECT` on an ext4 file and the other issuing
`IORING_OP_URING_CMD` against that same file's physical extents. It needs a
driver with poll queues. Its own
[`README`](tests/uring-passthrough-qd/README.md) carries the workload contract.
Nothing is recorded in this repository; a run writes everything under the
result directory the operator names.

## Runtime configuration

Exitos is inert until it is armed. With `EXITOS_FILES` unset or empty, both
frontends load, construct an inert configuration, and deliberately ignore every
other setting.

**Selecting what is intercepted**

| variable | meaning |
|---|---|
| `EXITOS_FILES` | colon-separated substrings; a file whose path contains one of them is a candidate. Setting it is what arms the library. |
| `EXITOS_DEV` | the device the operator is authorizing raw writes to. Without it, a real device is refused. |
| `EXITOS_IOPATH` | the backend, from the table above. Resolved once at initialization. |
| `EXITOS_FASTPATH` | `1` uses the selected backend for timed data and sync calls; `0` keeps preparation but sends them through ext4. Defaults to `1`. |

**Policy**

| variable | meaning |
|---|---|
| `EXITOS_STRICT` | `1` arms Exitos-S. Off by default. |
| `EXITOS_VERIFY_IDENTITY` | `1` adds a per-write `fstat` that catches a descriptor rebound underneath the registration. Off by default; strict mode implies it. |
| `EXITOS_EXPECT_SERIAL` | the namespace identity the registration path must match, compared exactly. |
| `EXITOS_WINDOW_IN_PART` | the partition an LBA window must lie inside, checked against the partition table. |
| `EXITOS_PIN`, `EXITOS_CERTIFY` | the file holding the device's certified fingerprint, and the switch that writes it. |

**Preparation for files that are not already written**

fio 3.41 defaults to `fallocate=native`, which allocates physical extents during
setup but leaves them `UNWRITTEN`; the first write to such an extent pays ext4's
conversion. To move that conversion out of the timed path without modifying the
application, set `EXITOS_PREPARE=donor-fallocate`, a private `EXITOS_DONOR_DIR`
on the same ext4 filesystem, and `EXITOS_DONOR_FILES=N`. The application's own
`fallocate(fd, 0, 0, len)` supplies the range. Donors and targets are fully
zero-written and synced, and a synchronized FIEMAP plus an atomic mapping
refresh must both succeed before `PREPARE_READY` is emitted. The lifecycle and
its failure boundary are implemented in `src/donor.c`, `src/donor_async.c` and
`src/frontend_fallocate.c`.

**Observation**

`EXITOS_STATS` names the file the counters are written to, `EXITOS_VERBOSE`
turns on the registration lines, and `EXITOS_TRACE` prints one line per claimed
syscall from inside the reentrancy guard.

Read the counters. A run that intercepted nothing and a run that helped nothing
produce the same throughput, and reading throughput alone has produced
comparisons that measured nothing at all.

`tools/README.md` documents the remaining variables, including the diagnostic
switches that exist to isolate specific failures.

## Examples

`example/` holds the runnable end-to-end example. Every runner has a read-only
`--plan` mode that prints the exact normalized command without creating a
directory, a file or any device I/O. Review that before `--run`.

### `example/fio-bpftime-e2e`

It takes an unchanged fio process, runs it plain, and then runs the same command
under Exitos with the instruction-rewriting frontend. fio is not modified, not
recompiled and not aware of the interception, so the difference is what an
application actually gets. The runner fixes both arms, rather than leaving them
to the environment:

* A, `plain-fio` — the fio command with no Exitos environment and no loader.
* B, `exitos` — the same command under the instruction-rewriting frontend, with
  `EXITOS_FASTPATH=1` and `EXITOS_IOPATH=uringpoll`. Every timed write and every
  timed `fdatasync` is submitted as an NVMe command on an io_uring ring created
  with `IORING_SETUP_IOPOLL`, so the filesystem and the ordinary block layer are
  both out of the path and completion is polled.

The rest of the workload is fixed as well: 4 KiB writes, `direct=1`,
`fdatasync=1`, `ioengine=psync`, application `iodepth=1`, worker counts 1, 2, 4
and 8, and `--repeats 2`, which places the arms A-B and then B-A.

That backend needs the `nvme` driver to have poll queues, which is a
driver-probe-time setting. `tools/nvme-4k-thread-qd1-transaction.sh` arranges it
for the workload runner it is given; running the example outside that
transaction leaves the poll queues for the operator to arrange.

The worker count is the queue depth here, and that is deliberate. The frontend
takes over `write`, `pwrite64`, `writev`, `pwritev`, `pwritev2` and `fdatasync`;
it does not take over `io_uring_enter`. An asynchronous fio engine at
`iodepth=N` would therefore leave the Exitos arm with nothing to take over, and
the summarizer would fail every cell. With a synchronous engine at application
`iodepth=1`, N workers is N writes in flight, so the worker count is the queue
depth — the only way this example produces a queue depth at all.

Inspect the normalized command first:

```sh
example/fio-bpftime-e2e/run.sh --plan \
  --mount /mnt/exitos_fs \
  --result-dir /root/exitos-artifacts/fio-bpftime-e2e \
  --cpus 0,1,2,3,4,5,6,7 \
  --active-so /absolute/path/to/this/repo/libexitos_bpftime_active.so \
  --transformer /absolute/path/to/bpftime/build/attach/text_segment_transformer/libbpftime-agent-transformer.so
```

Replace `--plan` with `--run` to execute the matrix. Everything a run produces
goes under that `--result-dir`, which has to be an absolute path that does not
exist yet and does not lie inside the mount; the example directory itself holds
no results.

`--worker-mode` selects how the workers are laid out. The two arrangements put
Exitos under different conditions, so both are worth running:

* `thread` (the default) — one fio process, `--thread=1`, `--numjobs=N`. The N
  job threads share one address space and one set of Exitos process state: one
  registration table, one set of counters, one donor pool.
* `process` — N independent fio processes, one job each (`--numjobs=1`,
  `--thread=0`), started together and pinned one per CPU, each writing its own
  file, each with a private donor directory and a donor pool one file wide.

A single fio with `--thread=0` cannot stand in for process mode: fio forks after
start, so every child would inherit the same `EXITOS_DONOR_DIR`; the donor pool
is created lazily on the first intercepted `fallocate` and its files are opened
`O_EXCL`, so the workers would collide on `donor-0000.dat` and every one but the
first would be refused.

The command above is thread mode. The process sweep has its own entry,
`example/fio-bpftime-e2e/multi-process/run.sh`, a thin wrapper that takes the
same arguments and adds `--worker-mode process` to them; it refuses the flag if
you pass it as well, and it needs a fresh `--result-dir`. Running the same
worker counts both ways separates what the workers share from what each worker
holds alone, so the summary has a `worker_mode` column and never pools a thread
cell with a process cell.

`example/fio-bpftime-e2e/summarize.py` checks the Exitos arm's counters and not
only the readback. It requires the taken-over write count to equal the timed
write count, the taken-over `fdatasync` count to equal the timed `fdatasync`
count, no write handed back to ext4, no registration refused, and at least one
mapping refresh per file. A correct readback on its own cannot tell a shortcut
that ran and was right from a shortcut that never ran; these counters are what
separate the two.

## Safety

This code writes raw LBAs to a block device. A wrong address does not raise an
error; it silently overwrites whatever lives there.

`SAFETY.md` is not optional reading before tier 2 or before any example that
touches a physical device. The short version: Linux NVMe device names are
assigned in probe order and are not stable across reboots, so a device must be
selected by its serial and namespace identity and re-verified immediately
before the write, never from an earlier certification and never from a
remembered `/dev/nvme*` name.

The device identity constants that appear in `tools/` and in the example
command blocks are placeholders — `EXAMPLESERIAL0001`,
`eui.00000000000000000000000000000000`, and an all-zero PCI DSN. They are
deliberately values that match nothing, so a copy-paste run refuses instead of
writing somewhere unintended. Substitute the identity of your own disposable
test namespace before running anything that touches hardware. For the device
entry point, `tools/nvme-4k-thread-qd1-transaction.sh`, you can supply it
without editing the file:

```sh
export EXITOS_TARGET_SERIAL=<controller serial>
export EXITOS_TARGET_WWID=<namespace WWID, e.g. eui....>
export EXITOS_TARGET_PCI_DSN=<PCI device serial number, xx-xx-xx-xx-xx-xx-xx-xx>
```

Whatever those hold is still compared byte-for-byte against the live
controller, the live namespace, the PCI function resolved by DSN, and the
`--confirm-*` arguments the operator has to repeat back from the plan output.
Supplying the identity does not weaken a single check; it only stops the
identity from being baked into the file.

## Scope

Exitos handles append and overwrite on files it was told about, and the
durability point that follows them. Shared memory mappings of an intercepted
file are outside the contract. Source transparency is not semantic invisibility:
file setup, mapping discovery, allocation timing, when `ENOSPC` becomes visible,
and the failure boundary can all differ from the ordinary path even though the
application binary is untouched.
