/* Does bpftime's `syscall; ret` trampoline shape mis-return in a clone child?
 *
 * bpftime rewrites every `syscall` instruction into `call *%rax`, which lands
 * (via a nop slide on a mapped page zero) in syscall_hooker_asm, which calls
 * C, which calls call_orig_syscall -- and call_orig_syscall is literally
 * `syscall; ret`.  For clone the kernel returns TWICE: the child resumes at
 * that `ret`, but on the child's brand-new stack, where no return address was
 * ever pushed.  This reproduces exactly that shape with no bpftime present.
 *
 * The child stack is primed with the address of child_landed, so if the child
 * really does `ret` off its own fresh stack the jump is observable instead of
 * being a wild branch. */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The exact shape of bpftime's call_orig_syscall: shuffle to syscall ABI,
 * execute the syscall, and return to the caller. */
__asm__(
    ".text\n\t"
    ".globl orig_syscall_shape\n\t"
    ".type orig_syscall_shape,@function\n"
    "orig_syscall_shape:\n\t"
    "movq %rdi, %rax\n\t"
    "movq %rsi, %rdi\n\t"
    "movq %rdx, %rsi\n\t"
    "movq %rcx, %rdx\n\t"
    "movq %r8, %r10\n\t"
    "movq %r9, %r8\n\t"
    "syscall\n\t"
    "ret\n\t"
    ".size orig_syscall_shape,.-orig_syscall_shape\n\t");
extern long orig_syscall_shape(long nr, long a1, long a2, long a3,
                               long a4, long a5);

static volatile int *g_flag;

/* Reached only if the child's `ret` popped this address off the fresh stack. */
__attribute__((noreturn)) static void child_landed(void)
{
    *g_flag = 1;
    syscall(SYS_exit, 0);
    __builtin_unreachable();
}

int main(void)
{
    size_t stack_size = 256 * 1024;
    unsigned char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    void *flag = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    unsigned char *top;
    long rc;
    int status = 0;

    if (stack == MAP_FAILED || flag == MAP_FAILED) {
        printf("mmap failed\n");
        return 2;
    }
    g_flag = flag;
    *g_flag = 0;

    /* Prime the child's stack so that a `ret` executed there is observable:
     * the popped value is the address of child_landed. */
    top = stack + stack_size;
    top -= 16;
    memcpy(top, &(void *){ (void *)child_landed }, sizeof(void *));

    /* CLONE_VM so the child shares the flag page; SIGCHLD so wait() works. */
    rc = orig_syscall_shape(SYS_clone, CLONE_VM | SIGCHLD, (long)(intptr_t)top,
                            0, 0, 0);
    if (rc < 0) {
        printf("clone failed rc=%ld\n", rc);
        return 2;
    }
    /* Parent only. */
    if (waitpid((pid_t)rc, &status, 0) < 0)
        printf("waitpid failed\n");

    printf("child exited: exited=%d status=%d signalled=%d sig=%d\n",
           WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    printf("child returned through the trampoline's `ret` onto its own "
           "fresh stack: %s\n", *g_flag ? "YES" : "no");
    return 0;
}
