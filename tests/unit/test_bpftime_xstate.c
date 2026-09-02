/* The xstate contract that currently keeps backend 2 hard-frozen.
 *
 * The rewritten call site stands in for a `syscall` INSTRUCTION.  A syscall
 * clobbers only rcx and r11; every vector register survives it, so a compiler
 * is entitled to keep live vector values across one.  A `call` into C obeys the
 * System V AMD64 function ABI instead, under which xmm/ymm/zmm are all
 * call-clobbered.  Saving xmm0..15 with `movaps` captures only the low 128 bits
 * of each register, so a hook that executes any VEX-encoded instruction --
 * VZEROALL here, but in production simply calling into libc's AVX memcpy --
 * destroys the upper halves with nothing to restore them from.
 *
 * Unlike test_bpftime_avx.c, which pins the frozen configuration (claim table
 * empty, so the shim tail-jumps and never enters C), this test drives the
 * dispatching configuration: the claim table is populated and the C hook runs.
 * It is the acceptance test for making that configuration safe.
 *
 * No syscall and no device I/O is performed: the original-handler stand-in is
 * an assembly stub that returns a constant. */
#include "tap.h"
#include "exitos_frontend_admission.h"
#include "exitos_intercept.h"

#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>

#if defined(__x86_64__)

static int g_hook_c_calls;

int xstate_test_txn_begin(struct exitos_ctx *ctx, int fd,
                          struct exitos_fd_txn *tx);

/* Populate the claim table and run the real dispatcher, so the assembly shim
 * takes its framed path into C instead of the frozen tail jump. */
#define EXITOS_BPFTIME_TEST_DORMANT_DISPATCH 1
#define exitos_fd_txn_begin xstate_test_txn_begin
#include "../../src/bpftime_hook.c"
#undef exitos_fd_txn_begin
#undef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH

/* Stands in for the interception core.  Returning non-zero means "this fd is
 * not registered", so the hook falls through to the original handler; the
 * VZEROALL models any VEX-encoded instruction the real core or the libc
 * routines it calls would execute. */
__attribute__((target("avx"), noinline))
int xstate_test_txn_begin(struct exitos_ctx *ctx, int fd,
                          struct exitos_fd_txn *tx)
{
    (void)ctx;
    (void)fd;
    (void)tx;
    g_hook_c_calls++;
    __asm__ volatile("vzeroall" ::: "memory");
    return -1;
}

/* The original-handler stand-in touches no vector register, so anything the
 * probe observes came from the shim, not from here. */
__asm__(
    ".text\n\t"
    ".type xstate_test_orig,@function\n"
    "xstate_test_orig:\n\t"
    "movq $17, %rax\n\t"
    "ret\n\t"
    ".size xstate_test_orig,.-xstate_test_orig\n\t");
extern int64_t xstate_test_orig(int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t);

#define STR1(x) #x
#define STR(x) STR1(x)

/* Load, call and store in one assembly routine: an ordinary C call is itself
 * allowed to clobber ymm0 and could not serve as the oracle. */
__asm__(
    ".section .rodata\n\t"
    ".p2align 5\n"
    "xstate_test_pattern:\n\t"
    ".byte 0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27\n\t"
    ".byte 0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f\n\t"
    ".byte 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37\n\t"
    ".byte 0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f\n\t"
    ".text\n\t"
    ".type xstate_test_probe,@function\n"
    "xstate_test_probe:\n\t"
    "pushq %rbx\n\t"
    "movq %rdi, %rbx\n\t"
    /* Entry is 8 mod 16; the push aligns it.  Reserve one stack argument slot
     * plus padding while keeping 16-byte alignment at the call. */
    "subq $16, %rsp\n\t"
    "movq $0, (%rsp)\n\t"
    "vmovdqa xstate_test_pattern(%rip), %ymm0\n\t"
    "movq $" STR(SYS_pwrite64) ", %rdi\n\t"
    "movq $71, %rsi\n\t"
    "movq $0, %rdx\n\t"
    "movq $4096, %rcx\n\t"
    "movq $0, %r8\n\t"
    "movq $0, %r9\n\t"
    "call exitos_syscall_hook\n\t"
    "vmovdqu %ymm0, (%rbx)\n\t"
    "vzeroupper\n\t"
    "addq $16, %rsp\n\t"
    "popq %rbx\n\t"
    "ret\n\t"
    ".size xstate_test_probe,.-xstate_test_probe\n\t");
extern void xstate_test_probe(unsigned char out[32]);

int main(void)
{
    static const unsigned char want[32] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    };
    unsigned char got[32];

    if (!__builtin_cpu_supports("avx")) {
        T_SKIP("CPU/OS has no AVX state; deterministic YMM contract test unavailable");
        T_DONE();
    }

    /* The claim table is filled by a constructor in the included source. */
    T_EQ(exitos_claim_tbl[SYS_pwrite64], 1,
         "the dispatching configuration claims pwrite64");

    memset(got, 0xa5, sizeof(got));
    g_ctx = (struct exitos_ctx *)(uintptr_t)1;
    g_orig = xstate_test_orig;
    g_hook_c_calls = 0;
    /* Production enables admission only after publishing g_ctx and installing
     * the transformer.  This white-box fixture publishes those globals by
     * hand, so it must mirror that final lifecycle transition explicitly. */
    exitos_frontend_admission_enable();
    xstate_test_probe(got);
    exitos_frontend_admission_quiesce();

    T_EQ(g_hook_c_calls, 1,
         "a claimed syscall really entered the C hook through the shim");
    T_OK(memcmp(got, want, sizeof(got)) == 0,
         "the shim preserves all 256 bits of a live YMM register across the "
         "C hook");
    T_DONE();
}

#else

int main(void)
{
    T_SKIP("bpftime instruction-rewriting backend is x86-64-only");
    T_DONE();
}

#endif
