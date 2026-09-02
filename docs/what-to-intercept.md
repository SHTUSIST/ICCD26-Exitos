# What interception should key on: the file, the process, or the thread

A database has several threads or processes writing its log. Other processes on
the same machine are doing unrelated work, and inside the database process there
are threads writing other files that must not be intercepted. Interception has
to cover the log writes and nothing else. This document sets out what the
interception decision keys on, why a PID supplied by the operator is not a
usable key, why the thread name is not one either, and where the current
implementation is still too loose.

## 1. Conclusion

**Do not key on the PID, and do not key on the thread name. Key on the file, and
key on the file's identity rather than on a fragment of its name.** The existing
implementation points the right way (`EXITOS_FILES` selects files, and
registration is per fd); what has to be added is tightening the match from "path
substring" to "exact path plus the (device number, inode) recorded at
registration", and checking that identity before every takeover. Of these three
items, the first two are new work; the third is already in the code
(`EXITOS_VERIFY_IDENTITY`, off by default).

## 2. Why the PID is the wrong key

**It is not stable.** A process gets a different PID on every restart, and the
way databases are commonly deployed makes it less stable still — MySQL is
brought up by `mysqld_safe` or systemd and restarted after a crash; OceanBase's
observer is likewise brought up by an external supervisor. The number filled in
today points at something else by the next morning, and **when it points at
something else it does not report an error**; it silently intercepts nothing at
all (or worse, intercepts an unrelated process that happens to have reused that
PID).

**Its granularity is wrong too.** In the deployment described above, the same
process holds both the threads writing the log and threads writing other files.
A PID covers the whole process, so intercepting by PID means intercepting every
write that process makes, including the ones that must not be touched. Dividing
further inside the process leads straight back to the same question: does this
fd point at the log file?

**If a process-scope restriction really is wanted, the stable key is still not
the PID**, but the executable path (the target of `/proc/<pid>/exe`), the cgroup
path, or the systemd unit name. Those three do not change across a restart. But
see section 4: in most cases none of them is needed.

## 3. Why the thread name is not enough either

The thread name (`prctl(PR_SET_NAME)`, visible through
`/proc/self/task/<tid>/comm`) has three problems:

1. **It is pure convention, with no guarantee behind it.** A database can rename
   its threads in a new version, and nothing announces the change.
2. **It is at most 15 characters**, and many implementations truncate to the
   same prefix.
3. **A thread does not write only one kind of file.** Even once the "log thread"
   is recognized, it still writes temporary files, reads configuration, and
   writes statistics; keying on the thread still means going back to the
   question of whether this particular write is to the log file.

Put the other way round, **keying on the file inherently does not require
knowing which thread it is**: whichever thread writes this fd takes the fast
path, and a thread writing other files is not touched by a single byte. That is
the semantics this deployment requires, and it depends on no convention.

## 4. Why keying on the file separates all three scopes on its own

- **Other threads in the same process writing other files:** unaffected.
  Registration is recorded per fd and the lookup is by fd; an fd that is not in
  the table is handed back to the kernel unchanged.
- **Several threads in the same process writing the same log fd:** all of them
  are taken over, which is correct — what they write is the same log. The code
  does hold one per-file lock across the device I/O (`r->mu` on both sides of
  `iopath_write`), so several threads genuinely sharing **the same fd** wait for
  each other. That lock is not the bottleneck in the 8-thread case, though: fio
  opens a separate fd for each thread, so there are 8 registrations and 8
  mutually independent locks, and no serialization occurs. The lock becomes the
  ceiling only when several threads really do share one fd writing one log.
- **Other processes on the machine:** entirely unaffected. Both frontends are
  scoped to the process they are injected into: `LD_PRELOAD` affects only the
  command tree it starts, and bpftime's instruction rewriting only rewrites
  instructions in its own process image. There is no "global interception".
- **Child processes the database forks:** they inherit the injection, but they
  also inherit the per-file decision — a child registers only when it opens the
  log.

## 5. One gap in the existing implementation: the match is too loose

The current selection logic is **path substring matching**: `EXITOS_FILES` holds
colon-separated fragments, and any opened path is selected as soon as `strstr`
hits one of them (`exitos_frontend_config_path_selected` in
`src/frontend_config.c`, which both frontends call). This is convenient in experiments and too
loose in production:

- `redo` matches both `/var/lib/mysql/#innodb_redo/...` and any temporary file
  whose path happens to contain `redo`.
- `.log` matches the database's own error log and slow query log — those are
  ordinary text append writes, not the object we are trying to accelerate, and
  they are usually buffered writes, which brings in a consistency contract
  outside the one in section 4.

**Suggested change:** every entry in `EXITOS_FILES` should be either an exact
absolute path or an explicitly wildcarded pattern
(`/var/lib/mysql/#innodb_redo/*`), with no unanchored substring matching left.
This is an incompatible change, so the code as it stands still does substring
matching and the tightening is listed as an open item.

**Pin the identity once more at registration.** The code already records
`(st_dev, st_ino)` at registration time (`src/intercept.c:866-867`, in
`reg_build`), and
`EXITOS_VERIFY_IDENTITY=1` spends one `fstat` before every write to recheck
whether this fd still points at the same inode; on finding the descriptor
rebound by `dup2` it revokes the registration and falls back to the normal path.
It is off by default, because it costs one extra syscall per write. For a
production database, keeping it on is recommended: when the log file is rotated,
renamed, or replaced, this is the only mechanism that can detect it on the spot.

## 6. Whether to keep a manual interface

Keep one, but **not a PID**. In order of usefulness:

1. **An exact list of file paths** (what `EXITOS_FILES` becomes once tightened).
   This is the only interface that is required.
2. **An optional process-scope restriction**: if several instances run on the
   same machine and only one of them is to be accelerated, filter a second time
   on the executable path or the cgroup path, not on the PID. Those two do not
   change across a restart.
3. **A mode that observes without taking over**, so the operator can first
   confirm that "the files selected are exactly these" before turning
   acceleration on. The existing `EXITOS_VERBOSE` prints the decision for every
   registration (`FAST PATH` or `declined`, with the reason) and can already
   serve this purpose; it only has to be written into the deployment procedure.

As for "deciding automatically": the part that can be automatic is already being
done — registration refuses every file that does not meet the conditions (not on
a permitted device, opened read-only, O_APPEND, holding an unwritten extent, and
so on), and those refusals are more reliable than any heuristic that guesses from
a name. Adding automatic recognition by thread name or by write pattern is **not
recommended**: the cost of guessing wrong is sending writes that must not be
accelerated to the raw device, while the benefit of guessing right is only
saving the operator one line of configuration.

## Open items

- Tighten the substring matching of `EXITOS_FILES` into "exact absolute path or
  explicit wildcard". This is an incompatible change: the tests and
  documentation that currently depend on substrings have to change with it.
- Deployment recommendation: in production, turn `EXITOS_VERIFY_IDENTITY=1` on,
  and in the deployment procedure use `EXITOS_VERBOSE=1` for one idle run to
  check the list of files that were selected.
