# Two defects in bpftime's syscall interception: vector state and the clone double return

## 1. Conclusions

The bpftime frontend in this tree is hard-frozen. Two unrelated defects sit behind that freeze, one
fixed here and one fixed in bpftime upstream.

1. **Vector state is destroyed (this repository, fixed).** Not a crash: a silent wrong value on the
   takeover path, independent of the thread count. The assembly shim saved only the low 128 bits of
   xmm0..15 with `movaps`, so any VEX-encoded instruction inside the C hook (in production, libc's
   AVX `memcpy`) destroys the upper halves. Fixed by switching to XSAVE/XRSTOR over the XCR0
   enabled mask so that the complete state is saved.
2. **clone/clone3 children return from a new stack (bpftime upstream, patch included here).** This
   one is a crash, and it has nothing to do with the code in this repository: it happens the same
   way on the official path with none of this repository's code loaded at all. The patch is
   18 lines, `tools/bpftime-clone-fix.patch`.
3. **Why nobody upstream has reported it: that path is opt-in, and upstream's own tests are all
   single-threaded.** See section 4.

The runs described below were done on a local machine and on a test host running kernel 6.18.0-rc5
with glibc 2.43.

## 2. Vector state: mechanism, fix and verification

**Mechanism.** What gets replaced is the `syscall` **instruction** — it destroys
only rax/rcx/r11; every other general-purpose register, all vector and x87 state, MXCSR and the
128-byte red zone are guaranteed by the CPU and the kernel to come back unchanged. Once it becomes
a `call` into a C function, what applies instead is the System V AMD64 function calling convention,
which permits the callee to destroy xmm0-15, the upper parts of ymm/zmm, the k0-k7 mask registers,
the x87 stack, the MXCSR status bits, AMX tiles and the red zone.

**The fix.** The shim does an XSAVE before entering C and an XRSTOR after returning, with the mask
taken from the actual XCR0 value read by `xgetbv(0)`; an older CPU without XSAVE falls back to
FXSAVE — such a CPU has no AVX either, so the x87+MXCSR+xmm0..15 that FXSAVE saves is the complete
state. Four key points were checked against Intel SDM volume 1 sections 13.2/13.4/13.7/13.14, the
xstate documentation of Linux v6.18-rc5 and glibc's `dl-trampoline`: the buffer must be 64-byte
aligned or XSAVE raises #GP; XSAVE writes only XSTATE_BV and leaves the remaining header bytes as
they were, so the 64-byte header must be zeroed before every save; the buffer size comes from
`CPUID.(EAX=0Dh,ECX=0):EBX`; XFD (on-demand enabling of large state) does not affect the
correctness of a save/restore round trip.

**Why the stack frame must be computed at runtime.** The local machine has
XCR0=0x7 (x87+SSE+AVX) and the computed frame is 1408 bytes; the test host has XCR0=0x602e7, which
includes the XTILECFG and XTILEDATA bits of AMX, and the computed frame is **11584 bytes**. A
hard-coded 3072-byte ceiling would not be large enough on that machine.

**Verification.** A new `tests/unit/test_bpftime_xstate.c`: it populates the claim table
so that the call really enters the C hook (the hook executes VZEROALL to model an arbitrary VEX
instruction) and uses an assembly probe to compare all 256 bits of a live YMM register before and
after the call. The test discriminates between the two shims: the assertion fails against the
`movaps` shim and holds against the XSAVE/XRSTOR shim. `make unit` and `make integ` report no
failures on the local machine, and `test_bpftime_xstate` and `test_bpftime_avx` pass on the test
host.

**Two residual gaps, both out of reach here.** The first is RFLAGS: a real syscall
restores the flags from r11 on sysret, whereas the shim's compare instruction that consults the
claim table changes the flags before any save happens; in practice this is not a problem, because a
syscall written in inline assembly always declares a `cc` clobber and DF is fixed at 0 by the ABI.
The second is the first 16 bytes of the red zone: see section 3, the damage happens before the shim
gets control.

## 3. clone/clone3: mechanism, patch and verification

**bpftime's rewriting mechanism.** `setup_syscall_tracer()` maps the page at
address 0 executable, fills 0..511 with `nop`, and places at address 512 a sequence
`push %rax; movabs $syscall_hooker_asm, %rax; jmp *%rax`; it then scans every executable segment and
rewrites each `syscall` (0F 05) in place into `call *%rax` (FF D0). At run time rax is the syscall
number, so `call *%rax` jumps to address N, slides along the nops to 512, pushes the syscall number
and jumps into the trampoline. What actually issues the syscall is `call_orig_syscall`, whose code
is literally `syscall; ret`.

**The defect.** clone and clone3 return twice. The child comes back from the kernel at that `ret`
inside `call_orig_syscall`, but it is on its own brand-new stack, where no return address was ever
pushed — so it pops whatever the thread stack happens to hold and jumps there. Upstream
special-cases only rt_sigreturn (number 15); there is no handling at all for clone and clone3.

**What this looks like when it happens.** This was observed on the test host with an unmodified
bpftime build. An ordinary `pthread_create` program exits normally when bpftime is not loaded; run
the way bpftime's own README documents for the FUSE benchmark
(`AGENT_SO=... LD_PRELOAD=libbpftime-agent-transformer.so`) it takes SIGSEGV, exit 139, and the
worker never runs. gdb shows the crashing thread at `rip=0x7ffff7e13380`, which is libc's **data
symbol** `_nl_global_locale`; dmesg records `error 15` (instruction fetch). This accounts for a
crash whose mechanism had not been located before, and it explains why the crash kept happening
after the switch to plain-jump forwarding and even after this repository's hook was removed
entirely — both of those sit upstream of the `ret` that is actually wrong.

**Patch approach.** Send clone and clone3 down the path that bypasses the C hook, the same way
rt_sigreturn already goes, and before issuing the syscall **write the application's return address
to the top of the child stack**:

- clone (number 56): arg2 (rsi) is the child stack pointer. `rsi -= 8`, and write the return address
  into `(%rsi)`. The kernel starts the child at rsi, that `ret` pops the return address and rsp is
  back at the original child-stack top — byte-for-byte identical to the un-rewritten behaviour. When
  rsi is 0 the semantics are fork-like, the child keeps the parent's stack layout, the original
  `ret` is already correct, and it is left alone.
- clone3 (number 435): arg1 is `struct clone_args*`, with the stack at offset 40 (stack) and 48
  (stack_size). Take `top = stack + stack_size`, write the return address at `top-8`, then subtract
  8 from stack_size.
- Both use rcx and r11 as scratch: `syscall` destroys those two registers anyway.

**Where the two handler blocks must sit.** They go **after** the `ret`, reachable only by an
explicit jump. Placing them **before** the `handle_sigreturn` label makes the normal path of
`call_orig_syscall` fall through into them: `testq %rsi, %rsi` then reads the arg2 of an ordinary
syscall, and a non-zero value writes a "return address" to `(%rsi-8)`, which is arbitrary memory
corruption. The symptom of that placement is a crash earlier than the one the patch removes.

**Verification.** With `libbpftime-agent-transformer.so` rebuilt after the
patch, the same program on the same official path: threads are created, run and joined normally,
exit 0. Under load, 10 rounds x 16 threads = 160 threads, each issuing 8 real syscalls:
`ok=160 expect=160`, exit code 0; the control arm without bpftime loaded gives the same result. The
patch is kept in `tools/bpftime-clone-fix.patch` (18 lines of change).

**Cost and scope of the patch.** clone and clone3 are from now on not traced (the same treatment
rt_sigreturn already gets) — tracing thread creation would require a different mechanism, which this
patch does not solve. The patch does not change the behaviour of any other syscall. The clone3
branch depends on the layout of the first 56 bytes of `struct clone_args`, which is a stable part of
the kernel UAPI; new fields are only appended.

## 4. Why nobody upstream has hit this

**First, this path is opt-in, not on by default.** In `tools/cli/main.cpp`, the transformer is
loaded only if `--enable-syscall-trace` was passed; without it only frida's uprobe path runs and no
instruction is rewritten at all. bpftime's headline use is a uprobe on a function entry.

**Second, the function-entry path has no vector-state problem by construction.** A uprobe attaches
at a function entry, and the System V AMD64 ABI already specifies that xmm/ymm/zmm are all saved by
the caller across a function call, so a hook destroying vector state is harmless — the ABI already
covers it. Replacing the `syscall` instruction is entirely different; see section 2. This is why
the vector defect shows up only on the instruction-rewriting path.

**Third, upstream's own tests of this path all use single-threaded programs.**
`benchmark/syscall/victim.cpp` contains 0 occurrences of `pthread`/`std::thread`;
`.github/workflows/test-bpftrace.yml` runs `cat` under the transformer. The one multi-threaded case,
`benchmark/redis-durability-tuning/benchmark.py`, writes the transformer as
`build/runtime/agent-transformer/...`, and that directory no longer exists in the current tree (the
library is at `attach/text_segment_transformer/`); an LD_PRELOAD pointing at a file that does not
exist is skipped silently — which means that benchmark does not actually load the transformer.

**Fourth, there is one more barrier: root is required.** This path has to map the page at address 0
executable, and `vm.mmap_min_addr` defaults to 65536 on distributions; it is 65536 on the test host
too, and the mapping succeeds only because the runs are done as root (CAP_SYS_RAWIO can bypass that
limit). An ordinary user running this path fails at the mmap.

**Search of the upstream repository.** Across all 284 issues of eunomia-bpf/bpftime, all pull
requests and the commit log, the terms
xmm/ymm/avx/sse/xsave/fxsave/vector register/red zone/text_segment_transformer
all have 0 hits in a syscall-interception context; a control query for
"syscall" returns 33 commits, which shows the search itself works. The related problems upstream has
fixed belong to a different family: #563 (a null-pointer race in initialize_ctx under multiple
threads; that fix is already in the checkout used here, and it belongs to the syscall server path),
#573 (errno being overwritten), #611 (misaligned perf records), #429/#430 (ring-buffer races), #605
(a still-open umbrella issue that an LD_PRELOAD shim must be transparent to its host, whose
enumerated items stop at process exit, stdio and exceptions, and do not include registers or vector
state).

**This is also not a problem with the compiler or the build configuration.** A 2x2 control was run
on the test host: same machine, same build, same agent, changing only two variables — whether
the library is patched, and whether the victim program is single-threaded or multi-threaded:

- unpatched + single-threaded victim program: exit code 0, **no crash**.
- unpatched + multi-threaded victim program: exit code 139, SIGSEGV.
- patched + single-threaded victim program: exit code 0, no crash.
- patched + multi-threaded victim program: exit code 0, no crash.

The first row shows that the build and the configuration are sound: the upstream library as it
stands does not crash on a single-threaded program. Upstream's passing runs are therefore genuine,
just narrow in coverage — every upstream test uses a single-threaded victim program (see the
previous paragraph). The only variable in the crash is threads, not the compiler, not the build
options, not the machine configuration.

**But "no crash" is not "nothing went wrong"; this table covers only the crash, not the vector
state.** The victim programs in the four rows above hold no live vector data across the syscall, so
the silent wrong-value defect of section 2 would not be observable even if it occurred. Two
additional single-threaded experiments on the test host separate the two:

- The unpatched upstream library plus its **default pass-through hook**, a single-threaded program
  keeping a live YMM register across a syscall: both the low and the high 128 bits are intact. The
  reason is that the C code executed on the default-hook path (`syscall_hooker_cxx`) is only a
  forwarder, and what it compiles to touches no vector register; the hazard exists but does not
  show.
- The same library with a **hook that does use vector registers** (installed with `set_call_hook`, a
  hook that executes VZEROALL and then forwards, modelling what an eBPF runtime or this
  repository's interception core would do), also **single-threaded**: all 32 bytes of YMM0 are
  zeroed, both the low and the high 128 bits destroyed.

So the vector defect really is independent of the thread count; a single thread is hit just the
same. It needs two conditions to hold at the same time before it is visible — the application really
is keeping live vector data across the syscall, and the hook body really does use vector registers.
Upstream's own tests satisfy neither, and the defect is **silent** (no crash, no error), so their
tests, which only check "the program ran to completion and the trace came out", could not detect it
even if it happened.

The compiler cannot be the cause either, and there is artifact-level evidence for that: the
`call_orig_syscall` at fault is hand-written inline assembly, which the compiler does not rewrite.
Disassembling the built `libbpftime-agent-transformer.so` shows exactly `0f 05` (`syscall`)
immediately followed by `c3` (`ret`), matching the source word for word.

**This is not a CPU difference.** The clone one is a control-flow defect, unrelated to the CPU
model; any x86-64 behaves the same. The **nature** of the vector one is also CPU-independent; only
**what specifically is lost** depends on which states the CPU supports (the local machine has only
AVX, the test host also has AVX-512 and AMX), and that is exactly why the fix must compute from XCR0
at runtime and cannot hard-code it. The glibc version decides whether thread creation goes through
clone or clone3 (glibc 2.43 on the test host goes through clone3), but both return twice and both
crash, so the patch covers both.

## 5. How to use it from now on (the settled invocation)

This project needs to intercept bare syscalls (an LD_PRELOAD frontend cannot see a write issued from
inline assembly), so **using the bpftime frontend at all requires the path with syscall rewriting
enabled**, that is, the equivalent of the CLI's `--enable-syscall-trace`:

~~~bash
AGENT_SO=<bpftime>/build/runtime/agent/libbpftime-agent.so \
LD_PRELOAD=<bpftime>/build/attach/text_segment_transformer/libbpftime-agent-transformer.so \
<program>
~~~

Or the project's own "route A": dlopen the transformer, take `_frida_cs_arch_register_x86` and
`_ZN7bpftime20setup_syscall_tracerEv`, register Capstone's x86 architecture first and then call
`setup_syscall_tracer()`, then install this repository's own hook with
`_ZN7bpftime13set_call_hookEPFllllllllE`. Note that the Capstone symbols carry a `_frida_` prefix (a
consequence of frida's packaging); a dlsym with the unprefixed name returns a null pointer, which
shows up as `Failed to open capstone instance: CS_ERR_ARCH`. This was observed directly.

Preconditions: run as root (otherwise mapping the zero page fails); use a transformer with
`tools/bpftime-clone-fix.patch` applied (otherwise any program that creates a thread crashes).

## 6. Current status

The vector-state defect is fixed, has a test, and passes on both the local machine and the test
host. The clone patch is verified on the test host, but it belongs in the upstream repository rather
than in this one; the bpftime frontend here stays hard-frozen, because unfreezing it would mean
depending on a local patch that upstream has not accepted.
