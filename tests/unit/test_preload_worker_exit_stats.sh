#!/bin/bash
# The process-mode fio workers terminate through _exit/_Exit, so the preload
# needs an explicit runner-only cold-path stats dump before those exits.
set -u

tmp=$(mktemp -d /tmp/exitos-worker-exit-stats.XXXXXX) || exit 1
trap 'rm -rf -- "$tmp"' EXIT
n=0
bad=0
ok() { n=$((n + 1)); echo "ok $n - $1"; }
not_ok() { n=$((n + 1)); bad=$((bad + 1)); echo "not ok $n - $1"; }

cat >"$tmp/fork-exit.c" <<'EOF'
#define _GNU_SOURCE
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    void (*increment)(int) = dlsym(RTLD_DEFAULT, "exitos_stat_inc");
    if (!increment) return 4;
    pid_t children[8];
    for (int i = 0; i < 8; ++i) {
        children[i] = fork();
        if (children[i] < 0) return 2;
        if (children[i] == 0) {
            increment(0);
            if (i & 1) _Exit(0);
            _exit(0);
        }
    }
    for (int i = 0; i < 8; ++i) {
        int status = 0;
        if (waitpid(children[i], &status, 0) != children[i] ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 3;
    }
    return 0;
}
EOF

cat >"$tmp/mutate-exit.c" <<'EOF'
#define _GNU_SOURCE
#include <stdlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    void (*increment)(int) = dlsym(RTLD_DEFAULT, "exitos_stat_inc");
    if (!increment || argc != 4) return 4;
    increment(0);
    if (argv[3][0] == 'R' || argv[3][0] == 'S') {
        const char *templ = getenv("EXITOS_STATS");
        const char *mark = strstr(templ ? templ : "", "%p");
        char destination[4096];
        if (!mark || snprintf(destination, sizeof destination, "%.*s%ld%s",
                              (int)(mark-templ), templ, (long)getpid(), mark+2) <= 0)
            return 7;
        if (argv[3][0] == 'R') {
            int fd = open(destination, O_WRONLY|O_CREAT|O_EXCL, 0600);
            if (fd < 0 || write(fd, "DO_NOT_OVERWRITE\n", 17) != 17) return 8;
            close(fd);
        } else if (symlink(argv[2], destination) != 0) return 9;
    }
    if (setenv("EXITOS_STATS_ON_WORKER_EXIT", argv[1], 1) != 0) return 5;
    if (argv[3][0] != 'R' && argv[3][0] != 'S' &&
            setenv("EXITOS_STATS", argv[2], 1) != 0) return 6;
    if (argv[3][0] == 'E') _Exit(0);
    _exit(0);
}
EOF

cat >"$tmp/stdio-lock.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static FILE *(*real_fopen)(const char *,const char *);
__attribute__((constructor)) static void init(void) {
    real_fopen=dlsym(RTLD_NEXT,"fopen");
}
void hold_fopen_lock(void) { pthread_mutex_lock(&lock); }
FILE *fopen(const char *path,const char *mode) {
    pthread_mutex_lock(&lock);
    FILE *result=real_fopen(path,mode);
    pthread_mutex_unlock(&lock);
    return result;
}
EOF
cat >"$tmp/locked-fork.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    void (*hold)(void)=dlsym(RTLD_DEFAULT,"hold_fopen_lock");
    void (*increment)(int)=dlsym(RTLD_DEFAULT,"exitos_stat_inc");
    if (!hold || !increment) return 4;
    hold();
    pid_t child=fork();
    if (child < 0) return 5;
    if (child == 0) { increment(0); _exit(0); }
    int status=0;
    if (waitpid(child,&status,0) != child) return 6;
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}
EOF
cat >"$tmp/racing-exit.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
static pthread_barrier_t barrier;
static void *run(void *unused) {
    (void)unused;
    void (*increment)(int)=dlsym(RTLD_DEFAULT,"exitos_stat_inc");
    if (!increment) _exit(90);
    increment(0);
    pthread_barrier_wait(&barrier);
    _exit(0);
}
int main(void) {
    pthread_t threads[8];
    if (pthread_barrier_init(&barrier,0,8) != 0) return 2;
    for (int i=0;i<8;i++) if (pthread_create(&threads[i],0,run,0) != 0) return 3;
    for (;;) pause();
}
EOF

if cc -O2 -Wall -Wextra "$tmp/fork-exit.c" -o "$tmp/fork-exit" -ldl; then
    ok "compiled real fork plus _exit/_Exit fixture"
else
    not_ok "compiled real fork plus _exit/_Exit fixture"
fi
if cc -O2 -Wall -Wextra "$tmp/mutate-exit.c" -o "$tmp/mutate-exit" -ldl; then
    ok "compiled environment-mutation _exit fixture"
else
    not_ok "compiled environment-mutation _exit fixture"
fi
if cc -O2 -Wall -Wextra -fPIC -shared "$tmp/stdio-lock.c" \
        -o "$tmp/stdio-lock.so" -ldl -lpthread &&
   cc -O2 -Wall -Wextra "$tmp/locked-fork.c" -o "$tmp/locked-fork" -ldl &&
   cc -O2 -Wall -Wextra "$tmp/racing-exit.c" -o "$tmp/racing-exit" -ldl -lpthread; then
    ok "compiled stdio-lock and concurrent-exit fixtures"
else
    not_ok "compiled stdio-lock and concurrent-exit fixtures"
fi

optin=$tmp/optin
mkdir -m 0700 "$optin"
if env EXITOS_STATS="$optin/stats-%p.txt" EXITOS_STATS_ON_WORKER_EXIT=1 \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/fork-exit"; then
    files=("$optin"/stats-*.txt)
    if [ "${#files[@]}" -eq 8 ] &&
       awk 'FNR == 1 { if ($0 != 1) exit 1 } FNR > 1 && FNR <= 5 { if ($0 != 0) exit 1 } FNR == 5 { complete++ } END { exit !(complete == 8) }' \
           "${files[@]}"; then
        ok "opt-in emits exactly eight nonzero worker dumps and omits zero parent"
    else
        not_ok "opt-in emits exactly eight nonzero worker dumps and omits zero parent"
    fi
else
    not_ok "opt-in emits exactly eight nonzero worker dumps and omits zero parent"
fi

locked=$tmp/locked
mkdir -m 0700 "$locked"
if timeout 3 env EXITOS_STATS_ON_WORKER_EXIT=1 \
        EXITOS_STATS="$locked/stats-%p.txt" \
        LD_PRELOAD="$tmp/stdio-lock.so:$PWD/libexitos_preload.so" \
        "$tmp/locked-fork" &&
   test "$(find "$locked" -name 'stats-*.txt' -type f | wc -l)" -eq 1; then
    ok "worker raw dump cannot deadlock on a stdio lock inherited across fork"
else
    not_ok "worker raw dump cannot deadlock on a stdio lock inherited across fork"
fi

racing=$tmp/racing
mkdir -m 0700 "$racing"
if timeout 3 env EXITOS_STATS_ON_WORKER_EXIT=1 \
        EXITOS_STATS="$racing/stats-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/racing-exit" &&
   test "$(find "$racing" -name 'stats-*.txt' -type f | wc -l)" -eq 1 &&
   test "$(sed -n '1p' "$racing"/stats-*.txt)" -eq 8 &&
   test "$(wc -l < "$racing"/stats-*.txt)" -eq 5; then
    ok "concurrent signal-style exit reentry emits one exact five-line dump"
else
    not_ok "concurrent signal-style exit reentry emits one exact five-line dump"
fi

cached_off=$tmp/cached-off
mkdir -m 0700 "$cached_off"
if env -u EXITOS_STATS_ON_WORKER_EXIT EXITOS_STATS="$cached_off/original-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/mutate-exit" 1 \
        "$cached_off/mutated-%p.txt" x; then
    files=("$cached_off"/stats-*.txt "$cached_off"/original-*.txt \
           "$cached_off"/mutated-*.txt)
    if [ "${files[*]}" = "$cached_off/stats-*.txt $cached_off/original-*.txt $cached_off/mutated-*.txt" ]; then
        ok "constructor-cached disabled opt-in ignores later environment mutation"
    else
        not_ok "constructor-cached disabled opt-in ignores later environment mutation"
    fi
else
    not_ok "constructor-cached disabled opt-in ignores later environment mutation"
fi

cached_on=$tmp/cached-on
mkdir -m 0700 "$cached_on"
if env EXITOS_STATS_ON_WORKER_EXIT=1 EXITOS_STATS="$cached_on/original-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/mutate-exit" 0 \
        "$cached_on/mutated-%p.txt" E; then
    original=("$cached_on"/original-*.txt)
    mutated=("$cached_on"/mutated-*.txt)
    if [ "${#original[@]}" -eq 1 ] && [ -e "${original[0]}" ] &&
       [ ! -e "${mutated[0]}" ]; then
        ok "constructor-cached immutable stats template ignores later environment mutation"
    else
        not_ok "constructor-cached immutable stats template ignores later environment mutation"
    fi
else
    not_ok "constructor-cached immutable stats template ignores later environment mutation"
fi

collision=$tmp/collision
mkdir -m 0700 "$collision"
if env EXITOS_STATS_ON_WORKER_EXIT=1 EXITOS_STATS="$collision/stats-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/mutate-exit" 1 \
        "$collision/ignored-%p.txt" R &&
   grep -qx DO_NOT_OVERWRITE "$collision"/stats-*.txt; then
    ok "worker exit dump refuses a pre-existing regular destination"
else
    not_ok "worker exit dump refuses a pre-existing regular destination"
fi

symlink=$tmp/symlink
mkdir -m 0700 "$symlink"
printf '%s\n' DO_NOT_OVERWRITE >"$symlink/target"
if env EXITOS_STATS_ON_WORKER_EXIT=1 EXITOS_STATS="$symlink/stats-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/mutate-exit" 1 \
        "$symlink/target" S &&
   grep -qx DO_NOT_OVERWRITE "$symlink/target"; then
    ok "worker exit dump refuses a pre-existing symlink destination"
else
    not_ok "worker exit dump refuses a pre-existing symlink destination"
fi

default=$tmp/default
mkdir -m 0700 "$default"
if env -u EXITOS_STATS_ON_WORKER_EXIT EXITOS_STATS="$default/stats-%p.txt" \
        LD_PRELOAD="$PWD/libexitos_preload.so" "$tmp/fork-exit"; then
    files=("$default"/stats-*.txt)
    if [ "${#files[@]}" -eq 1 ]; then
        ok "default behavior remains destructor-only in the parent"
    else
        not_ok "default behavior remains destructor-only in the parent"
    fi
else
    not_ok "default behavior remains destructor-only in the parent"
fi

echo "1..$n  ($bad failed)"
exit "$bad"
