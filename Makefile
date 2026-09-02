CC      ?= gcc
# -std=gnu17 is pinned deliberately. gcc 15 defaults to gnu23, where a repeated
# file-scope declaration of a thread-local object is a redefinition error rather
# than a tentative definition that merges. tests/unit/test_bpftime_backend.c
# relies on that merging: it declares g_ctl_depth so the code above line 578 can
# use it, then includes src/bpftime_hook.c, which declares the same object under
# EXITOS_BPFTIME_TEST_DORMANT_DISPATCH. Without the pin the tree builds on gcc 11
# and fails on gcc 15.
STD     ?= -std=gnu17
CFLAGS  ?= $(STD) -O2 -g -Wall -Wextra -D_GNU_SOURCE -Iinclude -Itests/harness
LDFLAGS ?= -lpthread
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard include/*.h)
LOADERS   := src/preload.c src/bpftime_loader.c src/bpftime_hook.c
SRC_COMMON := $(filter-out $(LOADERS),$(SRC))
OBJ     := $(SRC_COMMON:.c=.o)
# The static library must NOT carry the interposers. It did, and every program
# that linked it for the ctx API silently had its own open/write/pwrite/close
# replaced, gained two constructors that armed interception, and gained a
# destructor that overwrote EXITOS_STATS -- so a test binary that links the
# library to build its own oracle became an unannounced interceptor, and two
# destructors raced to write the same counters file. The loaders belong only in
# the .so each one defines, which is the rule the two .so targets already follow.
LIB     := libexitos.a
UNIT    := $(patsubst %.c,%,$(wildcard tests/unit/test_*.c))
UNIT_HELPERS := tests/unit/bpftime_dso_contract_helper \
	tests/unit/fortify_open_helper tests/unit/largefile_truncate_helper
UNIT_SH := $(wildcard tests/unit/test_*.sh)
# Only test_*.c are tests. The directory also holds writer helpers that the
# tests execute as child processes; running those as tests made them print
# their usage and exit non-zero, so `make test` could never report success.
INTEG := $(patsubst %.c,%,$(wildcard tests/integ/test_*.c))
DEVICE  := $(patsubst %.c,%,$(wildcard tests/device/*.c))
READBACK_PATTERN := tests/integ/readback_pattern_contract
FALLOCATE_PREPARE_PROBE := tests/integ/fallocate_prepare_probe
URING_QD_DIR := tests/uring-passthrough-qd
URING_QD_BENCH := $(URING_QD_DIR)/qd_bench

.PHONY: all lib unit integ device test clean examples
# The two shared libraries were missing from the default target, so editing
# src/preload.c and running `make` left a stale library on disk. The integration
# tests load it at run time rather than linking it, so running one of them
# directly then measured the OLD code while reporting on the new -- an hour was
# lost to a fix that was already correct and appeared not to work.
all: lib $(UNIT) $(UNIT_HELPERS) $(INTEG) libexitos_preload.so libexitos_bpftime.so $(URING_QD_BENCH)
lib: $(LIB)
# `ar rcs existing.a current.o...` updates named members but does not remove a
# member whose source disappeared.  That left the retired open-time preparer
# inside libexitos.a after its source was removed from src/.  Build a fresh
# archive and publish it atomically so membership is exactly $(OBJ).
$(LIB): $(OBJ)
	$(RM) $@.new
	ar rcs $@.new $^
	mv -f $@.new $@
%.o: %.c $(HDR); $(CC) $(CFLAGS) -c $< -o $@
# The backend-2 test exercises the hook's pure logic, which lives in the loader
# sources the static library deliberately does not carry (see OBJ above). It
# compiles that one file directly. Safe to link into a test binary: the
# constructor that arms interception is in bpftime_loader.c, not in the hook.
tests/unit/test_bpftime_backend: tests/unit/test_bpftime_backend.c $(LIB) src/bpftime_hook.c
	$(CC) $(CFLAGS) $< src/bpftime_hook.c $(LIB) -o $@ $(LDFLAGS) -ldl

tests/unit/test_minperf_safety: tests/unit/test_minperf_safety.c attribution/minperf.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

# These two include src/bpftime_hook.c to drive the real assembly shim. Without
# the explicit source dependency, editing the shim left the previous binary in
# place and the xstate contract was measured against code that no longer
# existed -- the exact stale-binary trap the DSO rules above were added for.
tests/unit/test_bpftime_avx: tests/unit/test_bpftime_avx.c src/bpftime_hook.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) -ldl
tests/unit/test_bpftime_xstate: tests/unit/test_bpftime_xstate.c src/bpftime_hook.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) -ldl

tests/unit/test_sync_lifecycle tests/unit/test_strict_mode: \
	src/intercept.c src/frontend_config.c src/preload.c $(HDR)
tests/unit/test_fdatasync_takeover: \
	src/intercept.c src/frontend_config.c src/preload.c $(HDR)
tests/unit/test_buffered_coherence: src/intercept.c $(HDR)
tests/unit/test_uring_cmd_batch: tests/unit/test_uring_cmd_batch.c \
	$(URING_QD_DIR)/uring_cmd_batch.c $(URING_QD_DIR)/uring_cmd_batch.h
	$(CC) $(CFLAGS) $< $(URING_QD_DIR)/uring_cmd_batch.c -o $@ $(LDFLAGS)
tests/unit/%: tests/unit/%.c $(LIB) $(HDR); $(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS)
tests/integ/%: tests/integ/%.c $(LIB) $(HDR); $(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS)
tests/device/%: tests/device/%.c $(LIB) $(HDR); $(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

# Tier 0: pure logic, no device, no root.  The backend E2E script is included
# only through its explicit early-exit contract mode; its loop-fixture path is
# never reached by this target, even when `make unit` itself runs as root.
# tools/cstate-keeper belongs here even though it is a tools/ binary:
# test_cpufreq_core_transaction.sh resolves it from the project root and fails
# the entry-symlink case without it. It used to pass only because a previous
# `make integ` had left the binary on disk, so a fresh clone that ran `make
# unit` first saw a failure that a second, unrelated target would have fixed.
unit: lib $(UNIT) $(UNIT_HELPERS) libexitos_preload.so libexitos_bpftime.so tools/cstate-keeper; @rc=0; for t in $(UNIT); do echo "== $$t"; ./$$t || rc=1; done; \
	for t in $(UNIT_SH); do echo "== $$t"; bash ./$$t || rc=1; done; \
	echo "== tests/integ/test_backends_e2e.sh (contract-only)"; EXITOS_BPFTIME_CONTRACT_ONLY=1 bash ./tests/integ/test_backends_e2e.sh || rc=1; exit $$rc
# Tier 1: loop device + ext4 image. Never touches a physical disk.
integ: lib $(INTEG) libexitos_preload.so libexitos_bpftime.so tools/cstate-keeper attribution/readback $(READBACK_PATTERN) $(FALLOCATE_PREPARE_PROBE); @rc=0; for t in $(INTEG); do echo "== $$t"; ./$$t || rc=1; done; \
	echo "== tests/integ/test_preload.sh"; ./tests/integ/test_preload.sh || rc=1; \
	echo "== tests/integ/test_crossproc.sh"; ./tests/integ/test_crossproc.sh || rc=1; \
	echo "== tests/integ/test_packaging.sh"; ./tests/integ/test_packaging.sh || rc=1; \
	echo "== tests/integ/test_bpftime_threads.sh"; ./tests/integ/test_bpftime_threads.sh || rc=1; \
	echo "== tests/integ/test_readback.sh"; EXITOS_REQUIRE_READBACK=1 ./tests/integ/test_readback.sh || rc=1; \
	echo "== tests/integ/test_cstate_keeper.sh"; bash ./tests/integ/test_cstate_keeper.sh || rc=1; \
	echo "== tests/integ/test_bpftime_fdatasync.sh"; ./tests/integ/test_bpftime_fdatasync.sh || rc=1; \
	echo "== tests/integ/test_fallocate_prepare_e2e.sh"; bash ./tests/integ/test_fallocate_prepare_e2e.sh || rc=1; exit $$rc
# The two backends must not end up in the same process: each .so carries exactly
# one loader constructor. Deriving the common set by FILTERING $(SRC) keeps the
# drift protection (a new module is picked up automatically) while keeping the
# loaders apart. Building both from plain $(SRC) put both constructors in both
# libraries, so a run meant to exercise symbol interposition also started the
# instruction rewriter -- and the comparison between the two backends was
# measuring the same thing twice.

libexitos_bpftime.so: $(SRC) $(HDR)
	@$(CC) $(STD) -O2 -Wall -D_GNU_SOURCE -fPIC -shared -Iinclude $(SRC_COMMON) src/bpftime_hook.c src/bpftime_loader.c -Wl,-z,now -Wl,-Bsymbolic -o libexitos_bpftime.so -ldl -lpthread
	@# Backend 2. Same sources as the static library, so the file list cannot drift.
	@# -z now / -Bsymbolic: every symbol is bound at load time and internal calls
	@# skip the PLT. Lazy binding is not safe inside a syscall hook -- the dynamic
	@# resolver runs on the intercepted thread and issues its own syscalls, which
	@# the rewriter has also rewritten, so resolving there re-enters the hook while
	@# the caller's argument registers are still live.

# The same sources with the dispatcher enabled, for measured end-to-end runs.
# NOT part of `all`: activating the syscall rewriter requires a transformer that
# carries tools/bpftime-clone-fix.patch, which upstream has not accepted, so the
# shipped libexitos_bpftime.so stays frozen and this variant is opt-in.
libexitos_bpftime_active.so: $(SRC) $(HDR)
	@$(CC) $(STD) -O2 -Wall -D_GNU_SOURCE -DEXITOS_BPFTIME_TEST_DORMANT_DISPATCH=1 -fPIC -shared -Iinclude $(SRC_COMMON) src/bpftime_hook.c src/bpftime_loader.c -Wl,-z,now -Wl,-Bsymbolic -o libexitos_bpftime_active.so -ldl -lpthread

libexitos_preload.so: $(SRC) $(HDR)
	@$(CC) $(STD) -O2 -Wall -D_GNU_SOURCE -fPIC -shared -Iinclude $(SRC_COMMON) src/preload.c -Wl,-z,now -Wl,-Bsymbolic -o libexitos_preload.so -ldl -lpthread
	@# Built from $(SRC), the same wildcard the static library uses. A hand-written
	@# file list here silently drifted once: src/durability.c and src/stats.c were
	@# added to the project but not to this line, and the library then failed at
	@# runtime with 'undefined symbol: exitos_durability_policy_for' only when armed,
	@# because the symbol resolves lazily and nothing calls it until registration.
# Tier 2: real NVMe. Opt-in only, requires EXITOS_DEV to be set explicitly.
device: lib $(DEVICE); @echo "*** RAW DEVICE WRITES. Read SAFETY.md. Names are NOT stable across reboots ***"; @test -n "$$EXITOS_DEV" || { echo "refusing: set EXITOS_DEV=/dev/... explicitly"; exit 2; }; \
	rc=0; for t in $(DEVICE); do echo "== $$t"; ./$$t || rc=1; done; exit $$rc
# The benchmark had no target at all: `make bench` silently did nothing and left
# whatever binary was there from a previous day, so a run that was meant to
# exercise a new safety gate exercised the old one instead. Anything that can be
# run must be buildable by name.
ATTRIBUTION := attribution/minperf attribution/probe attribution/donorpath attribution/hijackcost attribution/sizesweep attribution/qdsplit attribution/walwriter attribution/readback
attribution: $(ATTRIBUTION)
attribution/%: attribution/%.c $(LIB); $(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

examples: $(URING_QD_BENCH)
$(URING_QD_BENCH): $(URING_QD_DIR)/qd_bench.c \
	$(URING_QD_DIR)/uring_cmd_batch.c $(URING_QD_DIR)/uring_cmd_batch.h $(LIB) $(HDR)
	$(CC) $(CFLAGS) $(URING_QD_DIR)/qd_bench.c \
		$(URING_QD_DIR)/uring_cmd_batch.c $(LIB) -o $@ $(LDFLAGS)

tools/cstate-keeper: tools/cstate-keeper.c
	$(CC) $(CFLAGS) $< -o $@

$(READBACK_PATTERN): tests/integ/readback_pattern_contract.c attribution/readback.c
	$(CC) $(CFLAGS) $< -o $@

test: unit integ
clean:; rm -f $(OBJ) $(LIB) $(LIB).new $(UNIT) $(UNIT_HELPERS) $(INTEG) $(DEVICE) $(READBACK_PATTERN) $(FALLOCATE_PREPARE_PROBE) $(URING_QD_BENCH) libexitos_preload.so libexitos_bpftime.so libexitos_bpftime_active.so tests/unit/test_qdsplit_safety

# The two shared libraries had NO prerequisites, so make always considered them
# up to date and never rebuilt them. A remote copy then sat 16 hours behind its
# own source and a test read counters from a library that predated the counters.
# Naming the real files as targets, with their real inputs, removes the class.
.PHONY: preload bpftime-so
preload: libexitos_preload.so
bpftime-so: libexitos_bpftime.so
