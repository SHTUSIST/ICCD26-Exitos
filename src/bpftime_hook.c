#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_bpftime.h"
#include "exitos_frontend_admission.h"
#include "exitos_frontend_config.h"
#include "exitos_frontend_fallocate.h"
#include "exitos_intercept.h"
#include "exitos_stats.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>

/* The transformer's hook type: syscall number plus six arguments, C ABI. */
typedef int64_t (*hook_fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

static void            *g_so;
hook_fn                 exitos_bpftime_orig;   /* "just run the real syscall" */
#define g_orig exitos_bpftime_orig
static struct exitos_ctx *g_ctx;
static int              g_started;
static struct exitos_frontend_config *g_config;
static struct exitos_frontend_fallocate_runtime *g_fallocate_runtime;
static int              g_fastpath = 1;

/* Our own fast path issues syscalls (ioctl, pwrite). Without this guard those
 * would re-enter the hook and recurse until the stack ran out. */
/* initial-exec, not the default general-dynamic model. Reaching a
 * general-dynamic thread-local from a preloaded library calls
 * __tls_get_addr, which on a thread's first access can allocate -- and
 * allocating issues syscalls, which come straight back into this hook while the
 * guard it is trying to read does not exist yet. Every single-threaded test
 * passed because the main thread's block is set up before main() runs; the
 * first worker thread of a real program crashed with rip = 0xffffffffffffffff.
 * initial-exec puts the slot in the static block that every thread gets at
 * creation, so reading it calls nothing. It is only valid for a library loaded
 * at program start, which is exactly how this one is loaded. */
static __thread int __attribute__((tls_model("initial-exec"))) g_internal_fds[8];
static __thread unsigned __attribute__((tls_model("initial-exec"))) g_internal_depth;
#ifdef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
static __thread int __attribute__((tls_model("initial-exec"))) g_in_hook;
/* Depth of the library's OWN control path on this thread.
 *
 * The reentry branch below poisons the context when it sees a nested openat or
 * a nested mutating syscall, on the reasoning that the library issues its own
 * syscalls through the direct-call API and anything else arriving mid-hook is
 * asynchronous application reentry.  That reasoning has one hole: registration
 * reads sysfs through opendir() and fopen(), whose open/close syscalls libc
 * issues from inside its own wrappers, where no direct-call API can reach.
 * Under backend 2 those instructions are rewritten too, so they came back into
 * the hook and poisoned the context -- measured on the test host, where
 * every registration failed with the device probe reporting
 * EXITOS_DEV_UNKNOWN and registration refused with -EPERM.
 *
 * A nonzero depth means this thread is synchronously inside library control
 * code, so a nested syscall is ours by construction.  Registration is not a
 * raw-transaction window: no device write is in flight, which is the thing the
 * poison exists to protect.  The counter is only ever raised around control
 * paths, never around a raw write. */
static __thread int __attribute__((tls_model("initial-exec"))) g_ctl_depth;
static __thread int __attribute__((tls_model("initial-exec"))) g_internal_wrapper_active;
#endif

void exitos_internal_fd_enter(int fd)
{
    if (g_internal_depth < sizeof(g_internal_fds) / sizeof(g_internal_fds[0]))
        g_internal_fds[g_internal_depth++] = fd;
    else
        exitos_ctx_poison(g_ctx);
}

void exitos_internal_fd_leave(int fd)
{
    if (g_internal_depth && g_internal_fds[g_internal_depth - 1] == fd)
        g_internal_depth--;
    else {
        g_internal_depth = 0;
        exitos_ctx_poison(g_ctx);
    }
}

static int internal_libc_result(int64_t rc)
{
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }
    if (rc > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)rc;
}

/* The bpftime DSO does not contain preload.c.  Its common iopath/control
 * sources must therefore override iopath.c's weak libc implementations here.
 * Calling g_orig directly avoids a rewritten syscall and converts raw-kernel
 * negative errno to the libc contract these helpers expose. */
int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(g_orig(SYS_openat, AT_FDCWD,
                                       (int64_t)(uintptr_t)path, flags, mode,
                                       0, 0));
}

int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(g_orig(SYS_openat, dirfd,
                                       (int64_t)(uintptr_t)path, flags, mode,
                                       0, 0));
}

int exitos_internal_close_call(int fd)
{
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(g_orig(SYS_close, fd, 0, 0, 0, 0, 0));
}

int exitos_internal_ftruncate_call(int fd, off_t len)
{
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(
        g_orig(SYS_ftruncate, fd, (int64_t)len, 0, 0, 0, 0));
}

int exitos_internal_fallocate_call(int fd, int mode, off_t off, off_t len)
{
#ifdef SYS_fallocate
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(
        g_orig(SYS_fallocate, fd, mode, (int64_t)off, (int64_t)len, 0, 0));
#else
    (void)fd;
    (void)mode;
    (void)off;
    (void)len;
    errno = ENOSYS;
    return -1;
#endif
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{
    int64_t rc;
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    rc = g_orig(SYS_pwrite64, fd, (int64_t)(uintptr_t)buf, (int64_t)len,
                (int64_t)off, 0, 0);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }
    return (ssize_t)rc;
}

int exitos_internal_fdatasync_call(int fd)
{
    if (!g_orig) {
        errno = ENOSYS;
        return -1;
    }
    return internal_libc_result(g_orig(SYS_fdatasync, fd, 0, 0, 0, 0, 0));
}

#ifdef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
static int timed_data_or_sync_nr(int64_t nr)
{
    switch (nr) {
    case SYS_write:
    case SYS_pwrite64:
    case SYS_writev:
    case SYS_pwritev:
#ifdef SYS_pwritev2
    case SYS_pwritev2:
#endif
    case SYS_fdatasync:
    case SYS_fsync:
        return 1;
    default:
        return 0;
    }
}

static int reentrant_nr_mutates(int64_t nr, int64_t cmd)
{
    switch (nr) {
    case SYS_write:
    case SYS_pwrite64:
    case SYS_writev:
    case SYS_pwritev:
#ifdef SYS_pwritev2
    case SYS_pwritev2:
#endif
    case SYS_ftruncate:
#ifdef SYS_fallocate
    case SYS_fallocate:
#endif
    case SYS_close:
#ifdef SYS_close_range
    case SYS_close_range:
#endif
    case SYS_dup:
    case SYS_dup2:
    case SYS_dup3:
        return 1;
    case SYS_fcntl:
        return cmd == F_SETFL || cmd == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
            || cmd == F_DUPFD_CLOEXEC
#endif
            ;
    default:
        return 0;
    }
}

static int reentrant_internal_try_begin(int64_t nr, int fd)
{
    /* All library-owned mutable syscalls use g_orig directly. A rewritten
     * syscall reaching this branch is user/signal reentry; legacy exact-fd
     * markers no longer grant an exemption with a stealable pre-wrapper gap. */
    (void)nr;
    (void)fd;
    return 0;
}

static int reentrant_internal_begin(int64_t nr, int fd)
{
    if (reentrant_internal_try_begin(nr, fd))
        return 1;
    exitos_ctx_poison(g_ctx);
    return 0;
}
#endif
/* Unconditional entry counter. Without it, "nothing was intercepted" cannot be
 * told apart from "the hook was never called": both look like a zero fast-path
 * count, and they have completely different causes. */
static _Atomic unsigned long g_hook_entries;
static _Atomic unsigned long g_hook_claimed;
#ifdef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
static int              g_trace;
static int              g_noop;
#endif

/* One byte per syscall number, so the assembly shim can decide whether to build
 * a stack frame without calling anything. Kept in step with
 * exitos_bpftime_claims() by construction: that function reads this table. */
unsigned char exitos_claim_tbl[512];

#if defined(__x86_64__)
/* Complete-xstate save/restore across the C hook.
 *
 * The rewritten call site stands in for a `syscall` instruction, which
 * preserves every vector register; the C ABI the shim must use instead treats
 * xmm/ymm/zmm as call-clobbered. Saving xmm0..15 with `movaps` captures only
 * the low 128 bits, so any VEX-encoded instruction in the hook -- including
 * libc's own AVX memcpy -- destroys the upper halves with nothing to restore
 * them from. XSAVE/XRSTOR over the XCR0-enabled mask is the only save that
 * covers whatever the CPU and kernel actually enabled: x87, SSE, AVX,
 * AVX-512 with its opmask registers, PKRU, and anything added later.
 *
 * exitos_xstate_use selects the instruction pair the shim executes:
 *   1  XSAVE/XRSTOR over exitos_xstate_lo:hi (the XCR0 mask).
 *   0  FXSAVE/FXRSTOR, the pre-XSAVE baseline. Every x86-64 CPU has it, and a
 *      CPU without OSXSAVE has no AVX either, so x87 + MXCSR + xmm0..15 is
 *      the complete vector state there.
 * exitos_xstate_frame is how far the shim moves %rsp: the caller's 128-byte
 * red zone, the fixed slots, the save area, and 64 bytes of alignment slack.
 * The area size comes from CPUID.(EAX=0Dh,ECX=0):EBX, which on an AMX machine
 * already includes the ~8 KiB tile region, so the frame is computed at run
 * time rather than capped at a compile-time guess. The header is zeroed before
 * every XSAVE because XSAVE writes only XSTATE_BV and leaves the rest of those
 * 64 bytes as it found them.
 *
 * Two residuals this does NOT close, both upstream of the shim and both
 * unreachable from here:
 *   - RFLAGS. A real syscall restores flags from r11 on sysret; the shim's
 *     claim-table test destroys them before any save could run. In practice
 *     every inline-asm syscall declares a "cc" clobber and DF is ABI-fixed at
 *     0, so no compiler keeps live flags across a syscall.
 *   - The first 16 bytes of the red zone. bpftime rewrites `syscall` into
 *     `call *%rax`, whose pushed return address, plus the `push %rax` in its
 *     page-zero stub, land at [rsp-16, rsp) before any code of ours runs. */
unsigned int  exitos_xstate_use;
unsigned int  exitos_xstate_lo;
unsigned int  exitos_xstate_hi;
unsigned long exitos_xstate_frame;

/* Fixed slots below the save area; the area itself starts here and must stay a
 * multiple of 64 so that a 64-aligned %rsp keeps it 64-aligned for XSAVE. */
#define EXITOS_XSTATE_AREA_OFF 384
#define EXITOS_STR1(x) #x
#define EXITOS_STR(x) EXITOS_STR1(x)

__attribute__((constructor)) static void exitos_xstate_init(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned long area;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    if (ecx & (1u << 27)) {          /* OSXSAVE: the kernel enabled XSAVE */
        unsigned int lo = 0, hi = 0;
        /* xgetbv(0); written as bytes so the file needs no -mxsave. */
        __asm__ volatile(".byte 0x0f,0x01,0xd0"
                         : "=a"(lo), "=d"(hi) : "c"(0u));
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x0du), "c"(0u));
        /* EBX is the save-area size for the currently enabled XCR0. */
        area = ebx < 576u ? 576u : (unsigned long)ebx;
        exitos_xstate_lo = lo;
        exitos_xstate_hi = hi;
        exitos_xstate_use = 1;
    } else {
        area = 512u;                 /* FXSAVE area */
        exitos_xstate_use = 0;
    }
    area = (area + 63u) & ~63ul;
    /* 128 red zone + fixed slots + save area + 64 alignment slack. */
    exitos_xstate_frame = 128u + EXITOS_XSTATE_AREA_OFF + area + 64u;
    exitos_xstate_frame = (exitos_xstate_frame + 63u) & ~63ul;
}
#endif

__attribute__((constructor)) static void exitos_claim_tbl_init(void)
{
#ifdef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
    static const int claimed[] = {
        SYS_write, SYS_pwrite64, SYS_writev, SYS_pwritev,
#ifdef SYS_pwritev2
        SYS_pwritev2,
#endif
        SYS_fdatasync, SYS_fsync, SYS_openat, SYS_close,
#ifdef SYS_fallocate
        SYS_fallocate,
#endif
#ifdef SYS_close_range
        SYS_close_range,
#endif
        SYS_dup, SYS_dup2, SYS_dup3, SYS_fcntl, SYS_ftruncate,
    };
    size_t i;
    /* Diagnostic: with this set the table stays empty, so the shim forwards
     * every syscall with a plain jump and no frame is ever built. It separates
     * "entering and leaving the hook is wrong" from "the work inside it is". */
    if (getenv("EXITOS_BPFTIME_NOCLAIM"))
        return;
    for (i = 0; i < sizeof claimed / sizeof claimed[0]; i++)
        if (claimed[i] >= 0 && claimed[i] < 512)
            exitos_claim_tbl[claimed[i]] = 1;
#else
    /* HARD FREEZE. The xstate half of the original reason is now fixed: the
     * shim saves and restores the complete XCR0-enabled state with
     * XSAVE/XRSTOR (see exitos_xstate_init above), proven by
     * tests/unit/test_bpftime_xstate.c, which drives this dispatching
     * configuration with a live YMM register across a hook that executes
     * VZEROALL.
     *
     * What still blocks activation is upstream and is not reachable from
     * here. bpftime rewrites every `syscall` instruction into `call *%rax`,
     * which lands on a mapped page zero and jumps to its own trampoline; the
     * real syscall is finally issued by its call_orig_syscall, which is
     * `syscall; ret` (attach/text_segment_transformer/text_segment_transformer.cpp).
     * clone and clone3 return TWICE, and the child resumes at that `ret` on
     * its own brand-new stack, where no return address was ever pushed, so it
     * pops whatever the thread stack happens to hold and jumps there. Nothing
     * in bpftime special-cases clone -- only rt_sigreturn is. Reproduced
     * outside bpftime with the same `syscall; ret` shape: the
     * child demonstrably returned through the `ret` onto its fresh stack. This
     * is exactly the rip=0xffffffffffffffff signature the project recorded on
     * threaded programs, and it is why that crash survived both our
     * plain-jump forwarding and removing our hook entirely -- both sit
     * upstream of the `ret` that actually misfires.
     *
     * Keeping every byte zero makes even an accidental direct installation of
     * the shim a tail-jump-only forwarder. */
    memset(exitos_claim_tbl, 0, sizeof(exitos_claim_tbl));
#endif
}

int exitos_bpftime_claims(long nr)
{
#ifndef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
    (void)nr;
    return 0;
#else
    switch (nr) {
    case SYS_write:
    case SYS_pwrite64:
    case SYS_writev:
    case SYS_pwritev:
#ifdef SYS_pwritev2
    case SYS_pwritev2:
#endif
    case SYS_fdatasync:
    case SYS_fsync:
    case SYS_openat:
    case SYS_close:
#ifdef SYS_close_range
    case SYS_close_range:
#endif
    case SYS_dup:
    case SYS_dup2:
    case SYS_dup3:
    case SYS_fcntl:
    case SYS_ftruncate:
#ifdef SYS_fallocate
    case SYS_fallocate:
#endif
        return 1;
    default:
        return 0;
    }
#endif
}

/* Assembly shim around the C hook. It restores the register contract that the
 * replaced instruction had and the rewritten call site does not.
 *
 * bpftime's trampoline (syscall_hooker_asm in text_segment_transformer.cpp)
 * pushes rax, rdi, rsi, rdx, r10, r8 and r9, rearranges them into C argument
 * order, calls the hook, and then does `leave; ret` -- the pushed copies are
 * discarded, never popped back. A real `syscall` instruction destroys only rcx
 * and r11 and leaves the six argument registers alone, so any compiler is
 * entitled to keep live values in them across a syscall. gcc -O2 does exactly
 * that: an inlined pwrite loop kept its length and its loop counter in those
 * registers, and after one interception the counter came back as 50075857 and
 * the length as a pointer-shaped number. At -O0 the same program is fine
 * because every value is reloaded from memory, which is why this hides easily.
 *
 * We can put the six registers back because their values are the syscall
 * arguments, which arrive here as ordinary parameters. rcx and r11 are left
 * alone: the instruction we stand in for destroys those too.
 *
 * Two more differences with the same shape are handled here: the caller's
 * 128-byte red zone (untouched by `syscall`, overwritten by any C stack frame)
 * and the vector registers (preserved by `syscall`, caller-saved in C).
 *
 * Frame: 0(%rsp) is the 7th argument the callee reads off the stack, 16..271
 * the vector registers, 272..319 the six argument values. 456 keeps everything
 * clear of the red zone and is 8 mod 16, so %rsp is aligned at the call. */
#ifndef EXITOS_BPFTIME_NO_ASM
__asm__(
    ".text\n\t"
    ".globl exitos_syscall_hook\n\t"
    ".hidden exitos_syscall_hook\n\t"
    ".type exitos_syscall_hook,@function\n"
    "exitos_syscall_hook:\n\t"
    /* Anything we do not claim is forwarded without building a frame at all.
     * This is not an optimisation, it is a correctness requirement: clone and
     * clone3 return TWICE, and the child resumes on a different stack, where
     * the frame the parent built does not exist. Returning through it popped
     * whatever happened to be on the child stack -- observed as a jump to
     * 0xffffffffffffffff on the first worker thread of any threaded program,
     * while every single-threaded test passed. Forwarding with a plain jump
     * leaves the stack exactly as the replaced `syscall` instruction found it,
     * so the child returns where it was going to return anyway. */
    "cmpq $511, %rdi\n\t"
    "ja 1f\n\t"
    "leaq exitos_claim_tbl(%rip), %rax\n\t"
    "cmpb $0, (%rax,%rdi,1)\n\t"
    "jne 2f\n"
    "1:\n\t"
    "movq exitos_bpftime_orig(%rip), %rax\n\t"
    "testq %rax, %rax\n\t"
    "je 2f\n\t"
    "jmp *%rax\n"
    "2:\n\t"
    /* Read the 7th argument while %rsp still points at the caller's frame,
     * then move %rsp clear of the 128-byte red zone before writing anything.
     * %r11 is free scratch: the instruction this shim replaces destroys it. */
    "movq 8(%rsp), %rax\n\t"
    "movq %rsp, %r11\n\t"
    "subq exitos_xstate_frame(%rip), %rsp\n\t"
    /* 64-byte alignment is XSAVE's requirement and keeps the area below
     * 64-aligned too; it also leaves %rsp 16-aligned for the call. */
    "andq $-64, %rsp\n\t"
    "movq %r11, 8(%rsp)\n\t"
    "movq %rax, (%rsp)\n\t"
    "movq %rax, 312(%rsp)\n\t"
    "movq %rsi, 272(%rsp)\n\t"
    "movq %rdx, 280(%rsp)\n\t"
    "movq %rcx, 288(%rsp)\n\t"
    "movq %r8,  296(%rsp)\n\t"
    "movq %r9,  304(%rsp)\n\t"
    /* Save the complete enabled xstate. XSAVE writes XSTATE_BV itself but not
     * the rest of the 64-byte header, so zero the header first. %rax and %rdx
     * carry the mask, so %rdx (the hook's third argument) is reloaded after. */
    "leaq " EXITOS_STR(EXITOS_XSTATE_AREA_OFF) "(%rsp), %r11\n\t"
    "cmpl $0, exitos_xstate_use(%rip)\n\t"
    "je 3f\n\t"
    "xorl %eax, %eax\n\t"
    "movq %rax, 512(%r11)\n\t"
    "movq %rax, 520(%r11)\n\t"
    "movq %rax, 528(%r11)\n\t"
    "movq %rax, 536(%r11)\n\t"
    "movq %rax, 544(%r11)\n\t"
    "movq %rax, 552(%r11)\n\t"
    "movq %rax, 560(%r11)\n\t"
    "movq %rax, 568(%r11)\n\t"
    "movl exitos_xstate_lo(%rip), %eax\n\t"
    "movl exitos_xstate_hi(%rip), %edx\n\t"
    "xsave (%r11)\n\t"
    "jmp 4f\n"
    "3:\n\t"
    "fxsave (%r11)\n"
    "4:\n\t"
    "movq 280(%rsp), %rdx\n\t"
    "call exitos_syscall_hook_c\n\t"
    /* Restore the xstate. The mask again goes in %eax:%edx, so the hook's
     * return value is parked in a frame slot across the restore. */
    "movq %rax, 320(%rsp)\n\t"
    "leaq " EXITOS_STR(EXITOS_XSTATE_AREA_OFF) "(%rsp), %r11\n\t"
    "cmpl $0, exitos_xstate_use(%rip)\n\t"
    "je 5f\n\t"
    "movl exitos_xstate_lo(%rip), %eax\n\t"
    "movl exitos_xstate_hi(%rip), %edx\n\t"
    "xrstor (%r11)\n\t"
    "jmp 6f\n"
    "5:\n\t"
    "fxrstor (%r11)\n"
    "6:\n\t"
    "movq 320(%rsp), %rax\n\t"
    /* Put back the register state a real `syscall` would have left: the six
     * argument registers hold the values that arrived here as arguments. */
    "movq 272(%rsp), %rdi\n\t"
    "movq 280(%rsp), %rsi\n\t"
    "movq 288(%rsp), %rdx\n\t"
    "movq 296(%rsp), %r10\n\t"
    "movq 304(%rsp), %r8\n\t"
    "movq 312(%rsp), %r9\n\t"
    "movq 8(%rsp), %rsp\n\t"
    "ret\n\t"
    ".size exitos_syscall_hook,.-exitos_syscall_hook\n\t");
#endif
int64_t exitos_syscall_hook(int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t, int64_t);

#ifdef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
static void internal_log(const char *buf, int n)
{
    if (!g_orig || !buf || n <= 0)
        return;
    (void)g_orig(SYS_write, STDERR_FILENO, (int64_t)(uintptr_t)buf,
                 (int64_t)(size_t)n, 0, 0, 0);
}

static void bpftime_prepare_maybe_arm(void)
{
    unsigned arrived = 0, expected = 0;
    char msg[192];
    int n;

    if (!exitos_frontend_config_prepare_arrive(g_config, &arrived, &expected))
        return;
    n = snprintf(msg, sizeof msg,
                 "PREPARE_STOP_ARMED v=1 frontend=bpftime arrived=%u "
                 "expected=%u stop_after=selected-close\n", arrived, expected);
    if (n >= (int)sizeof msg)
        n = (int)sizeof msg - 1;
    internal_log(msg, n);
}

static int bpftime_prepare_maybe_stop(void)
{
    unsigned expected;
    char msg[192];
    int n;

    if (!exitos_frontend_config_prepare_stop_take(g_config))
        return 0;
    expected = exitos_frontend_config_prepare_expected(g_config);
    n = snprintf(msg, sizeof msg,
                 "PREPARE_STOP_READY v=1 frontend=bpftime arrived=%u "
                 "expected=%u signal=SIGSTOP after=selected-close\n",
                 expected, expected);
    if (n >= (int)sizeof msg)
        n = (int)sizeof msg - 1;
    internal_log(msg, n);
    if (raise(SIGSTOP) != 0)
        return -(errno ? errno : EIO);
    n = snprintf(msg, sizeof msg,
                 "PREPARE_STOP_RELEASED v=1 frontend=bpftime arrived=%u "
                 "expected=%u signal=SIGCONT\n", expected, expected);
    if (n >= (int)sizeof msg)
        n = (int)sizeof msg - 1;
    internal_log(msg, n);
    return 0;
}

static void bpftime_admission_cleanup(int *admitted)
{
    if (*admitted)
        exitos_frontend_admission_leave();
}

struct bpftime_native_fallocate {
    int64_t a5;
    int64_t a6;
};

static int bpftime_native_fallocate_call(void *opaque, int fd, int mode,
                                         off_t off, off_t len)
{
    struct bpftime_native_fallocate *call = opaque;
    int64_t rc = g_orig(SYS_fallocate, fd, mode, (int64_t)off,
                        (int64_t)len, call->a5, call->a6);

    if (rc < 0)
        return (int)rc;
    return rc == 0 ? 0 : -EIO;
}
#endif

/* The registered hook is reached from a rewritten call site, not from a real
 * `syscall` instruction. The two differ in one way that silently corrupts data:
 * a `syscall` leaves the caller's 128-byte red zone intact, while a call into a
 * C function builds a stack frame right on top of it. Compilers keep live values
 * in the red zone across an inlined syscall -- gcc -O2 parked the length operand
 * of a pwrite loop there, and the second iteration then read a garbage length.
 * exitos_syscall_hook below is an assembly shim that steps over the red zone
 * before calling this function, so C code never touches it. */
int64_t exitos_syscall_hook_c(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                                   int64_t a4, int64_t a5, int64_t a6)
{
    /* Default is to pass: anything we do not claim, anything while re-entered,
     * and anything before the context exists goes straight to the real syscall. */
    atomic_fetch_add_explicit(&g_hook_entries, 1, memory_order_relaxed);
#ifndef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
    /* Defence in depth behind the empty claim table: no externally reachable
     * C entry point may revive takeover while the xstate contract is unmet. */
    return g_orig ? g_orig(nr, a1, a2, a3, a4, a5, a6) : -ENOSYS;
#else
    int admitted __attribute__((cleanup(bpftime_admission_cleanup))) =
        exitos_frontend_admission_enter();

    if (!admitted)
        return g_orig ? g_orig(nr, a1, a2, a3, a4, a5, a6) : -ENOSYS;
    if (g_noop == 1)                  /* diagnostic: entry/exit mechanics only */
        return g_orig(nr, a1, a2, a3, a4, a5, a6);
    if (g_noop == 2) {                /* + thread-local guard and the claim test */
        int c = exitos_bpftime_claims((long)nr);
        g_in_hook = 1; g_in_hook = 0; (void)c;
        return g_orig(nr, a1, a2, a3, a4, a5, a6);
    }
    if (g_noop == 3) {                /* + the mapping lookup, result discarded */
        ssize_t r0 = 0;
        g_in_hook = 1;
        if (g_ctx && nr == SYS_pwrite64)
            (void)exitos_on_write(g_ctx, (int)a1, (const void *)a2,
                                  (size_t)a3, (off_t)a4, &r0);
        g_in_hook = 0;
        return g_orig(nr, a1, a2, a3, a4, a5, a6);
    }
    if (!g_ctx || !exitos_bpftime_claims((long)nr))
        return g_orig(nr, a1, a2, a3, a4, a5, a6);
    if (g_in_hook) {
        int internal = 0;
        int64_t rr;
        if (g_ctl_depth > 0)
            return g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (nr == SYS_fdatasync) {
            internal = reentrant_internal_try_begin(nr, (int)a1);
            if (!internal) {
                exitos_ctx_poison(g_ctx);
                return -EIO;
            }
        } else if (nr == SYS_fsync) {
            /* There is no internal iopath fsync.  A signal-handler fsync
             * cannot settle debt owned by the interrupted raw transaction. */
            exitos_ctx_poison(g_ctx);
            return -EIO;
        } else if (nr == SYS_openat) {
            /* All library-owned opens use the strong direct-call API above.
             * A rewritten openat seen here is asynchronous application reentry. */
            exitos_ctx_poison(g_ctx);
            return -EIO;
        } else if (nr == SYS_ftruncate
#ifdef SYS_fallocate
                   || nr == SYS_fallocate
#endif
                   ) {
            /* Extent mutation cannot safely pass while a raw transaction is
             * interrupted: it may release the very blocks being written. */
            exitos_ctx_poison(g_ctx);
            return -EIO;
        } else if (reentrant_nr_mutates(nr, a2))
            internal = reentrant_internal_begin(nr, (int)a1);
        rr = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (internal)
            g_internal_wrapper_active = 0;
        return rr;
    }
    if (!g_fastpath && timed_data_or_sync_nr(nr))
        return g_orig(nr, a1, a2, a3, a4, a5, a6);
    atomic_fetch_add_explicit(&g_hook_claimed, 1, memory_order_relaxed);

    g_in_hook = 1;
    /* Format in userspace, then call the original write handler directly.
     * Exact-fd marker + wrapper had a signal-stealable gap before the wrapper
     * set its active bit. */
    if (g_trace) {
        char msg[192];
        int n = snprintf(msg, sizeof(msg),
                         "[exitos/bpftime] claim nr=%ld fd=%ld len=%ld off=%ld\n",
                         (long)nr, (long)a1, (long)a3, (long)a4);
        if (n >= (int)sizeof(msg))
            n = (int)sizeof(msg) - 1;
        internal_log(msg, n);
    }
    int64_t ret;
    ssize_t res = 0;
    int ires = 0;

    switch (nr) {
    case SYS_openat: {
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (ret >= 0 && exitos_frontend_config_path_selected(
                            g_config, (const char *)a2)) {
            int rrc;
            g_ctl_depth++;
            rrc = exitos_register_fd(g_ctx, (int)ret);
            g_ctl_depth--;
            if (rrc != 0)
                exitos_stat_inc(EXITOS_STAT_DECLINED);
            if (getenv("EXITOS_VERBOSE")) {
                char msg[512];
                int n = snprintf(msg, sizeof(msg),
                                 "[exitos/bpftime] register fd=%d %s -> %s (rc=%d)\n",
                                 (int)ret, (const char *)a2,
                                 rrc == 0 ? "FAST PATH" : "declined", rrc);
                if (n >= (int)sizeof(msg))
                    n = (int)sizeof(msg) - 1;
                internal_log(msg, n);
            }
        }
        break;
    }
    case SYS_fsync: {
        struct exitos_fd_txn tx;
        /* Claim fsync only to add the raw-data half; never take metadata away
         * from the filesystem. Raw syscall ABI already represents failures as
         * negative errno values, so no libc conversion belongs here. */
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        int raw_rc = exitos_txn_flush_raw_debt(&tx);
        int64_t kernel_rc = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (kernel_rc == 0)
            exitos_txn_note_kernel_sync(&tx);
        exitos_fd_txn_end(&tx);
        ret = raw_rc < 0 ? (int64_t)raw_rc : kernel_rc;
        break;
    }
    case SYS_pwrite64: {
        struct exitos_fd_txn tx;
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        if (exitos_fd_txn_registered(&tx) &&
            exitos_txn_on_write(&tx, (const void *)a2, (size_t)a3,
                                (off_t)a4, &res) == EXITOS_TAKEOVER)
            ret = (int64_t)res;                 /* skip the real syscall */
        else
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        exitos_fd_txn_end(&tx);
        break;
    }
    case SYS_writev:
    case SYS_pwritev:
#ifdef SYS_pwritev2
    case SYS_pwritev2:
#endif
    {
        struct exitos_fd_txn tx;

        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        /* These interfaces are PASS-only.  Publish kernel debt before the
         * syscall while the same-fd transaction is held; a concurrent sync
         * must not clear the old debt in the gap before this write executes. */
        if (exitos_fd_txn_registered(&tx))
            exitos_txn_note_kernel_write(&tx);
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        exitos_fd_txn_end(&tx);
        break;
    }
    case SYS_write: {
        struct exitos_fd_txn tx;
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        /* write() has no explicit offset, so we need the file position. Take it
         * through g_orig so this lookup does not re-enter the hook. */
        int64_t off = g_orig(SYS_lseek, a1, 0, SEEK_CUR, 0, 0, 0);
        if (off >= 0 && exitos_fd_txn_registered(&tx) &&
            exitos_txn_on_write(&tx, (const void *)a2, (size_t)a3,
                                (off_t)off, &res) == EXITOS_TAKEOVER) {
            (void)g_orig(SYS_lseek, a1, res, SEEK_CUR, 0, 0, 0);  /* advance it */
            ret = (int64_t)res;
        } else {
            if (off < 0)
                exitos_txn_note_kernel_write(&tx);
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        }
        exitos_fd_txn_end(&tx);
        break;
    }
    case SYS_fdatasync: {
        struct exitos_fd_txn tx;
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        if (exitos_fd_txn_registered(&tx) &&
            exitos_txn_on_fdatasync(&tx, &ires) == EXITOS_TAKEOVER) {
            ret = (int64_t)ires;
        } else {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            /* Same reporting the LD_PRELOAD path does. Without it the two
             * backends disagree on the same preallocated workload: LD_PRELOAD
             * answers fdatasync calls itself and this path answers none of
             * them, because a successful kernel fdatasync was never
             * reported and the flag recording the kernel's unpersisted data
             * stayed set for the life of the descriptor. */
            if (ret == 0)
                exitos_txn_note_kernel_sync(&tx);
        }
        exitos_fd_txn_end(&tx);
        break;
    }
    case SYS_ftruncate: {
        struct exitos_fd_txn tx;
        int raw_rc;
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        raw_rc = exitos_txn_flush_raw_debt(&tx);
        if (raw_rc < 0) {
            exitos_fd_txn_end(&tx);
            ret = raw_rc;
            break;
        }
        /* Runs after old raw debt is paid but before the truncation so cached mappings are invalidated:
         * a later write must not reuse an LBA the file no longer owns. */
        if (exitos_fd_txn_registered(&tx))
            (void)exitos_txn_on_ftruncate(&tx, (off_t)a2, &ires);
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (ret == 0)
            exitos_txn_note_kernel_metadata(&tx);
        exitos_fd_txn_end(&tx);
        break;
    }
#ifdef SYS_fallocate
    case SYS_fallocate: {
        struct exitos_frontend_fallocate_result result;
        struct bpftime_native_fallocate native = { .a5 = a5, .a6 = a6 };
        int rc;

        g_ctl_depth++;
        rc = exitos_frontend_fallocate_execute(
            g_fallocate_runtime, (int)a1, (int)a2, (off_t)a3, (off_t)a4,
            bpftime_native_fallocate_call, &native, &result);
        {
            char msg[512];
            int n = snprintf(
                msg, sizeof msg,
                "PREPARE_OUTCOME v=1 frontend=bpftime fd=%d mode=%d "
                "off=%lld len=%lld outcome=%s stage=%s rc=%d return_rc=%d "
                "return_errno=0 rdwr_alias=%u prepared=%llu chunks=%u\n",
                (int)a1, (int)a2, (long long)(off_t)a3,
                (long long)(off_t)a4,
                exitos_frontend_fallocate_outcome_name(result.outcome),
                exitos_frontend_fallocate_stage_name(result.stage), result.rc,
                rc, result.used_rdwr_alias ? 1U : 0U,
                (unsigned long long)result.prepared_bytes, result.chunks);
            if (n >= (int)sizeof msg)
                n = (int)sizeof msg - 1;
            internal_log(msg, n);
        }
        if (rc == 0 && result.prepared) {
            char msg[256];
            int n = snprintf(
                msg, sizeof msg,
                "PREPARE_READY frontend=bpftime fd=%d mode=%d off=%lld "
                "len=%lld prepared=%llu chunks=%u fiemap=safe "
                "unsafe_flags=0\n",
                (int)a1, (int)a2, (long long)(off_t)a3,
                (long long)(off_t)a4,
                (unsigned long long)result.prepared_bytes, result.chunks);
            if (n >= (int)sizeof msg)
                n = (int)sizeof msg - 1;
            internal_log(msg, n);
            bpftime_prepare_maybe_arm();
        }
        g_ctl_depth--;
        ret = rc;
        break;
    }
#endif
    case SYS_close: {
        struct exitos_fd_txn tx;
        int prep, selected;
        if (exitos_fd_txn_begin(g_ctx, (int)a1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        selected = exitos_fd_txn_registered(&tx);
        prep = exitos_fd_txn_prepare_forget(&tx);
        if (prep < 0) {
            exitos_fd_txn_end(&tx);
            ret = prep;
            break;
        }
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        /* Linux releases fd before reporting late EINTR/EIO; EBADF means this
         * fd-number registration was stale already. Never retain either. */
        (void)exitos_fd_txn_forget(&tx);
        exitos_fd_txn_end(&tx);
        if (selected) {
            int stop_rc = bpftime_prepare_maybe_stop();
            if (stop_rc < 0)
                ret = stop_rc;
        }
        break;
    }
#ifdef SYS_close_range
    case SYS_close_range: {
        int prep = exitos_ctx_flush_all_and_poison(g_ctx);
        if (prep < 0)
            ret = prep;
        else
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        break;
    }
#endif
    case SYS_dup: {
        struct exitos_alias_txn tx;
        int prep;
        if (exitos_alias_txn_begin(g_ctx, (int)a1, -1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        prep = exitos_alias_txn_prepare(&tx);
        if (prep < 0) {
            exitos_alias_txn_end(&tx);
            ret = prep;
            break;
        }
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (ret >= 0)
            exitos_alias_txn_commit(&tx);
        else
            exitos_alias_txn_abort(&tx);
        exitos_alias_txn_end(&tx);
        break;
    }
    case SYS_fcntl: {
        struct exitos_alias_txn tx;
        int prep;

        if ((int)a2 == F_SETFL) {
            struct exitos_fd_txn ftx;

            if (exitos_fd_txn_begin(g_ctx, (int)a1, &ftx) != 0) {
                ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
                break;
            }
            prep = exitos_fd_txn_prepare_forget(&ftx);
            if (prep < 0) {
                exitos_fd_txn_end(&ftx);
                ret = prep;
                break;
            }
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            if (ret == 0)
                (void)exitos_fd_txn_forget(&ftx);
            else
                exitos_fd_txn_abort_forget(&ftx);
            exitos_fd_txn_end(&ftx);
            break;
        }
        if ((int)a2 != F_DUPFD
#ifdef F_DUPFD_CLOEXEC
            && (int)a2 != F_DUPFD_CLOEXEC
#endif
            ) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        if (exitos_alias_txn_begin(g_ctx, (int)a1, -1, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        prep = exitos_alias_txn_prepare(&tx);
        if (prep < 0) {
            exitos_alias_txn_end(&tx);
            ret = prep;
            break;
        }
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (ret >= 0)
            exitos_alias_txn_commit(&tx);
        else
            exitos_alias_txn_abort(&tx);
        exitos_alias_txn_end(&tx);
        break;
    }
    case SYS_dup2:
    case SYS_dup3: {
        struct exitos_alias_txn tx;
        int prep;
        if ((int)a1 == (int)a2) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        if (exitos_alias_txn_begin(g_ctx, (int)a1, (int)a2, &tx) != 0) {
            ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
            break;
        }
        /* The fd number being overwritten stops meaning what it meant when it
         * was registered; a later write on it would be serviced against the
         * previous file's map and land on that file's blocks. */
        prep = exitos_alias_txn_prepare(&tx);
        if (prep < 0) {
            exitos_alias_txn_end(&tx);
            ret = prep;
            break;
        }
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        if (ret >= 0)
            exitos_alias_txn_commit(&tx);
        else
            exitos_alias_txn_abort(&tx);
        exitos_alias_txn_end(&tx);
        break;
    }
    default:
        ret = g_orig(nr, a1, a2, a3, a4, a5, a6);
        break;
    }

    g_in_hook = 0;
    return ret;
#endif
}

int exitos_bpftime_available(const char *so)
{
#ifndef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
    (void)so;
    return 0;
#else
    struct stat st;
    if (!so || !*so)
        return 0;
    return (stat(so, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
#endif
}

int exitos_bpftime_started(void) { return g_started; }

static void bpftime_release_frontend(void)
{
    exitos_frontend_fallocate_runtime_destroy(g_fallocate_runtime);
    g_fallocate_runtime = NULL;
    g_ctx = NULL;
    exitos_frontend_config_destroy(g_config);
    g_config = NULL;
    g_fastpath = 1;
}

int exitos_bpftime_start(const char *so)
{
#ifndef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
    /* Refuse before dlopen/setup: setup_syscall_tracer rewrites executable
     * text process-wide and is not safely reversible.
     *
     * The xstate half of the original reason is now met: the shim
     * saves and restores the complete XCR0-enabled state with a dynamically
     * sized XSAVE/XRSTOR (proven by tests/unit/test_bpftime_xstate.c on both a
     * plain-AVX host and an AVX-512+AMX host). What keeps this frozen is
     * upstream: bpftime returns clone/clone3 children through a `ret` on their
     * own fresh stack, so every threaded program dies. tools/bpftime-clone-fix.patch
     * fixes that and is verified on the test host, but it is a local patch that upstream
     * has not accepted, so the shipped default must not depend on it.
     *
     * libexitos_bpftime_active.so is the same sources built with
     * EXITOS_BPFTIME_TEST_DORMANT_DISPATCH, for measured end-to-end runs on a
     * host whose transformer carries that patch. */
    (void)so;
    return -EOPNOTSUPP;
#else
    void (*csreg)(void);
    void (*setup)(void);
    hook_fn (*getk)(void);
    void (*setk)(hook_fn);
    int config_rc;
    int install_hook;
    const char *noop_value;

    if (g_started)
        return 0;

    /* Snapshot and validate the same immutable configuration preload uses
     * before inspecting the optional transformer.  This gives malformed
     * EXITOS_* settings deterministic precedence and leaves backend 2 with no
     * adapter-local interpretation of those settings. */
    config_rc = exitos_frontend_config_from_env(&g_config);
    if (config_rc != 0)
        return config_rc;
    if (!exitos_frontend_config_enabled(g_config)) {
        exitos_frontend_config_destroy(g_config);
        g_config = NULL;
        return -1;
    }
    g_ctx = exitos_frontend_config_ctx(g_config); /* borrowed */
    g_fastpath = exitos_frontend_config_fastpath(g_config);
    config_rc = exitos_frontend_fallocate_runtime_create(
        &g_fallocate_runtime, g_config, g_ctx);
    if (config_rc != 0) {
        bpftime_release_frontend();
        return config_rc;
    }

    if (!exitos_bpftime_available(so)) {
        bpftime_release_frontend();
        return -1;
    }

    g_so = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!g_so) {
        bpftime_release_frontend();
        return -1;
    }

    /* Capstone must have x86 registered before the transformer disassembles
     * anything. The symbol carries frida's prefix, so looking it up under its
     * documented name returns NULL. */
    csreg = (void (*)(void))dlsym(g_so, "_frida_cs_arch_register_x86");
    setup = (void (*)(void))dlsym(g_so, "_ZN7bpftime20setup_syscall_tracerEv");
    getk  = (hook_fn (*)(void))dlsym(g_so, "_ZN7bpftime13get_call_hookEv");
    setk  = (void (*)(hook_fn))dlsym(g_so, "_ZN7bpftime13set_call_hookEPFllllllllE");
    if (!csreg || !setup || !getk || !setk) {
        dlclose(g_so); g_so = NULL;
        bpftime_release_frontend();
        return -1;
    }

    /* get_call_hook is initialized to the transformer's real-syscall handler
     * before setup.  Validate and retain it while failure is still reversible:
     * setup() rewrites executable text process-wide, after which dlclose would
     * leave every rewritten call targeting unmapped transformer code. */
    g_orig = getk();
    if (!g_orig) {
        bpftime_release_frontend();
        dlclose(g_so); g_so = NULL;
        return -1;
    }

    /* Freeze every value the hook may read before publishing the hook.  A
     * transformer can expose set_call_hook() to a process that already has
     * other threads (or a signal handler), so writing these ordinary ints
     * after setk() would be a C data race as well as a brief policy mismatch. */
    install_hook = getenv("EXITOS_BPFTIME_NOHOOK") == NULL;
    g_trace = getenv("EXITOS_TRACE") != NULL;
    noop_value = getenv("EXITOS_BPFTIME_NOOP");
    g_noop = noop_value ? atoi(noop_value) : 0;

    csreg();
    setup();                              /* needs root; exit(1)s if refused */
    /* Diagnostic: rewrite the syscall instructions but leave bpftime's own hook
     * in place. It is the only honest control for "does the rewriting itself
     * break this program", because loading the transformer alone does nothing
     * at all -- its agent entry point returns before it rewrites anything
    * unless AGENT_SO is set. */
    exitos_frontend_admission_enable();
    if (install_hook)
        setk(exitos_syscall_hook);
    g_started = 1;
    return 0;
#endif
}

unsigned long exitos_bpftime_entries(void)
{
    return atomic_load_explicit(&g_hook_entries, memory_order_relaxed);
}
unsigned long exitos_bpftime_claimed(void)
{
    return atomic_load_explicit(&g_hook_claimed, memory_order_relaxed);
}

void exitos_bpftime_stop(void)
{
    if (!g_started)
        return;
    /* Prevent a new hook from borrowing g_config/g_ctx, then wait until every
     * hook admitted before the close has returned.  The transformer still
     * points at our hook while draining; late calls see the closed gate and go
     * directly to g_orig without touching frontend-owned state. */
    exitos_frontend_admission_quiesce();
    if (getenv("EXITOS_VERBOSE"))
        fprintf(stderr, "[exitos/bpftime] hook entered %lu times, claimed %lu\n",
                atomic_load_explicit(&g_hook_entries, memory_order_relaxed),
                atomic_load_explicit(&g_hook_claimed, memory_order_relaxed));
    /* Put the original handler back before tearing down the context, so a
     * syscall arriving mid-teardown still reaches the kernel. */
    if (g_so) {
        void (*setk)(hook_fn) =
            (void (*)(hook_fn))dlsym(g_so, "_ZN7bpftime13set_call_hookEPFllllllllE");
        if (setk && g_orig)
            setk(g_orig);
    }
    bpftime_release_frontend();
    g_started = 0;
    /* The transformer is left loaded on purpose: it has rewritten this process's
     * text, and unloading it would leave those rewrites pointing at nothing. */
}
