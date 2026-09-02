/* Loader for backend 2. Production activation is currently hard-frozen in
 * exitos_bpftime_start(): this interposer reaches that function but it returns
 * -EOPNOTSUPP before dlopen or text rewriting. The historical sequencing notes
 * below document what must still be audited if activation is ever restored.
 *
 * Its job is to start the syscall-instruction rewriter at
 * the right moment, and the moment is the whole story.
 *
 * It used to run from a library constructor, on the reasoning that the dynamic
 * linker maps the executable and every DT_NEEDED library before constructors
 * run, so a scan then covers all of them. That reasoning is right about what is
 * mapped and wrong about when the rewriting is safe. bpftime's own agent does
 * not do it from a constructor: it interposes __libc_start_main, lets the real
 * one run, and rewrites just before main() is entered
 * (agent-transformer.cpp). Doing it from a constructor instead rewrote the
 * program while the C library was still initialising, and every threaded
 * program then died -- the first worker thread ended up issuing syscall number
 * -38 in a loop and jumping into libc's data segment. A single-threaded program
 * never noticed, which is why every test writer passed.
 *
 * So the same shape is used here: interpose __libc_start_main, hand it a
 * replacement main, and do the arming inside that replacement, immediately
 * before the program's own main runs. If for any reason __libc_start_main is
 * never reached -- a statically linked program, or one started in a way that
 * skips it -- the constructor below still arms as a fallback, because an
 * unarmed backend is a silent no-op and that is worse than the old ordering.
 *
 * The scan covers what is mapped at that moment and never rescans: anything
 * dlopen'd later needs its own call.
 *
 * Inert unless EXITOS_FILES names something, and silent unless EXITOS_VERBOSE.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_bpftime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "exitos_stats.h"

typedef int (*main_fn)(int, char **, char **);
typedef int (*libc_start_main_fn)(main_fn, int, char **, main_fn,
                                  void (*)(void), void (*)(void), void *);

static main_fn g_real_main;
static int     g_armed;

static void exitos_bpftime_arm(void)
{
    const char *so;
    int rc;

    if (g_armed)
        return;
    g_armed = 1;
    so = getenv("EXITOS_BPFTIME_SO");
    if (!so || !*so)
        so = EXITOS_BPFTIME_DEFAULT_SO;
    rc = exitos_bpftime_start(so);
    if (getenv("EXITOS_VERBOSE"))
        fprintf(stderr, "[exitos/bpftime] start(%s) -> %s\n", so,
                rc == 0 ? "ARMED (syscall instructions rewritten)"
                        : rc == -EOPNOTSUPP
                        ? "disabled (upstream bpftime returns clone/clone3 "
                          "children through a `ret` on their own fresh stack)"
                        : "inert (transformer unavailable, EXITOS_FILES unset, or not root)");
}

static int exitos_wrapped_main(int argc, char **argv, char **envp)
{
    exitos_bpftime_arm();
    return g_real_main(argc, argv, envp);
}

int __libc_start_main(main_fn main_fn_, int argc, char **argv, main_fn init,
                      void (*fini)(void), void (*rtld_fini)(void),
                      void *stack_end)
{
    libc_start_main_fn real =
        (libc_start_main_fn)dlsym(RTLD_NEXT, "__libc_start_main");

    if (!real) {                        /* nothing sensible left to do */
        exitos_bpftime_arm();
        return main_fn_ ? main_fn_(argc, argv, NULL) : 1;
    }
    g_real_main = main_fn_;
    return real(exitos_wrapped_main, argc, argv, init, fini, rtld_fini,
                stack_end);
}

/* Fallback only: if __libc_start_main above was reached, this has already run
 * by the time the program's main is entered and does nothing. */
__attribute__((destructor)) static void exitos_bpftime_late_arm_check(void)
{
    /* nothing: arming is done from the replacement main */
}

__attribute__((destructor)) static void exitos_bpftime_unloader(void)
{
    /* Publish the counters the same way backend 1 does. Without this the two
     * backends cannot be compared at all: a run that intercepted nothing and a
     * run that never reported look identical from outside the process. */
    const char *sp = getenv("EXITOS_STATS");
    if (sp)
        (void)exitos_stats_dump(sp);
    exitos_bpftime_stop();
}
