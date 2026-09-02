# PR draft: fix clone/clone3 children returning through a `ret` on their own stack

Not submitted. `tools/bpftime-clone-fix.patch` holds the change; this file is the text to
paste into the pull request. Target: `eunomia-bpf/bpftime`.

---

## Title

`fix(syscall-trace): return clone/clone3 children to the rewritten call site`

## Summary

With `--enable-syscall-trace`, any program that creates a thread crashes. `pthread_create`
is enough. The child never runs.

The trampoline installed for a rewritten `syscall` instruction ends in
`call_orig_syscall`, which is `syscall; ret`. `clone` and `clone3` return twice: the child
resumes at that `ret`, but on its own brand-new stack, where no return address was ever
pushed. It pops whatever the fresh thread stack happens to hold and jumps there.

`rt_sigreturn` is already special-cased for a related reason. `clone` and `clone3` are not
special-cased anywhere.

## Reproduction

Any threaded program, run the way `benchmark/fuse/bpf/README.md` documents:

```console
$ cat > t.c <<'EOF'
#include <pthread.h>
#include <stdio.h>
static void *worker(void *p) { (void)p; fprintf(stderr, "[worker ran]\n"); return 0; }
int main(void) {
    pthread_t t;
    fprintf(stderr, "[creating thread]\n");
    if (pthread_create(&t, NULL, worker, NULL) != 0) return 1;
    fprintf(stderr, "[thread created]\n");
    pthread_join(t, NULL);
    fprintf(stderr, "[done]\n");
    return 0;
}
EOF
$ gcc -O2 -o t t.c -lpthread

$ ./t                                     # no bpftime
[creating thread]
[thread created]
[worker ran]
[done]

$ sudo AGENT_SO=build/runtime/agent/libbpftime-agent.so \
       LD_PRELOAD=build/attach/text_segment_transformer/libbpftime-agent-transformer.so ./t
[creating thread]
[thread created]
Segmentation fault (core dumped)          # exit 139, [worker ran] never printed
```

The child faults executing a data address. On the machine below it landed on a libc data
symbol:

```
Thread 2 received signal SIGSEGV
0x00007ffff7e13380 in _nl_global_locale () from /usr/lib/x86_64-linux-gnu/libc.so.6
```

```
kernel: t[417234]: segfault at 72f5b5813380 ip 000072f5b5813380 sp 000072f5b47ff648
        error 15 in libc.so.6
```

`error 15` is an instruction fetch: the child jumped into data, which is what popping a
non-return-address off a fresh stack looks like.

Environment: Ubuntu 26.04, kernel 6.18.0-rc5, glibc 2.43 (so `pthread_create` uses
`clone3`), gcc 15.2, x86-64, bpftime at 2a45936.

The same shape reproduces with no bpftime at all, which isolates the mechanism from
everything else in the runtime:

```c
/* orig_syscall_shape is `syscall; ret`, exactly call_orig_syscall's tail. */
top = child_stack + size - 16;
*(void **)top = (void *)child_landed;          /* prime the child stack */
orig_syscall_shape(SYS_clone, CLONE_VM | SIGCHLD, (long)top, 0, 0, 0);
```

The child reaches `child_landed`, i.e. it really does execute the `ret` against its own
new stack. A real thread stack holds no such address there.

## Severity

Every program that creates a thread, under the documented syscall-tracing path. Not a
corner case: `pthread_create` alone triggers it, and the child dies before running any of
its own code, so there is no partial-work window to reason about — but there is also no
error, just SIGSEGV in a place unrelated to the cause, which makes it expensive to
diagnose from the outside.

Nothing else is affected: the uprobe/frida path does not rewrite instructions, and
single-threaded programs never issue `clone`.

## Why the examples, benchmarks and CI do not reproduce this

This is worth spelling out, because "the tutorial works for me" is the natural first
reaction to a report like this — and on the current tree it genuinely does work, for
reasons that have nothing to do with the trampoline being correct.

The syscall-rewriting path is opt-in (`--enable-syscall-trace`; `bpftime start` without it
loads only the agent). Every in-tree exercise of it is single-threaded, and the one
threaded case does not load the transformer at all:

- **The documented tutorial never creates a thread.** `tools/README.md` is the only file
  documenting `-s, --enable-syscall-trace`, and both of its examples are
  `bpftime start -s ./target_application` — a placeholder. Whether you hit this depends
  entirely on which application you substitute, and the docs give no threaded one.

- **`benchmark/syscall/victim.cpp` contains no `pthread`/`std::thread`.** It is a
  `getppid()` loop, so it never issues `clone`.

- **CI runs `cat` under the transformer.**
  `.github/workflows/test-bpftrace.yml:229` is
  `sudo -E env AGENT_SO=$AGENT_LIB LD_PRELOAD=$TRANSFORMER_LIB cat build/install_manifest.txt`.
  `cat` is single-threaded, so this job passes whether or not the trampoline handles
  `clone` correctly. The other syscall-trace CI coverage,
  `.github/workflows/test-attach.yml`, builds and runs
  `bpftime_syscall_trace_attach_tests`, which unit-tests the attach implementation without
  going through the rewritten trampoline in a live threaded process.

- **The one threaded example silently does not load the transformer.**
  `benchmark/redis-durability-tuning/benchmark.py:130` sets
  `LD_PRELOAD=build/runtime/agent-transformer/libbpftime-agent-transformer.so`. That path
  does not exist. The target is declared in `attach/text_segment_transformer/CMakeLists.txt`
  with `OUTPUT_NAME "bpftime-agent-transformer"`, so the built library is at
  `build/attach/text_segment_transformer/libbpftime-agent-transformer.so` — which is
  exactly the path `.github/workflows/test-bpftrace.yml` uses. The dynamic loader skips a
  missing `LD_PRELOAD` entry silently, with no warning and no non-zero exit, so this
  benchmark runs unmodified Redis, reports success, and never exercises the code it is
  named after.

The last point is the one I would flag independently of this patch: **a green result from
that benchmark is not evidence that syscall tracing works**, and the same stale path would
hide any future regression in the transformer just as effectively.

Two further reasons this stays out of sight: the rewriting needs `CAP_SYS_RAWIO` or
`vm.mmap_min_addr=0` to map the page at address 0, so a casual user does not stumble into
it; and when it does fire, the failure is a `SIGSEGV` at an address unrelated to the cause,
which reads as "my program crashed" rather than "the tracer broke my program".

I could not find an existing issue describing this. If one exists, I am happy to close this
in favour of it.

## The fix

Send `clone` and `clone3` down the untraced path the way `rt_sigreturn` already goes, and
before issuing the syscall, seed the child stack so that the child's `ret` lands where the
replaced `syscall` instruction would have continued.

- `clone`: arg2 is the child stack pointer. Decrement it by 8 and store the return address
  there. The kernel starts the child at that pointer; `ret` pops the address and leaves
  `rsp` at the original child-stack top, which is exactly the un-rewritten behaviour. A
  null child stack is fork-like — the child keeps the parent's layout and the ordinary
  `ret` is already correct — so it is left alone.
- `clone3`: arg1 is `struct clone_args *`; the stack is `stack` (offset 40) plus
  `stack_size` (offset 48). Write the return address at `stack + stack_size - 8` and
  shrink `stack_size` by 8.
- `rcx` and `r11` are the scratch registers, because the `syscall` that follows destroys
  both anyway.

Cost: `clone` and `clone3` are no longer traced, the same trade already made for
`rt_sigreturn`. No other syscall changes behaviour.

One placement detail matters and cost me a debugging round: the two handlers must sit
**after** the `ret`, reachable only by an explicit jump. Putting them before
`handle_sigreturn` makes `call_orig_syscall`'s normal path fall through into them, where
`testq %rsi, %rsi` reads an ordinary syscall's second argument and a non-zero value causes
a write to arbitrary memory.

## Test results

All on the environment above.

| | single-threaded victim | threaded victim |
|---|---|---|
| before this change | exit 0 | **exit 139, SIGSEGV** |
| after this change | exit 0 | exit 0 |

Stress: 10 rounds x 16 threads = 160 threads, each issuing 8 real syscalls, under the
patched transformer with rewriting active: every thread ran and joined,
`ok=160 expect=160`, exit 0. The same program without bpftime gives the same result.

A longer run on top of this change: a multi-threaded I/O-heavy program under a syscall
hook, 16384 writes and 16383 fdatasyncs, all intercepted, no crash, and the program's own
read-back verification pass reported no mismatches.

## Notes for reviewers, beyond this patch

Two further gaps in the same trampoline, both out of scope here, both worth knowing:

1. **No vector state is saved across the C hook.** The rewritten instruction stands in for
   `syscall`, which preserves every vector register; the C ABI the trampoline uses instead
   treats `xmm/ymm/zmm` as call-clobbered. With the default pass-through hook nothing in the
   path touches them, so it does not show. With a hook that does — an eBPF runtime, or
   anything that calls libc's AVX `memcpy` — a live `ymm0` across a syscall comes back
   destroyed. Measured single-threaded on the environment above: all 32 bytes of a live
   `ymm0` zeroed. This is silent; no crash, no error. A complete fix is
   `XSAVE`/`XRSTOR` over the `XCR0`-enabled mask with a runtime-sized 64-byte-aligned
   buffer (`CPUID.(EAX=0Dh,ECX=0):EBX`; that value already includes the ~8 KiB tile area on
   an AMX machine, so the buffer cannot be a compile-time constant).
2. **The six syscall argument registers are clobbered.** They are pushed to build the C
   argument list and then discarded by `leave`, never popped back. A real Linux `syscall`
   preserves `rdi/rsi/rdx/r10/r8/r9`, so inline-asm syscall sites that keep live values in
   them read garbage afterwards.
3. **The first 16 bytes of the caller's red zone are gone before any bpftime code runs**:
   `call *%rax` pushes a return address and the page-zero stub pushes `rax`. Inherent to
   rewriting a 2-byte `syscall` into a `call`; noted for the record rather than as a
   request.

Happy to split any of these into their own issues or PRs if that is easier to review.
