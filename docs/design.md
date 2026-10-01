# Design notes

## The workload

A pipeline stage allocates a message, fills it, pushes the pointer through
a ring buffer, and the next stage (on another core) reads it and frees it.
In a typical malloc the free side is where things go wrong. The block
belongs to the producer's thread cache or arena, so the consumer either
takes a lock or puts the block in its own cache. The second option slowly
migrates memory from producer to consumer, and something has to rebalance
it later. Either way the tail latency is bad.

clusteralloc makes the cross-thread free cheap and always sends the block
home.

## Spans

All memory comes from the host in 64 KiB spans, aligned to 64 KiB. A small
span has a 128 byte header followed by blocks of a single size class:

```
[ header | block | block | ... | block | unused ]
           ^ free list               ^ bump pointer
```

Because spans are aligned, `p & ~0xFFFF` is the header for any pointer
into the span. The header records the owning heap, the block size and a
magic number (so stray pointers can be detected).

Size classes go 16..128 in steps of 16, then 4 classes per power of two up
to 8 KiB, 32 in total. Anything bigger is a "large" allocation: a run of
whole spans with the same header at the front, so the mask trick still
works.

## Local and remote frees

Only the owning heap touches a span's free list, so a local free is a plain
linked-list push with no atomics.

A remote free does one CAS to push the block onto the owner's `inbox`, a
Treiber stack that lives on its own cache line so producers and consumers
don't false-share. The owner swaps the whole inbox out with one exchange and
files the blocks back into their spans a few at a time: at most 8 blocks
every 4 allocations. Bigger batches (64 every 32) had the same median but
put a ~1.2 us stall into about 3% of allocations, which is all of p99.

`used` counts blocks sitting in an inbox as still in use, so a span can't be
recycled while a block is in flight.

## Spans coming and going

A span that runs out of free blocks is taken off its class list and marked
`full`. The first free that lands in it puts it back. An empty span is
retired unless it's the last one in its class, which avoids churn when a
class bounces between 0 and 1 live blocks.

Retired spans go into a pool (runs of 1-8 spans, capped at 64 MiB) instead
of back to the OS. `malloc_trim()` drains all inboxes and empties the pool.

## Aligned allocations

`posix_memalign` and friends with alignment <= 8 KiB over-allocate a small
block and return an aligned pointer inside it. `free()` has to map that
interior pointer back to the block start. That would be a division by the
block size, but since the offset is < 2^16 and the block size is <= 2^13,
multiplying by `ceil(2^32 / size)` and shifting right by 32 gives the exact
quotient, and the reciprocal is stored in the span header.

## Thread exit

Heaps are never destroyed: a block from a dead thread's heap might still be
in some other thread's hands. When a thread exits (pthread key destructor)
its heap gives back its empty spans and is parked. The next new thread
adopts a parked heap before creating a new one.

## The kernel

The kernel exists to run the benchmark without an OS in the way. It boots
via Multiboot (QEMU `-kernel`), identity-maps the first 4 GiB, and sets up
just enough to have a timer interrupt. Physical memory is managed as a
bitmap of 64 KiB frames, which is the span size, so the allocator's page
hook doesn't have to do any alignment work. The kernel's own `Box`/`Vec`
also go through the C core via `#[global_allocator]`.

The benchmark runs both pipeline stages on one CPU with separate heaps, so
every free still takes the remote path, minus the cache misses. It runs once
with interrupts masked and once with a 1 kHz PIT timer to show how much of
the tail is just timer interrupts.
