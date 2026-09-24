# clusteralloc

A small memory allocator for producer/consumer pipelines, where one core
allocates a message and a different core frees it. Think market data feed
handlers or packet processing: the cross-thread free is the common case, not
the exception, and most general purpose mallocs aren't built around that.

The allocator core is ~500 lines of freestanding C, wrapped as a drop-in
`malloc` for Linux (`LD_PRELOAD`).

```
alloc/    allocator core (C, no libc)
libc/     malloc/free/etc. wrappers + a multithreaded stress test
bench/    pipeline latency benchmark
```

## Building

Linux (WSL2 works), gcc and make:

```sh
make test     # stress test + a few real programs under LD_PRELOAD
make bench    # pipeline benchmark, glibc vs clusteralloc
```

To try it on something else:

```sh
LD_PRELOAD=build/libclusteralloc.so ./your-program
CLUSTERALLOC_STATS=1 LD_PRELOAD=build/libclusteralloc.so ./your-program
```

## How it works

Each thread owns a heap. Memory is carved into 64 KiB aligned spans, each
holding one size class, so `free()` finds a block's span (and its owner)
with a mask. Freeing a block you own is a push onto the span's free list.
Freeing someone else's block is a single CAS onto the owner's inbox, and
the owner drains its inbox in small batches as it allocates. No locks on
either path.

## Numbers

`make bench` on an i5-12400F under WSL2, producer pinned to core 0 and
consumer to core 2, 2M messages (mostly 32-287 bytes, 1 in 64 is 1-4 KiB).
Latency in ns:

|                     | p50 | p99  | p99.9 | p99.99 | msgs/s |
|---------------------|----:|-----:|------:|-------:|-------:|
| glibc malloc        | 115 | 1744 | 11077 | 26257  | 2.6M   |
| clusteralloc malloc |  32 |  256 |   397 |  2462  | 6.2M   |
| glibc free          | 128 | 3282 | 13128 | 31180  |        |
| clusteralloc free   |  61 |  122 |   186 |  2359  |        |

Run-to-run noise is maybe 10-20% on the tail columns.

## Limitations

- x86-64 only (the benchmark uses rdtsc).
- Heaps are never freed. When a thread exits its heap is parked and reused
  by the next thread.
- No hardening. Invalid and double frees are counted and ignored.
- Freed spans are pooled (64 MiB by default) instead of returned to the OS
  until `malloc_trim()`. Fine for a long-running pipeline, wasteful for a
  short-lived program.

## License

MIT, see [LICENSE](LICENSE).
