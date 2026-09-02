# Exitos-S: strict mode as a zero-length write probe

Exitos has two modes. Fast mode hands a qualifying write straight to the device
and never enters the kernel's file path. Strict mode, Exitos-S, runs the
kernel's own per-write permission gate before every takeover. It is selected by
`EXITOS_STRICT=1`, is off by default, and when it is off the write path executes
not one extra instruction.

This document states where Linux checks permissions, what a kernel-bypassing
write path therefore gives up, why strict mode is built as a zero-length write
probe rather than as a user-space attribute query, and what the implementation
and its tests cover.

## 1. Where Linux checks permissions

The four points below are the kernel behavior the design has to reproduce. They
were read from the v6.18-rc5 sources, with the file and line ranges given so
they can be checked again.

**Discretionary access control is checked at open, not per write.** The ordinary
write path `vfs_write` (`fs/read_write.c:666-696`) does exactly two things about
permission: it checks the `FMODE_WRITE` bit in `f_mode` — a bit fixed at open
time, so a later `chmod` that turns the file read-only does not affect an
already-open descriptor — and then it enters `rw_verify_area`. The kernel never
re-checks the permission bits for an already-open descriptor.

**Mandatory access control is checked on every operation, and it sits directly
on the ordinary write path.** `rw_verify_area` (`fs/read_write.c:451-478`) calls
`security_file_permission(file, mask)` on every read and write. SELinux attaches
`selinux_file_permission` to this hook (`security/selinux/hooks.c:3774-3792`),
and its logic is: if the security label of the calling process, the security
label of the file inode, and the policy sequence number have all been unchanged
since open, permit immediately from one cached comparison; if any one of them
changed, redo the full decision. AppArmor attaches to the same hook
(`security/apparmor/lsm.c:546,1696`). On a host running SELinux or AppArmor,
every ordinary write goes through the security module, and once the policy is
tightened the next write gets `EACCES`.

**That per-operation hook is exactly what a kernel-bypassing write path gives
up.** Fast mode hands the write to the device driver and
`security_file_permission` never executes. On a host that uses discretionary
control only, nothing is lost, because the kernel does not re-check either. On a
host with a security module enabled, the guarantee fast mode gives is weaker
than the unmodified path: after the policy is tightened, fast mode keeps issuing
raw writes. Strict mode exists to close that gap back to parity with the
unmodified path.

**`fdatasync` runs no security-module file hook on the ordinary path.**
`security/security.c` contains no fsync-related hook anywhere in the file. So
strict mode probes writes only and deliberately does not probe `fdatasync`:
adding a permission check there would be stricter than the kernel path and could
make a legitimate program fail.

## 2. Why not a user-space attribute check

The obvious alternative is to read the file's inode attributes before every
operation — through an `ioctl` or an `fstat` — and refuse the takeover when they
have changed. That does not do the job.

Inode attributes (owner, permission bits, and metadata of that kind) are on the
discretionary side. A mandatory-access decision rests on the security labels
plus the current policy state, which exist only inside the kernel's security
module; no user-space attribute query reaches them. Such a check costs more than
the probe — one `ioctl` plus parsing — and still cannot detect a tightened
SELinux policy: after the policy is tightened, a mode built this way keeps
issuing raw writes, no different from fast mode.

Rebuilding the decision in user space from eBPF also does not work. No eBPF
helper asks the security module for a decision on an arbitrary file, and the
kernel's BPF LSM is an attach point for a security module to hang policy on, not
a query interface a bypassing writer can call. Whatever user space rebuilds is
necessarily behind the kernel's policy state.

Detecting a `chmod` is a different thing from a permission check, and it is not
on by default: the kernel ignores `chmod` for an already-open descriptor, so
refusing on it would be stricter than the path being replaced. The default
follows semantic equivalence with the ordinary path.

## 3. The design: one zero-length write per takeover

**What it does.** With strict mode on, every write that is about to be taken
over first issues, before the raw device command goes out, a `pwrite` of length
0 on the intercepted file descriptor, at this write's own offset, through the
internal real-syscall channel `exitos_internal_pwrite_call`. Both frontends wire
that channel to the real call, so the library does not intercept its own probe:
`src/preload.c` resolves the real libc `pwrite`, and `src/bpftime_hook.c` issues
`SYS_pwrite64` through the original syscall handler.

Only a return of 0 continues down the shortcut. On any error no raw write is
issued, the whole operation falls back to the ordinary path, and the kernel
refuses it there with its own decision — the error code the application sees,
and when it sees it, are the same as with the library not installed.

Strict mode also turns on the per-write descriptor identity check
(`EXITOS_VERIFY_IDENTITY`, otherwise an independent bit): one `fstat` per write,
and if the descriptor has been rebound to another file by `dup2` or
`fcntl(F_DUPFD)`, the registration is dropped and the write falls back to the
ordinary path. The probe answers "may this write still be made"; the identity
check answers "is this still the file that was registered". Neither covers the
other.

**Why zero length is enough.** Inside the kernel a zero-length write reaches
exactly the permission gates and nothing past them: `vfs_write` checks
`FMODE_WRITE`, so a descriptor that is no longer writable is caught;
`rw_verify_area` calls `security_file_permission`, so SELinux or AppArmor redoes
its decision as soon as a label or the policy changes; and the filesystem then
sees a length of 0 and returns straight away, touching no data, writing no
journal, updating no timestamp. This was observed on a test host running kernel
5.15 on ext4: the call returns 0 with mtime, ctime and file size unchanged, and
returns `EBADF` on a read-only descriptor.

**What it costs.** One system call per taken-over write, plus the security
module's decision, which is a single cached comparison when neither the labels
nor the policy have changed since open. The added per-write cost has not been
measured on hardware.

**Where the design comes from.** The kernel's own answer to "check permission on
every operation" is one `security_file_permission` per write. The probe borrows
that answer instead of rebuilding one in user space. Designs that bypass the
kernel entirely, such as SPDK, give up file-level permission and shrink the
boundary to who may open the device; that is the situation Exitos fast mode is
in on a host with a security module, and strict mode is what buys the kernel's
semantics back when they are wanted.

**Scope, and what is not claimed.** What the probe restores is per-operation
permission semantics equivalent to the ordinary kernel path, on the condition
that the application issues its writes through the Exitos interception layer. It
is not an adversarial defense: a process that can send passthrough commands to
the NVMe device node can already write anywhere on that device, file-level
permission has stopped applying at that level, and the real isolation boundary
is the access permission on the device node itself. That boundary is shared by
every device-passthrough design and is not introduced by this one. `fdatasync`
gets no probe, for the reason in Section 1. A failed probe always falls back to
the ordinary path rather than inventing an error code of its own, so Exitos and
the kernel cannot report different errors for the same situation.

## 4. Implementation

`src/intercept.c` carries the mode. `exitos_ctx_strict(ctx, on)` sets the
context's `strict` bit under the table write lock and, when arming, also sets
`verify_identity`. The probe sits in the write path immediately before the raw
submit, after the mapping lookup and the coverage guard, so a write that would
not have been taken over anyway never pays for it. A refused probe returns
`EXITOS_PASS`, is counted in the PASS statistics, and leaves no raw durability
debt behind, so the caller's ordinary-path reissue is the only write that
happens.

The frontends `src/preload.c` and `src/bpftime_hook.c` both reach the mode
through the shared configuration loader in `src/frontend_config.c`, which reads
`EXITOS_STRICT`. Exactly `1` arms it, `0` leaves it off, presence alone does not
arm it, and a malformed value is rejected rather than guessed at.

## 5. What the tests establish

`tests/unit/test_strict_mode.c` covers the mode end to end:

* Fast mode issues no probe, and submits exactly one raw write.
* With strict mode armed and the kernel allowing the write, exactly one probe is
  issued per taken-over write; it is zero-length, on the intercepted descriptor,
  at the write's own offset, and it runs before the raw submit.
* When the probe is refused, no raw write is submitted, the result is left for
  the kernel to set on the ordinary path, the write is counted as a PASS, and no
  unflushed raw debt is left behind.
* `fdatasync` is never probed.
* An unregistered descriptor is never probed.
* `EXITOS_STRICT=1` arms both strict mode and the descriptor identity check at
  frontend init; `EXITOS_STRICT=0` arms neither; `EXITOS_VERIFY_IDENTITY` stays
  an independent bit that does not select Exitos-S on its own.

`tests/unit/test_frontend_config.c` covers the environment-variable parsing,
including rejection of a malformed `EXITOS_STRICT` value.
