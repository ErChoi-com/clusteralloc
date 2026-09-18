/*
 * clusteralloc: allocator for pipelines where one core allocates and
 * another frees.
 *
 * Each thread (or CPU) owns a heap and blocks always go back to their owner.
 * A cross-core free is a single CAS onto the owner's inbox; the owner drains
 * the inbox a few blocks at a time. Empty spans are pooled, not unmapped.
 *
 * Freestanding. Hosts: libc/ (malloc replacement, heap = thread) and
 * kernel/ (bare-metal lab, heap = pipeline stage).
 */
#ifndef CLUSTERALLOC_H
#define CLUSTERALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifndef CA_API
#define CA_API
#endif

#define CA_SPAN_SHIFT 16
#define CA_SPAN_SIZE  ((size_t)1 << CA_SPAN_SHIFT)   /* 64 KiB */
#define CA_MAX_SMALL  8192                           /* larger sizes get whole spans */
#define CA_MIN_ALIGN  16

typedef struct ca_heap ca_heap;

typedef struct {
    uint64_t heaps;          /* heaps ever created */
    uint64_t allocs;         /* small blocks handed out */
    uint64_t frees;          /* small blocks returned to their owner */
    uint64_t remote_frees;   /* ...of which were freed by another heap */
    uint64_t small_bytes;    /* bytes in live small blocks (inboxes included) */
    uint64_t spans;          /* small spans held by heaps */
    uint64_t large_allocs;   /* live large allocations */
    uint64_t large_bytes;    /* bytes mapped for them */
    uint64_t pool_bytes;     /* empty runs kept for reuse */
    uint64_t mapped_bytes;   /* everything currently taken from the host */
    uint64_t invalid_frees;  /* pointers that were not ours (or freed twice) */
} ca_stats;

/* Host hooks. ca_pages_alloc must return CA_SPAN_SIZE-aligned memory; both
 * may be called from any thread. */
void *ca_pages_alloc(size_t bytes);
void  ca_pages_free(void *p, size_t bytes);

/* Heaps are never destroyed. A released heap is parked and reused, so blocks
 * still in flight always have an owner. */
CA_API ca_heap *ca_heap_acquire(void);
CA_API void     ca_heap_release(ca_heap *h);

/* `h` is the caller's heap. ca_free accepts NULL for callers without one. */
CA_API void   *ca_alloc(ca_heap *h, size_t size);
CA_API void   *ca_alloc_aligned(ca_heap *h, size_t size, size_t align);
CA_API void   *ca_realloc(ca_heap *h, void *p, size_t size);
CA_API void    ca_free(ca_heap *h, void *p);
CA_API size_t  ca_usable_size(const void *p);

/* Drain `h` and all parked heaps, return empty spans and the pool to the host. */
CA_API void     ca_trim(ca_heap *h);
CA_API ca_stats ca_stats_get(void);

/* Hold every internal lock across fork(). */
CA_API void ca_lock_all(void);
CA_API void ca_unlock_all(void);

#endif
