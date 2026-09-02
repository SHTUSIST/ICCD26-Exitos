# Can intercepted synchronous I/O be combined into one high-QD submission?

## Short answer

Not automatically. Eight synchronous workers already produce aggregate queue
depth eight. A central queue would change who submits those eight requests; it
would not create eight requests where only one exists.

This is a theory result only. No batching implementation or experiment was
performed.

## Where the aggregate queue depth already comes from

Little's law states that outstanding operations equal throughput times
per-operation latency. For a synchronous caller that number is fixed by the
call itself: one worker blocked inside a write keeps one operation outstanding,
and eight such workers keep eight. The existing design is therefore already
presenting aggregate QD approximately equal to the worker count, even though
every registration owns a one-entry ring.

A central submitter changes which thread rings the doorbell, not how many
requests exist at that instant. It could still alter CPU efficiency or tail
behavior, but the count of outstanding requests is set by the number of callers
concurrently blocked in the interceptor.

## When batching can help

A per-process submitter could collect simultaneously available requests from
multiple threads, fill several SQEs, and ring the submission doorbell once.
It helps only when savings from fewer `io_uring_enter` calls, fewer doorbells,
and better device batching exceed all of the following:

- MPSC enqueue/dequeue atomics and cacheline transfers;
- waking or polling a submitter and waking each waiting caller;
- completion lookup and delivery to the correct caller;
- queue ownership, fairness, cancellation, and error bookkeeping;
- any batching wait window added to a latency-sensitive request.

When an operation takes only a few microseconds, even a sub-microsecond
transfer between threads is material. A single central cacheline or dispatcher
can also recreate the process-wide contention ceiling that table/statistics
sharding removed.

## Why one synchronous thread cannot be made high-QD transparently

A synchronous `pwrite` caller does not issue its next write until the current
call returns. The interceptor cannot submit a future request it has not seen.
It can obtain QD greater than one only by:

1. having several callers concurrently blocked in the interceptor;
2. accepting an explicit asynchronous/batch API from the application; or
3. returning before completion, which changes synchronous syscall semantics.

Case 1 is exactly what the existing multi-thread and multi-process
configurations already do. Case 2 is a useful separate design, but no longer
transparent interception. Case 3 is incorrect for the current contract.

## Threads versus processes

Threads in one process can hand request descriptors to a shared submitter, but
their application buffers and fd lifecycle still have to remain valid until
completion. Multiple processes require a broker plus IPC/shared memory, buffer
copying or cross-mm pinning, credential/ownership checks, crash recovery, and
per-process completion delivery. That extra boundary makes a multi-process
central queue substantially more expensive and less transparent than a
per-process thread dispatcher.

Separate rings can still feed the same device hardware queues concurrently.
Combining them into one userspace ring is therefore not synonymous with
increasing device queue depth.

## Ordering and durability constraints

Any transparent design must preserve at least:

- order and overlap behavior for the same open-file description;
- the return value, partial-write, and error identity of every call;
- fd reuse, close/dup/truncate/fallocate, cancellation, and process-exit rules;
- the barrier represented by each `fdatasync`/`fsync` and the raw-debt state it
  acknowledges.

Independent writes may be submitted together while retaining individual
completions. Combining several callers' durability boundaries into one device
flush is closer to database group commit. It can be correct only with an
explicit barrier protocol that proves every covered write completed before the
flush and propagates failure to every dependent caller. It also changes timing
and failure coupling, so it must not be advertised as a free transparent
optimization.

## Decision

Do not replace the current per-registration QD1 design with a central queue.
For transparent synchronous interception, the worker concurrency already
supplies the available aggregate QD.

If a future application can expose batches or group-commit epochs explicitly,
an asynchronous API with per-fd ordering and individual completion tokens is a
better fit. Its likely first benefit is syscall/CPU amortization, not guaranteed
device throughput. That feature should be evaluated separately from the
transparent LD_PRELOAD/bpftime contract.
