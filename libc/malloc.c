/*
 * malloc replacement for Linux, one clusteralloc heap per thread.
 *
 *   LD_PRELOAD=build/libclusteralloc.so your-program
 *   CLUSTERALLOC_STATS=1 LD_PRELOAD=...  prints allocator stats at exit
 *
 * Spans are mapped with MAP_POPULATE so page faults happen at map time, and
 * empty spans are pooled (malloc_trim() flushes the pool). The thread's heap
 * is in initial-exec TLS to avoid a __tls_get_addr call.
 */
#define _GNU_SOURCE
#include "../alloc/alloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static __thread ca_heap *my_heap __attribute__((tls_model("initial-exec")));
static pthread_key_t heap_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

/* ---- host pages ---- */

static void *map(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void *ca_pages_alloc(size_t bytes)
{
    /* Usually already aligned; try that before over-mapping. */
    char *p = map(bytes);
    if (!p || ((uintptr_t)p & (CA_SPAN_SIZE - 1)) == 0)
        return p;
    munmap(p, bytes);

    /* Over-map by one span and trim both ends. */
    char *raw = map(bytes + CA_SPAN_SIZE);
    if (!raw)
        return NULL;
    char *aligned = (char *)(((uintptr_t)raw + CA_SPAN_SIZE - 1) & ~(uintptr_t)(CA_SPAN_SIZE - 1));
    size_t head = (size_t)(aligned - raw);
    if (head)
        munmap(raw, head);
    if (CA_SPAN_SIZE - head)
        munmap(aligned + bytes, CA_SPAN_SIZE - head);
    return aligned;
}

void ca_pages_free(void *p, size_t bytes)
{
    munmap(p, bytes);
}

/* ---- per-thread heaps ---- */

static void thread_exit(void *h)
{
    my_heap = NULL;
    ca_heap_release(h);
}

static void make_key(void)
{
    pthread_key_create(&heap_key, thread_exit);
}

static __attribute__((noinline)) ca_heap *heap_slow(void)
{
    pthread_once(&key_once, make_key);
    ca_heap *h = ca_heap_acquire();
    if (h) {
        my_heap = h;                       /* before setspecific, which may allocate */
        pthread_setspecific(heap_key, h);  /* arms thread_exit */
    }
    return h;
}

static inline ca_heap *heap(void)
{
    ca_heap *h = my_heap;
    return __builtin_expect(h != NULL, 1) ? h : heap_slow();
}

static void *or_enomem(void *p)
{
    if (!p)
        errno = ENOMEM;
    return p;
}

/* ---- malloc family ---- */

EXPORT void *malloc(size_t size)
{
    ca_heap *h = heap();
    return or_enomem(h ? ca_alloc(h, size) : NULL);
}

EXPORT void free(void *p)
{
    /* A thread that never allocated has no heap; its frees are all remote. */
    ca_free(my_heap, p);
}

EXPORT void *calloc(size_t n, size_t size)
{
    size_t total;
    if (__builtin_mul_overflow(n, size, &total))
        return or_enomem(NULL);
    ca_heap *h = heap();
    void *p = h ? ca_alloc(h, total) : NULL;
    if (p)
        memset(p, 0, total);
    return or_enomem(p);
}

EXPORT void *realloc(void *p, size_t size)
{
    if (p && size == 0) {   /* glibc semantics */
        free(p);
        return NULL;
    }
    ca_heap *h = heap();
    return or_enomem(h ? ca_realloc(h, p, size) : NULL);
}

static void *aligned(size_t align, size_t size)
{
    ca_heap *h = heap();
    return or_enomem(h ? ca_alloc_aligned(h, size, align) : NULL);
}

EXPORT int posix_memalign(void **out, size_t align, size_t size)
{
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    void *p = aligned(align, size);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

EXPORT void *aligned_alloc(size_t align, size_t size)
{
    if (align == 0 || (align & (align - 1))) {
        errno = EINVAL;
        return NULL;
    }
    return aligned(align, size);
}

EXPORT void *memalign(size_t align, size_t size) { return aligned_alloc(align, size); }
EXPORT void *valloc(size_t size) { return aligned((size_t)sysconf(_SC_PAGESIZE), size); }

EXPORT void *pvalloc(size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return aligned(page, (size + page - 1) & ~(page - 1));
}

EXPORT size_t malloc_usable_size(void *p) { return ca_usable_size(p); }

EXPORT int malloc_trim(size_t pad)
{
    (void)pad;
    ca_trim(my_heap);
    return 1;
}

/* ---- process hooks ---- */

__attribute__((constructor)) static void setup(void)
{
    /* Keep the child from inheriting a held lock. */
    pthread_atfork(ca_lock_all, ca_unlock_all, ca_unlock_all);
}

__attribute__((destructor)) static void report(void)
{
    const char *v = getenv("CLUSTERALLOC_STATS");
    if (!v || !*v || *v == '0')
        return;

    ca_stats s = ca_stats_get();
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "clusteralloc: %llu heaps, %llu allocs, %llu frees (%llu remote), "
        "%llu KiB live small, %llu live large, %llu KiB mapped, %llu KiB pooled, %llu invalid frees\n",
        (unsigned long long)s.heaps, (unsigned long long)s.allocs,
        (unsigned long long)s.frees, (unsigned long long)s.remote_frees,
        (unsigned long long)(s.small_bytes >> 10), (unsigned long long)s.large_allocs,
        (unsigned long long)(s.mapped_bytes >> 10), (unsigned long long)(s.pool_bytes >> 10),
        (unsigned long long)s.invalid_frees);
    if (n > 0)
        (void)!write(2, buf, (size_t)n);
}
