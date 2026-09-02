/* Minimal assertion framework. Tests print TAP-ish lines and return nonzero
 * on failure so the Makefile can gate on them. */
#ifndef EXITOS_TAP_H
#define EXITOS_TAP_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   /* geteuid: implicit decl is an ERROR on gcc >= 14 */
static int _t_run, _t_fail;
#define T_OK(cond, ...) do { _t_run++; if (cond) { printf("ok %d - ", _t_run); printf(__VA_ARGS__); printf("\n"); } \
    else { _t_fail++; printf("not ok %d - ", _t_run); printf(__VA_ARGS__); printf("  [%s:%d]\n", __FILE__, __LINE__); } } while (0)
#define T_EQ(a, b, ...)  T_OK((long long)(a) == (long long)(b), __VA_ARGS__)
#define T_SKIP(...) do { printf("# SKIP - "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define T_DONE() do { printf("1..%d  (%d failed)\n", _t_run, _t_fail); return _t_fail ? 1 : 0; } while (0)
/* Guard for tests that need root + loop devices. */
static inline int t_need_root(void) { return geteuid() == 0; }
#endif
