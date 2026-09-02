# Shared static frontend configuration

## Outcome

`LD_PRELOAD` and the opt-in active bpftime frontend now obtain path selection,
I/O backend selection, strict mode, and identity policy from one shared
configuration object.  Exitos-S is the strict-policy setting on that same
object; it is not a third frontend and not a separate I/O backend.

The backend is chosen by the operator through `EXITOS_IOPATH`, resolved once
at context initialization, and immutable for the life of that context.  It is
not an 8-KiB detector and it never chooses a backend from the length of an
individual write.  Changing `EXITOS_IOPATH` affects a newly initialized
process/context; changing the canonical implementation or mode table affects
every frontend when the shared sources are rebuilt into their DSOs.

## Ownership and data flow

```text
process environment
        |
        v
exitos_frontend_config_from_env()
  - copies EXITOS_FILES
  - validates exact 0/1 policy flags
  - parses EXITOS_IOPATH through the core's one mode table
  - constructs and owns one immutable-mode exitos_ctx
        |
        +-------------------------+
        |                         |
        v                         v
LD_PRELOAD ABI adapter     active-bpftime ABI adapter
(libc errno/results)       (raw-syscall errno/results)
        |                         |
        +------------+------------+
                     v
            shared interception core
              + optional Exitos-S policy
                     |
                     v
       registration-owned iopath backend
```

The configuration object owns the context.  The adapters borrow it and use an
admission/refcount gate: shutdown first refuses new hook entries, then waits
for admitted calls to leave before destroying the configuration.  The gate has
64 cacheline-separated atomic reference shards, so the lifetime proof does not
reintroduce one process-wide hot RMW cacheline.  A frontend publishes its
configuration/context before `enable`; every entry that can borrow either
holds admission through its last use; teardown calls `quiesce` before destroy.
Each
registration gets its backend policy from the context snapshot and never
re-reads `EXITOS_IOPATH`.  Registration still resolves the device and namespace
identity inputs (`EXITOS_DEV` and the legacy-named `EXITOS_EXPECT_SERIAL`, which
the interception path compares to the namespace WWID) because those checks are
tied to the fd/device being registered; they cannot change the frozen backend
policy.  Hardware runners separately prove the controller serial to PCI to
namespace binding before registration.

## Interfaces

- `include/exitos_frontend_config.h` is the frontend-facing opaque API.
  `exitos_frontend_config_from_env()` constructs the snapshot,
  `exitos_frontend_config_path_selected()` implements the common
  colon-separated selector, and the object owns the borrowed context.
- `exitos_ctx_create_with_options()` is the error-returning core constructor.
  It accepts a mode name directly and records both the backend and whether the
  choice was explicit.
- `exitos_ctx_create()` remains the compatibility constructor for embedding.
  It snapshots `EXITOS_IOPATH` once at context creation.
- `exitos_ctx_strict()` remains the programmatic Exitos-S policy switch.
  Frontend environment loading treats strict mode as implying identity
  verification.
- `include/exitos_frontend_admission.h` and `src/frontend_admission.c` define
  the process-lifetime publish/enter/leave/quiesce protocol shared by both
  adapters.  LD_PRELOAD closes and drains it before its destructor destroys the
  config.  Active bpftime closes and drains it while the transformer still
  points at the hook, restores the original handler, and only then destroys
  frontend-owned state.  Active startup also freezes `NOHOOK`, trace, and noop
  diagnostics before enabling admission and publishing the hook, so an
  existing thread cannot race a later ordinary-variable initialization.

An absent `EXITOS_FILES` leaves either DSO inert.  Once interception is armed,
`EXITOS_STRICT` and `EXITOS_VERIFY_IDENTITY` accept only exact `0` or `1`.
Malformed policy or backend configuration fails before any fd is registered.

## Static backend table

| `EXITOS_IOPATH` | Core backend | Selection rule |
|---|---|---|
| `pwrite` | block `pwrite` | explicit, no fallback |
| `nvme` | NVMe ioctl passthrough | explicit, no fallback |
| `uring` | interrupt-completion `URING_CMD` | explicit, no fallback |
| `uringpoll` | polled `URING_CMD` passthrough | explicit, no fallback |
| `uringwritepoll` | polled ordinary block write | explicit, fail closed |
| unset | legacy unset-mode fallback | frozen ordered policy, attempted when each fd is registered |

Empty or unknown explicit values return `EINVAL`.  A later `setenv()` cannot
make another fd in the same context use a different backend.  In particular,
there is no branch of the form “8 KiB uses block polling, 4 KiB uses
passthrough” on the write path.  The legacy unset-mode fallback may test
device-specific backend availability during registration; that is not a
runtime I/O-size switch.  Explicit modes never fall back.

## Frontend boundaries

- `libexitos_preload.so` interposes libc symbols and translates the shared
  core result into libc return/`errno` semantics.
- `libexitos_bpftime_active.so` is an explicitly requested experimental target.
  It installs the same context behind bpftime syscall-instruction rewriting and
  preserves raw-syscall negative-errno semantics.
- `libexitos_bpftime.so` keeps the frozen production contract: empty claim
  table and no process text rewriting.  Adding the active target did not
  silently change that artifact.
- Exitos-S adds its per-write permission/identity checks before submitting via
  the same registration-owned iopath.  It cannot select or override a backend.

## Verification and archived detour

The configuration behavior is covered by
`tests/unit/test_frontend_config.c` plus the preload lifecycle, strict-mode,
active-bpftime, fdatasync, and buffered-coherence unit suites.  Admission is
covered directly by `tests/unit/test_frontend_admission.c`, and deterministic
teardown-race fixtures prove both adapters wait for an admitted borrower.  The
tests cover the exact mode table, invalid values, environment mutation after
context creation, common adapter initialization, lifecycle ordering, and the
frozen bpftime contract.  They run under `make unit`, including the
contract-only bpftime integration gate.

The admission-gated shared libraries were also exercised against a
serial-resolved NVMe namespace, where a full readback of the written region
matched byte for byte.

The abandoned qdsplit campaign-completeness tests were preserved under
`tests/archive/qdsplit-campaign-completeness/`.  They are not built
or run by the default targets.  `attribution/qdsplit.c` remains as a backend-level
diagnostic; it is not a runtime policy engine.
