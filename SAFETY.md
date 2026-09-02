# STOP — read before pointing this code at any real disk

This project writes **raw LBAs straight to a block device**. A wrong address does
not raise an error; it silently overwrites whatever lives there.

## The near-miss that produced these rules

A 32 GB window on `/dev/nvme3n1` was scanned block-by-block and certified all-zero.
Between that certification and the write, **the machine rebooted**. After the reboot:

| Serial | Before | After | What it is |
|---|---|---|---|
| `EXAMPLESERIAL0002` | `nvme1n1` | **`nvme3n1`** | **the root filesystem `/`** |
| `EXAMPLESERIAL0001` | `nvme3n1` | **`nvme2n1`** | the free disk |

The controller serials in this document are placeholders: `EXAMPLESERIAL0001`,
`EXAMPLESERIAL0002` and `EXAMPLESERIAL0003` stand in for the real serials, which are not
published. The same placeholder always means the same physical disk.

**`nvme3n1` stopped meaning "the spare disk" and started meaning "the system disk."**
Writing to the remembered *name* would have destroyed a shared server's root filesystem.

**Linux NVMe names are assigned in probe order and are NOT stable across reboots.**

## It happened again, twice in one hour, and the guard caught it

The same test host rebooted twice while these measurements were running. After the second
reboot the names had moved again:

| Serial | Before | After | What it is |
|---|---|---|---|
| `EXAMPLESERIAL0001` | `nvme1n1` | **`nvme3n1`** | the test disk |
| `EXAMPLESERIAL0003` | `nvme0n1` | **`nvme1n1`** | **another user's data disk, mounted at /mnt/disk0** |
| `EXAMPLESERIAL0002` | `nvme3n1` | `nvme2n1` | the root filesystem |

So `/dev/nvme1n1` -- the name every command in the previous session used -- became someone
else's mounted filesystem. Running the benchmark against the remembered name was refused:

```
REFUSING to write: serial does NOT match the expected disk (refusing)
devguard: /dev/nvme1n1  serial=EXAMPLESERIAL0003 -> serial does NOT match the expected disk
```

**That refusal depended on a fix made earlier.** The identity check used to run
after the partitioned and mounted checks, so a disk that was partitioned or mounted returned
its own verdict and the named serial was never compared at all. On the test host that would
have returned MOUNTED and said nothing about the identity. The check now runs first, before
every other verdict, which is why it named the wrong serial instead of the wrong reason.

The pin then refused the correct disk too, for a different and equally correct reason: the
window's contents had changed since certification, because the measurements themselves wrote
there. Re-certification is a deliberate act, not an automatic one.

**A shared machine can reboot on its own**, and several times in one hour. Do not assume a
device name survives even a few minutes.

## Default non-negotiable rules

These are the default rules for media whose contents are not explicitly disposable.  The
authorization for the designated NVMe test device in the next section narrowly overrides the
content-preservation gates in rules 3 and 4 and permits a transaction-owned runtime
partition/mount only after identity proof and reprobe. It never permits an entry-time or
foreign mount, a PCI rebind while any namespace is mounted, weak identity, the system disk,
unbounded I/O, or incomplete restoration.

1. **Identity is stable metadata, never the device name.** Resolve the controller and PCI
   function by exact controller serial. The legacy variable `EXITOS_EXPECT_SERIAL` has two
   exact-match contracts: device-level devguard/benchmark calls expect that controller
   serial, but interception registration compares the namespace identity/WWID returned by
   `exitos_devguard_identity()`. A frontend runner must prove the serial-to-PCI-to-namespace
   binding externally, then pass the exact namespace identity to registration. The old
   substring comparison was unsafe and is gone. `devguard` and pin files represent a loop
   device by the synthesised identity `(loop:<name>)`. **Current interception registration
   is a narrower exception:** it deliberately skips this variable for loop-backed
   integration fixtures. Do not interpret a green loop test as proof that either production
   hardware identity gate ran. This exception is acceptable only for a test-owned image;
   arbitrary pre-existing loop devices are not certified merely because their name starts
   with `loop`.
2. **Refuse any unowned partition or mount before raw authorization or PCI rebinding.** A
   partition table normally means someone's data.
   A partition NODE is now reported as its own verdict, `DEVGUARD_IS_PARTITION`, and is
   never `OK`: it is the data that refusing the partitioned parent protects. The
   benchmark accepts one only with `EXITOS_ALLOW_PARTITION=1`, and only for the pwrite
   backend, whose offsets the kernel clamps to the partition. NVMe passthru stays
   refused there under every setting, because its LBAs are namespace-absolute.
3. **Pin the disk when you certify it** (`EXITOS_CERTIFY=1 EXITOS_PIN=<file>`), and
   re-verify the pin immediately before every write. The pin covers serial, size and a
   content hash, so it catches "the disk changed under us".
4. **Re-read the exact target window and confirm it is all zero** immediately before
   writing — not once at planning time. Waiving this needs a deliberate
   `EXITOS_REUSE_WINDOW=1`, valid only for a window you certified and wrote yourself.
5. **Never widen the window to "use up the rest of the disk."** Bounds are checked per I/O.

## Authorized experiment scope for the designated NVMe test device

The data on the dedicated NVMe test device, controller serial `EXAMPLESERIAL0001`, is
explicitly disposable for this project. It may be overwritten and its runtime test
partition/filesystem may be recreated. This authorization does not identify the disk by a
remembered `/dev/nvme*` name and does not extend to any other controller.

For this exact controller, do not require an all-zero window, a content-hash pin, or a
per-write rehash before the authorized experiment. This explicitly supersedes default rules
3 and 4 above for data preservation only. The destructive transaction must enter with no
pre-existing or foreign partition/mount and no holder/swap/md/dm user. After identity proof,
poll setup, exact-function reprobe, and serial/WWID revalidation, it may create and mount its
own runtime partition for the ext4/Exitos arms. It must unmount that partition before any
later unbind/rebind, and must remove it and restore the exact entry state before exit. Exact
identity binding, non-system-disk proof, per-I/O bounds, and final restoration remain
mandatory.

Polling may be enabled by the already documented serial-resolved PCI procedure: resolve the
controller and PCI function again, prove that function is not the root disk and has no mounted
namespace/holder/swap/md/dm user, set the poll-queue parameter, unbind only that exact
function, rebind it, resolve the serial again, and require both `poll_queues>0` and namespace
`queue/io_poll=1` before writing. Restore the original binding, override, parameter, partition,
and mount state when the transaction ends. Do not build extra data-preservation proofs for
this disposable disk in place of the correctness and performance test this scope
authorizes.

Evidence hashing is milestone-based. Do not hash every transfer, benchmark cell, or I/O.
Use transfer exit status for routine copies and, if needed, hash the frozen input set once and
the final archived result once. Hashing never belongs in the timed path.

## The two polled write paths do not have the same boundary

`EXITOS_IOPATH` is selected explicitly by the operator, snapped once when the shared
frontend context is created, and immutable for the life of that context. LD_PRELOAD,
opt-in active bpftime, and Exitos-S all consume that same context. There is no per-write or
per-size backend switch: an explicit `uringwritepoll` process remains on that backend, and an
explicit `uringpoll` process remains on passthrough. Unknown/empty explicit modes fail closed.

`EXITOS_IOPATH=uringpoll` opens the NVMe generic-character device paired with the
filesystem's namespace and submits `IORING_OP_URING_CMD`. It bypasses both ext4 and the
normal block layer. `EXITOS_IOPATH=uringwritepoll` opens the exact block object identified by
the intercepted file's `st_dev` and submits ordinary `IORING_OP_WRITE` on an IOPOLL ring. It
bypasses ext4 but retains the normal block layer.

If an experiment specifically wants to avoid the multi-segment passthrough command shape for
a physically scattered 8 KiB payload below 32 KiB, it can use physically contiguous memory
or explicitly choose `uringwritepoll`. Multi-segment user passthrough can force an SGL
descriptor path, whose construction and device-side descriptor fetch can increase latency.
The block path lets the kernel's ordinary block/NVMe mapping choose the command shape.
Fixed-buffer registration does not prove that the registered pages are physically
contiguous.

The block path is not permission to weaken any raw-write guard. It must retain the exact
partition or whole-device open file description, convert absolute Maco LBAs into that
object's relative byte range, and reject any underflow, overrun, identity drift, missing
poll capability, or loss of direct I/O. An explicit `uringwritepoll` request fails closed;
it never silently changes into passthrough, interrupt-driven, or buffered I/O.

## Also learned the hard way

An "only checks the gate" invocation actually wrote ~512 KB, because an argument-clamp
turned `iters=0` into `iters=2000`. **A safety tool that silently substitutes a default
for "do nothing" is a hazard.** `iters=0` now means zero writes, and bad input is an error.

## Frontend contract: this is not transparent POSIX interposition

Registration accepts only a regular fd whose `F_GETFL` succeeds and which is
writable, non-`O_APPEND`, and neither `O_SYNC` nor `O_DSYNC`. This
is an authority boundary: the raw backend has its own writable device handle,
so accepting an `O_RDONLY` application fd would otherwise turn a required
`EBADF` into a successful device write. A buffered fd enters the separate
registration-settlement/kernel-debt/page-invalidation machinery; the hardware
performance results reported for this project use `O_DIRECT` and do not
validate buffered-mode performance. Append and per-write-sync modes remain
outside the fast-path contract.

The supported use is an **exclusive direct-I/O epoch**. Before any of the
following, stop and join all writers, call a real sync, pay Éxitos raw debt, and
unregister the fd:

- `fork()` or passing/receiving the open file description with `SCM_RIGHTS`;
- io_uring/AIO writes, writable `mmap`, or any buffered alias of the inode;
- copy/offload and write-like calls not explicitly covered by the active
  frontend (for example a newly added libc/syscall ABI);
- changing ownership of the file description across a subsystem that can write
  without using the interposed entry points.

An O_DIRECT writer does not make a dirty writable mapping safe. A dirty page
can be written back after a raw write and overwrite the newer device bytes; a
clean cached page can continue returning stale bytes. Readers and writers must
therefore stay within the exclusive direct-I/O epoch, not merely add
`O_DIRECT` to one alias.

`F_SETFL` is transactional: existing raw debt is paid before the real call; a
successful flag change disables the fast path, while a failed call restores the
old debt state. `close_range()` and `freopen()` cannot be represented safely as
one-fd table edits, so they first pay every registration's raw debt and then
permanently poison takeover in that process. This also applies to
`CLOSE_RANGE_CLOEXEC`: the close is deferred to a future `exec`, but the old
mapping must not survive as usable fast-path state.

The preload frontend guards the direct image-boundary ABIs `execve()`,
`execveat()`, and `fexecve()`: it pays all raw debt before calling libc and
permanently poisons takeover. A FLUSH failure prevents the real exec and remains
retryable; a real exec failure preserves libc's errno but does not unpoison the
old snapshot. Glibc convenience entry points (`execv`, `execvp`, `execvpe`, the
`execl*` family), a raw `SYS_execve*`, and runtime-specific exec paths are not
assumed to route through these exported symbols. Before using one of those,
quiesce writers, perform a reportable sync/unregister, and only then exec.

`exitos_ctx_destroy()` attempts to flush every completed raw debt before it
releases the retained iopaths, but it is a `void` API and cannot report a device
FLUSH failure. It is therefore cleanup, not a durability acknowledgement.
Callers that need a success/failure result must sync or unregister every fd
before destroy/dlclose. Ordinary process exit without a successful sync has the
same deliberately weak durability contract as ordinary unsynced writes.

`ftruncate()` and `fallocate()`/`fallocate64()` are extent-lifecycle
transactions. They first pay completed raw debt; a FLUSH failure means the real
metadata syscall is **not called** and the mapping/debt remain retryable. After
successful preflight, the cached mapping is invalidated before the filesystem
may release, move, zero, allocate, or unshare blocks. A submitted syscall that
later fails still leaves the mapping conservatively invalidated. The dormant
bpftime dispatcher has unit coverage for the same `SYS_ftruncate` and
`SYS_fallocate` ordering, but it is not an enabled production frontend.

The normal `libexitos_bpftime.so` artifact remains **hard-frozen**: its claim
table is empty and startup returns `-EOPNOTSUPP` before process-wide text
rewriting. A separate, explicitly built `libexitos_bpftime_active.so` now uses
the shared static configuration and an XSAVE/XRSTOR-aware shim. It is opt-in,
requires root plus the transformer carrying `tools/bpftime-clone-fix.patch`,
and is not silently substituted for the frozen artifact. Reach for LD_PRELOAD
first: wherever libc-symbol interposition can see the application's writes, it
carries none of that extra risk.

All three frontends borrow that shared configuration through the same sharded
admission gate. Startup freezes the active-bpftime control flags before hook
publication; teardown first closes admission, waits for admitted calls to leave,
and only then destroys the context. No frontend may cache a context pointer and
use it outside an admitted interval, and no write may reread `EXITOS_IOPATH` or
select a backend from the request size.

Optimized glibc applications may call `__open_2`, `__open64_2`, `__openat_2`,
or `__openat64_2`; the preload frontend covers those fixed-arity ABIs and
forwards their missing-mode checks to libc unchanged. Linux `openat2` is not an
automatic registration point in either frontend. A file opened that way stays
on the kernel path unless it is explicitly registered; adding any new open or
write ABI requires the quiesce/sync/unregister protocol above before relying on
the fast path.

A pre-close raw `FLUSH` failure has deliberately unusual but explicit
semantics: Éxitos returns the error **without calling the real `close`**, so the
fd remains open and the debt remains retryable. Once real Linux `close` is
called, late `EINTR`/`EIO` does not restore the registration because Linux may
already have released and reused the number.

Signal-style reentrant write/open/close/dup mutations atomically poison the
context; future writes use the kernel path, while a later ordinary sync still
pays any existing raw debt first. A reentrant `fdatasync`/`fsync` cannot safely
pay debt owned by the interrupted raw transaction, so that invocation fails
closed with `EIO` rather than returning false durability. Registration-time
sysfs/device opens and closes, and iopath raw `pwrite`/`fdatasync`, use explicit
direct calls to already resolved libc functions (or the dormant backend's saved
original syscall handler). They do not traverse an interposed wrapper, so there
is no consumable "next fd is internal" token or exact-fd enter-to-wrapper window
for a signal handler to steal. A user mutation that actually re-enters a
wrapper is always treated as asynchronous reentry and poisons the context.
