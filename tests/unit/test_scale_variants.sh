#!/bin/bash
# The performance attribution campaign must compare one source snapshot with
# only the two intended compile-time switches changed.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
BUILDER=$ROOT/tools/build-scale-variants.sh
TMP=$(mktemp -d /tmp/exitos-scale-variants-test.XXXXXX) || exit 1
trap 'rm -rf -- "$TMP"' EXIT
n=0
bad=0
ok() { n=$((n + 1)); echo "ok $n - $1"; }
not_ok() { n=$((n + 1)); bad=$((bad + 1)); echo "not ok $n - $1"; }

if [ -x "$BUILDER" ]; then
    ok "A/B variant builder exists"
else
    not_ok "A/B variant builder exists"
fi

if [ -x "$BUILDER" ] && bash -n "$BUILDER" &&
   "$BUILDER" "$TMP/variants" >"$TMP/build.out" 2>"$TMP/build.err"; then
    ok "all four same-source variants build"
else
    not_ok "all four same-source variants build"
fi

names=(
    preload-table_global-stats_global.so
    preload-table_global-stats_sharded.so
    preload-table_sharded-stats_global.so
    preload-table_sharded-stats_sharded.so
)
hashes=()
for name in "${names[@]}"; do
    path=$TMP/variants/$name
    if [ -r "$path" ] && env -u EXITOS_FILES -u EXITOS_STATS \
        LD_PRELOAD="$path" /bin/true; then
        ok "$name is a loadable preload library"
        hashes+=("$(sha256sum "$path" | awk '{print $1}')")
    else
        not_ok "$name is a loadable preload library"
    fi
done

if [ "$(printf '%s\n' "${hashes[@]}" | sort -u | wc -l)" -eq 4 ]; then
    ok "the four macro combinations produce four distinct artifacts"
else
    not_ok "the four macro combinations produce four distinct artifacts"
fi

manifest=$TMP/variants/MANIFEST.txt
if [ -r "$manifest" ] &&
   rg -q 'EXITOS_TABLE_GLOBAL_BASELINE' "$manifest" &&
   rg -q 'EXITOS_STATS_GLOBAL_BASELINE' "$manifest" &&
   rg -q 'src/intercept.c' "$manifest" &&
   rg -q 'src/stats.c' "$manifest"; then
    ok "manifest records switches and source hashes"
else
    not_ok "manifest records switches and source hashes"
fi

snapshot=$TMP/variants/source-snapshot
if [ -d "$snapshot/src" ] && [ -d "$snapshot/include" ] &&
   [ -r "$snapshot/src/intercept.c" ] && [ -r "$snapshot/src/stats.c" ] &&
   ! find "$snapshot" -type l -print -quit | grep -q . &&
   ! find "$snapshot" -type f -perm /0222 -print -quit | grep -q .; then
    ok "variants retain a regular read-only source snapshot"
else
    not_ok "variants retain a regular read-only source snapshot"
fi

if [ -r "$manifest" ] && python3 - "$manifest" "$snapshot" <<'PY'
import hashlib,os,re,stat,sys
manifest,snapshot=sys.argv[1:]
lines=open(manifest,encoding="ascii").read().splitlines()
one=lambda prefix:[x[len(prefix):] for x in lines if x.startswith(prefix)]
before=one("snapshot_hash_before=")
after=one("snapshot_hash_after=")
if len(before)!=1 or len(after)!=1 or before[0]!=after[0]: raise SystemExit(1)
try:
    a=lines.index("source_hashes_begin")
    b=lines.index("source_hashes_end")
except ValueError: raise SystemExit(2)
records=lines[a+1:b]
if not records: raise SystemExit(3)
actual=[]
for root,dirs,files in os.walk(snapshot):
    dirs.sort(); files.sort()
    for name in files:
        p=os.path.join(root,name)
        st=os.lstat(p)
        if not stat.S_ISREG(st.st_mode): raise SystemExit(4)
        rel=os.path.relpath(p,snapshot)
        actual.append(hashlib.sha256(open(p,"rb").read()).hexdigest()+"  "+rel)
actual.sort(key=lambda x:x.split("  ",1)[1])
if records!=actual: raise SystemExit(5)
tree=hashlib.sha256(("\n".join(records)+"\n").encode()).hexdigest()
if tree!=before[0]: raise SystemExit(6)
PY
then
    ok "manifest pre/post hash binds the exact retained source snapshot"
else
    not_ok "manifest pre/post hash binds the exact retained source snapshot"
fi

if [ -r "$manifest" ] && python3 - "$manifest" <<'PY'
import re,sys
lines=open(sys.argv[1],encoding="ascii").read().splitlines()
expected={
 "preload-table_global-stats_global.so":("global","global"),
 "preload-table_global-stats_sharded.so":("global","sharded"),
 "preload-table_sharded-stats_global.so":("sharded","global"),
 "preload-table_sharded-stats_sharded.so":("sharded","sharded"),
}
seen={}
pat=re.compile(r"artifact=([^ ]+) table=(global|sharded) stats=(global|sharded) defs=([^ ]+) sha256=([0-9a-f]{64})")
for line in lines:
    if not line.startswith("artifact="): continue
    m=pat.fullmatch(line)
    if not m or m.group(1) in seen: raise SystemExit(1)
    seen[m.group(1)]=(m.group(2),m.group(3))
if seen!=expected: raise SystemExit(2)
PY
then
    ok "manifest has exact one-to-one membership for all four macro combinations"
else
    not_ok "manifest has exact one-to-one membership for all four macro combinations"
fi

runner=$ROOT/tools/fio-scale-pilot.sh
validate_out=$TMP/validate.out
if EXITOS_SCALE_TEST_MODE=1 bash "$runner" --test-variants "$TMP/variants" \
        >"$validate_out" 2>"$TMP/validate.err" &&
   grep -q '^VARIANTS_VALIDATED endpoints=gg,ss all=gg,gs,sg,ss$' "$validate_out"; then
    ok "runner independently validates snapshot, membership, and four artifact hashes"
else
    not_ok "runner independently validates snapshot, membership, and four artifact hashes"
fi

cp -a "$TMP/variants" "$TMP/tampered"
chmod u+w "$TMP/tampered/preload-table_global-stats_global.so"
printf X >>"$TMP/tampered/preload-table_global-stats_global.so"
if EXITOS_SCALE_TEST_MODE=1 bash "$runner" --test-variants "$TMP/tampered" \
        >"$TMP/tampered.out" 2>"$TMP/tampered.err"; then
    not_ok "runner refuses a post-manifest artifact mutation"
elif grep -q 'artifact hash mismatch' "$TMP/tampered.err"; then
    ok "runner refuses a post-manifest artifact mutation"
else
    not_ok "runner refuses a post-manifest artifact mutation"
fi

cp -a "$TMP/variants" "$TMP/extra-artifact"
cp "$TMP/extra-artifact/preload-table_global-stats_global.so" \
   "$TMP/extra-artifact/unmanifested.so"
chmod 0500 "$TMP/extra-artifact/unmanifested.so"
if EXITOS_SCALE_TEST_MODE=1 bash "$runner" --test-variants "$TMP/extra-artifact" \
        >"$TMP/extra.out" 2>"$TMP/extra.err"; then
    not_ok "runner refuses an unmanifested shared object"
elif grep -q 'artifact directory membership mismatch' "$TMP/extra.err"; then
    ok "runner refuses an unmanifested shared object"
else
    not_ok "runner refuses an unmanifested shared object"
fi

cp -a "$TMP/variants" "$TMP/false-defs"
chmod u+w "$TMP/false-defs/MANIFEST.txt"
sed -i 's/defs=EXITOS_TABLE_GLOBAL_BASELINE+EXITOS_STATS_GLOBAL_BASELINE/defs=none/' \
    "$TMP/false-defs/MANIFEST.txt"
chmod 0400 "$TMP/false-defs/MANIFEST.txt"
if EXITOS_SCALE_TEST_MODE=1 bash "$runner" --test-variants "$TMP/false-defs" \
        >"$TMP/defs.out" 2>"$TMP/defs.err"; then
    not_ok "runner refuses a manifest whose macro membership is false"
elif grep -q 'artifact defs membership mismatch' "$TMP/defs.err"; then
    ok "runner refuses a manifest whose macro membership is false"
else
    not_ok "runner refuses a manifest whose macro membership is false"
fi

if bash -c '
    lock=$(grep -n '\''chmod 0500.*SNAPSHOT'\'' "$1" | head -1 | cut -d: -f1)
    first_hash=$(grep -n '\''^SOURCE_HASHES_BEFORE='\'' "$1" | head -1 | cut -d: -f1)
    first_build=$(grep -n '\''^build_one global global'\'' "$1" | head -1 | cut -d: -f1)
    test -n "$lock" -a -n "$first_hash" -a -n "$first_build" &&
        test "$lock" -lt "$first_hash" -a "$lock" -lt "$first_build"
' bash "$BUILDER"; then
    ok "snapshot directories are non-writable before hashing and compilation"
else
    not_ok "snapshot directories are non-writable before hashing and compilation"
fi

marker_source=$TMP/marker.c
marker_decoy_source=$TMP/marker-decoy.c
marker_so=$TMP/marker.so
marker_decoy_so=$TMP/marker-decoy.so
marker_log=$TMP/marker.log
cat >"$marker_source" <<'EOF'
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
__attribute__((constructor)) static void mark(void) {
    const char *path = getenv("BOUND_ARTIFACT_MARKER");
    int fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    if (fd >= 0) { (void)write(fd, "ORIGINAL\n", 9); (void)close(fd); }
}
EOF
cat >"$marker_decoy_source" <<'EOF'
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
__attribute__((constructor)) static void mark(void) {
    const char *path = getenv("BOUND_ARTIFACT_MARKER");
    int fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    if (fd >= 0) { (void)write(fd, "DECOY\n", 6); (void)close(fd); }
}
EOF
"${CC:-cc}" -fPIC -shared -o "$marker_so" "$marker_source"
"${CC:-cc}" -fPIC -shared -o "$marker_decoy_so" "$marker_decoy_source"
chmod 0500 "$marker_so" "$marker_decoy_so"
marker_identity=$(python3 - "$marker_so" <<'PY'
import os,stat,sys
st=os.lstat(sys.argv[1])
print("%d:%d:%d:%o:%d:%d:%d:%d"%(
    st.st_dev,st.st_ino,st.st_uid,stat.S_IMODE(st.st_mode),st.st_nlink,
    st.st_size,st.st_mtime_ns,st.st_ctime_ns))
PY
)
artifact_command=$TMP/artifact-command
cat >"$artifact_command" <<'EOF'
#!/bin/bash
exec env LD_PRELOAD=/proc/self/fd/120 /bin/true
EOF
chmod 0700 "$artifact_command"
env EXITOS_SCALE_TEST_MODE=1 EXITOS_SCALE_TEST_PAUSE_AFTER_BOUND_FDS=1 \
    BOUND_ARTIFACT_MARKER="$marker_log" \
    bash "$runner" --test-bound-artifact "$marker_so" "$marker_identity" "$artifact_command" \
    >"$TMP/bound-artifact.out" 2>"$TMP/bound-artifact.err" & artifact_pid=$!
artifact_ready=0
for _ in $(seq 1 200); do
    if grep -qx BOUND_FDS_READY "$TMP/bound-artifact.out" 2>/dev/null; then
        artifact_ready=1
        break
    fi
    kill -0 "$artifact_pid" 2>/dev/null || break
    sleep 0.01
done
artifact_rc=0
if [ "$artifact_ready" -eq 1 ]; then
    artifact_child=$(ps --ppid "$artifact_pid" -o pid= | awk 'NR == 1 { print $1 }')
    mv "$marker_so" "$marker_so.retained"
    mv "$marker_decoy_so" "$marker_so"
    [ -n "$artifact_child" ] && kill -CONT "$artifact_child" 2>/dev/null || true
fi
wait "$artifact_pid" || artifact_rc=$?
if [ "$artifact_ready" -eq 1 ] && [ "$artifact_rc" -eq 0 ] &&
   [ "$(cat "$marker_log" 2>/dev/null)" = ORIGINAL ]; then
    ok "dynamic loader uses the retained artifact inode after pathname replacement"
else
    not_ok "dynamic loader uses the retained artifact inode after pathname replacement"
fi
if rg -q 'LD_PRELOAD=/proc/self/fd/120' "$runner"; then
    ok "production loader uses the retained artifact descriptor"
else
    not_ok "production loader uses the retained artifact descriptor"
fi

# A 0500 snapshot can still be renamed by way of its writable parent.  Pause
# the compiler after the pre-hash, swap in a decoy tree for all four builds,
# then restore the original before the post-hash.  The compiler must observe
# the retained snapshot inode, not whichever tree currently has its pathname.
race_stub_source=$TMP/race-stub.c
race_stub_so=$TMP/race-stub.so
printf '%s\n' 'int exits_scale_race_stub(void) { return 0; }' >"$race_stub_source"
"${CC:-cc}" -fPIC -shared -o "$race_stub_so" "$race_stub_source"
race_cc=$TMP/race-cc
cat >"$race_cc" <<'EOF'
#!/bin/bash
set -eu
if [ "${1:-}" = --version ]; then
    printf '%s\n' 'retained-snapshot test compiler 1'
    exit 0
fi
count=0
[ ! -f "$RACE_COUNTER" ] || read -r count <"$RACE_COUNTER"
count=$((count + 1))
printf '%s\n' "$count" >"$RACE_COUNTER"
preload=
output=
previous=
for argument in "$@"; do
    case "$argument" in */src/preload.c) preload=$argument ;; esac
    [ "$previous" != -o ] || output=$argument
    previous=$argument
done
[ -n "$preload" ] && [ -n "$output" ]
if [ "$count" -eq 1 ]; then
    : >"$RACE_FIRST_READY"
    kill -STOP "$$"
fi
sha256sum "$preload" | awk '{print $1}' >>"$RACE_SEEN"
cp -- "$RACE_STUB_SO" "$output"
if [ "$count" -eq 4 ]; then
    : >"$RACE_ALL_DONE"
    kill -STOP "$$"
fi
EOF
chmod 0700 "$race_cc"
race_out=$TMP/race-variants
race_original=$TMP/race-original-snapshot
race_decoy=$TMP/race-decoy-snapshot
race_counter=$TMP/race-counter
race_seen=$TMP/race-seen
race_first_ready=$TMP/race-first-ready
race_all_done=$TMP/race-all-done
original_preload_sha=$(sha256sum "$ROOT/src/preload.c" | awk '{print $1}')
env CC="$race_cc" RACE_COUNTER="$race_counter" RACE_SEEN="$race_seen" \
    RACE_FIRST_READY="$race_first_ready" RACE_ALL_DONE="$race_all_done" \
    RACE_STUB_SO="$race_stub_so" \
    "$BUILDER" "$race_out" >"$TMP/race-build.out" 2>"$TMP/race-build.err" &
race_builder_pid=$!
race_ready=0
for _ in $(seq 1 500); do
    if [ -e "$race_first_ready" ]; then race_ready=1; break; fi
    kill -0 "$race_builder_pid" 2>/dev/null || break
    sleep 0.01
done
if [ "$race_ready" -eq 1 ]; then
    race_child=$(ps --ppid "$race_builder_pid" -o pid= | awk 'NR == 1 { print $1 }')
    mv "$race_out/source-snapshot" "$race_original"
    cp -a "$race_original" "$race_out/source-snapshot"
    chmod u+w "$race_out/source-snapshot" "$race_out/source-snapshot/src" \
        "$race_out/source-snapshot/src/preload.c"
    printf '%s\n' '/* decoy snapshot */' >>"$race_out/source-snapshot/src/preload.c"
    chmod 0400 "$race_out/source-snapshot/src/preload.c"
    chmod 0500 "$race_out/source-snapshot/src" "$race_out/source-snapshot"
    [ -n "$race_child" ] && kill -CONT "$race_child" 2>/dev/null || true
fi
race_done=0
for _ in $(seq 1 500); do
    if [ -e "$race_all_done" ]; then race_done=1; break; fi
    kill -0 "$race_builder_pid" 2>/dev/null || break
    sleep 0.01
done
if [ "$race_done" -eq 1 ]; then
    race_child=$(ps --ppid "$race_builder_pid" -o pid= | awk 'NR == 1 { print $1 }')
    mv "$race_out/source-snapshot" "$race_decoy"
    mv "$race_original" "$race_out/source-snapshot"
    [ -n "$race_child" ] && kill -CONT "$race_child" 2>/dev/null || true
fi
race_build_rc=0
if [ "$race_done" -ne 1 ]; then
    kill "$race_builder_pid" 2>/dev/null || true
    race_child=$(ps --ppid "$race_builder_pid" -o pid= | awk 'NR == 1 { print $1 }')
    [ -z "$race_child" ] || kill -CONT "$race_child" 2>/dev/null || true
fi
wait "$race_builder_pid" || race_build_rc=$?
if [ "$race_ready" -eq 1 ] && [ "$race_done" -eq 1 ] &&
   [ "$race_build_rc" -eq 0 ] &&
   awk -v expected="$original_preload_sha" \
       'BEGIN { good=1 } { count++; if ($0 != expected) good=0 }
        END { exit !(good && count == 4) }' "$race_seen"; then
    ok "compiler reads the retained snapshot inode across pathname replacement"
else
    not_ok "compiler reads the retained snapshot inode across pathname replacement"
fi

echo "1..$n  ($bad failed)"
exit "$bad"
