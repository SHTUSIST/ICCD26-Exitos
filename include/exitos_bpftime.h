/* Backend 2: capture syscalls by rewriting syscall instructions in the process
 * image (bpftime's zpoline transformer), rather than by replacing libc symbols.
 *
 * WHY: backend 1 (src/preload.c) interposes libc entry points, so it cannot see
 * a write issued by inline assembly. This repo's own raw-syscall writer is
 * missed by it, and Go runtimes and statically linked binaries issue syscalls
 * the same way. Rewriting the instructions catches those too.
 *
 * MECHANISM (route A of docs/bpftime-build.md, verified there):
 *   dlopen libbpftime-agent-transformer.so
 *   resolve and validate bpftime::get_call_hook() while failure is reversible
 *   call _frida_cs_arch_register_x86   <- MUST precede setup; without it
 *                                         Capstone fails and exits(1)
 *   call bpftime::setup_syscall_tracer()
 *   install ours with bpftime::set_call_hook()
 * Inside the hook, returning a value WITHOUT calling the saved original is how
 * "skip the syscall and use my result" is expressed. No eBPF program is
 * involved, and the hook is a plain C function pointer.
 *
 * TWO DEPLOYMENT CONSTRAINTS, both measured, both worth knowing before use:
 *
 *  1. It needs root. setup_syscall_tracer() maps a page at address 0, which
 *     mmap_min_addr forbids for an unprivileged process; the transformer then
 *     exit(1)s rather than degrading. (Its own error log misreports errno as 25
 *     / ENOTTY; the real error is EPERM. Do not chase ENOTTY.)
 *
 *  2. It scans /proc/self/maps ONCE. Code dlopen'd afterwards is never rewritten
 *     and its syscalls are missed — measured directly: a function in a library
 *     loaded after setup was not intercepted. Anything that loads plugins at
 *     runtime (fio's ioengines, for one) must either be set up after those loads
 *     or have setup called again.
 */
#ifndef EXITOS_BPFTIME_H
#define EXITOS_BPFTIME_H

/* Does this build take over this syscall number?  The shipped production DSO
 * always returns zero.  The explicitly built active DSO uses its immutable
 * syscall-claim table; complete XCR0-enabled state preservation is tested. */
int  exitos_bpftime_claims(long nr);

/* Is the transformer usable?  The shipped production DSO always returns zero;
 * the opt-in active DSO accepts an existing regular transformer file. */
int  exitos_bpftime_available(const char *transformer_so);

/* Install the hook.  The shipped production DSO returns -EOPNOTSUPP before
 * loading or rewriting anything because activation depends on the local
 * clone/clone3 transformer fix below.  The opt-in active DSO returns zero on
 * success; every returned failure occurs before process text is rewritten. */
int  exitos_bpftime_start(const char *transformer_so);
void exitos_bpftime_stop(void);
int  exitos_bpftime_started(void);

/* How many times the hook was entered at all, and how many of those were
 * syscalls this backend claims. Entered==0 means the rewriter never routed
 * anything to us, which is a different failure from claiming nothing. */
unsigned long exitos_bpftime_entries(void);
unsigned long exitos_bpftime_claimed(void);

/* Fallback location, relative to the working directory; set
 * EXITOS_BPFTIME_SO to the absolute path of your own build. The value below
 * is bpftime's own default build layout and is only a convenience -- when it
 * does not resolve, the loader reports the backend inert instead of guessing.
 *
 * This names the TRANSFORMER, not the agent, and that is deliberate: the
 * transformer is what rewrites `syscall` instructions, which is the whole
 * reason this backend exists (an LD_PRELOAD frontend cannot see a write
 * issued from inline assembly). Loading the agent alone gives frida uprobes
 * and no syscall interception at all. The bpftime CLI expresses the same
 * choice as `--enable-syscall-trace`; this backend is always the equivalent
 * of that flag being on, and there is no configuration in which it should be
 * off.
 *
 * Two hard prerequisites for this path, both learned the hard way and both
 * written up in docs/bpftime-clone-and-xstate.md:
 *   - It must run as root. The transformer maps the page at address 0, and
 *     vm.mmap_min_addr is 65536 on a stock distribution; only CAP_SYS_RAWIO
 *     gets past that.
 *   - The transformer must carry tools/bpftime-clone-fix.patch. Without it,
 *     upstream returns clone/clone3 children through a `ret` on their own
 *     fresh stack and every program that creates a thread dies with SIGSEGV,
 *     rip inside libc's data segment.
 */
#define EXITOS_BPFTIME_DEFAULT_SO \
    "build/attach/text_segment_transformer/libbpftime-agent-transformer.so"
#endif
