/* The rewritten instruction stands in for `syscall`, which preserves every
 * vector register.  A call into C does not: AVX upper halves are caller-saved.
 * Drive the real assembly shim while a claimed dispatch deliberately executes
 * VZEROALL, so saving only XMM0..15 is observably insufficient.  The safe
 * configuration is therefore an unclaimed direct jump until the shim preserves
 * the complete enabled xstate.  No syscall and no device I/O is performed. */
#include "tap.h"
#include "exitos_intercept.h"

#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>

#if defined(__x86_64__)

static int g_txn_begin_calls;

int avx_test_txn_begin(struct exitos_ctx *ctx, int fd,
                       struct exitos_fd_txn *tx);

#define exitos_fd_txn_begin avx_test_txn_begin
#include "../../src/bpftime_hook.c"
#undef exitos_fd_txn_begin

__attribute__((target("avx"), noinline))
int avx_test_txn_begin(struct exitos_ctx *ctx, int fd,
                       struct exitos_fd_txn *tx)
{
    (void)ctx;
    (void)fd;
    (void)tx;
    g_txn_begin_calls++;
    __asm__ volatile("vzeroall" ::: "memory");
    return -1;
}

/* The original-handler stand-in intentionally touches no vector register. */
__asm__(
    ".text\n\t"
    ".type avx_test_orig,@function\n"
    "avx_test_orig:\n\t"
    "movq $17, %rax\n\t"
    "ret\n\t"
    ".size avx_test_orig,.-avx_test_orig\n\t");
extern int64_t avx_test_orig(int64_t, int64_t, int64_t, int64_t,
                             int64_t, int64_t, int64_t);

#define STR1(x) #x
#define STR(x) STR1(x)

/* Keep load, call, and store in one assembly routine: a normal C call is
 * allowed to clobber YMM0 itself and could not serve as an oracle. */
__asm__(
    ".section .rodata\n\t"
    ".p2align 5\n"
    "avx_test_pattern:\n\t"
    ".byte 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07\n\t"
    ".byte 0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f\n\t"
    ".byte 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17\n\t"
    ".byte 0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f\n\t"
    ".text\n\t"
    ".type avx_test_probe,@function\n"
    "avx_test_probe:\n\t"
    "pushq %rbx\n\t"
    "movq %rdi, %rbx\n\t"
    /* Entry is 8 mod 16; push makes it aligned. Reserve one stack argument
     * plus padding while retaining pre-call 16-byte alignment. */
    "subq $16, %rsp\n\t"
    "movq $0, (%rsp)\n\t"
    "vmovdqa avx_test_pattern(%rip), %ymm0\n\t"
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
    ".size avx_test_probe,.-avx_test_probe\n\t");
extern void avx_test_probe(unsigned char out[32]);

int main(void)
{
    static const unsigned char want[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    unsigned char got[32];

    if (!__builtin_cpu_supports("avx")) {
        T_SKIP("CPU/OS has no AVX state; deterministic YMM contract test unavailable");
        T_DONE();
    }

    memset(got, 0xa5, sizeof(got));
    g_ctx = (struct exitos_ctx *)(uintptr_t)1;
    g_orig = avx_test_orig;
    g_txn_begin_calls = 0;
    avx_test_probe(got);

    T_EQ(exitos_claim_tbl[SYS_pwrite64], 0,
         "unsafe bpftime assembly takeover is hard-frozen at the claim table");
    T_EQ(exitos_bpftime_claims(SYS_pwrite64), 0,
         "public backend contract reports no takeover while xstate is unsafe");
    T_EQ(g_txn_begin_calls, 0,
         "hard freeze jumps directly to original handler without entering C");
    T_OK(memcmp(got, want, sizeof(got)) == 0,
         "bpftime forwarding preserves all 256 bits of a live YMM register");
    T_DONE();
}

#else

int main(void)
{
    T_SKIP("bpftime instruction-rewriting backend is x86-64-only");
    T_DONE();
}

#endif
