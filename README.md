# clusteralloc

[![ci](https://github.com/ErChoi-com/clusteralloc/actions/workflows/ci.yml/badge.svg)](https://github.com/ErChoi-com/clusteralloc/actions/workflows/ci.yml)

A small memory allocator for producer/consumer pipelines, where one core
allocates a message and a different core frees it. Think market data feed
handlers or packet processing: the cross-thread free is the common case, not
the exception, and most general purpose mallocs aren't built around that.

The allocator core is ~500 lines of freestanding C. It's used in two places:

- `libc/` wraps it as a drop-in `malloc` for Linux (`LD_PRELOAD`).
- `kernel/` is a tiny bare-metal x86-64 kernel in Rust that uses the same
  core as its heap and runs the pipeline benchmark with no OS underneath,
  so you can see what's the allocator and what's the OS.

```
alloc/    allocator core (C, no libc)
libc/     malloc/free/etc. wrappers + a multithreaded stress test
bench/    pipeline benchmark, shared by Linux and the kernel
kernel/   the "latency lab" kernel
docs/     design notes
```

## Building

You need Linux (WSL2 works), gcc, make, QEMU and a Rust toolchain with the
bare-metal target:

```sh
sudo apt install build-essential qemu-system-x86
rustup target add x86_64-unknown-none

make test     # stress test + a few real programs under LD_PRELOAD
make bench    # pipeline benchmark, glibc vs clusteralloc
make run      # boot the kernel in QEMU (uses KVM if /dev/kvm is writable)
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

More detail, including the trade-offs, is in [docs/design.md](docs/design.md).

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

Run-to-run noise is maybe 10-20% on the tail columns. The kernel (`make run`)
runs the same benchmark on one CPU, once with interrupts off and once with a
1 kHz timer. Typical output:

```
interrupts off: 0 timer interrupts during the run
  (ns)              p50      p99    p99.9   p99.99        max
  alloc              28       73      109      616    2512645
  remote free        25       40       57      218    2284813
1 kHz timer: 236 timer interrupts during the run
  (ns)              p50      p99    p99.9   p99.99        max
  alloc              27       70      102     2157     584026
  remote free        25       36       49      333    1157059
```

The ~2 ms max shows up even with interrupts off. That's the host
descheduling the VM's vCPU, which the guest can't do anything about.

## Limitations

- x86-64 only. The kernel is single-CPU.
- Heaps are never freed. When a thread exits its heap is parked and reused
  by the next thread.
- No hardening. Invalid and double frees are counted and ignored.
- Freed spans are pooled (64 MiB by default) instead of returned to the OS
  until `malloc_trim()`. Fine for a long-running pipeline, wasteful for a
  short-lived program.

## License

MIT, see [LICENSE](LICENSE).
