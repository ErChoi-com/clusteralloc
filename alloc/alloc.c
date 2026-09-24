/*
 * clusteralloc core. See alloc.h for the overview.
 *
 * Memory comes from the host in 64 KiB-aligned spans. A small span has a
 * header at its start and holds blocks of one size class, so a pointer finds
 * its span with a mask. A large allocation is a run of whole spans with the
 * same header.
 *
 *   span:  [ header | block | block | block | ... | (never used yet) ]
 *                     ^ free list (owner only)      ^ bump
 *
 * Ownership: a span's free list, counts and links belong to its heap alone.
 * A heap's inbox is pushed by anyone (CAS) and taken by the owner (exchange).
 * The span pool and heap registry sit behind spinlocks.
 */
#include "alloc.h"

#include <stdatomic.h>
#include <stdbool.h>

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#else
#define cpu_relax() ((void)0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifndef CA_POOL_BYTES
#define CA_POOL_BYTES ((size_t)64 << 20)  /* max empty span bytes kept for reuse */
#endif

#define NUM_CLASSES  32
#define HEADER_SIZE  128       /* blocks start at this offset in a span */
#define POOL_RUNS    8         /* the pool keeps runs of 1..8 spans */
#define SPAN_MAGIC   0xC1A5A11Cu

/* Inbox drain: every DRAIN_EVERY allocations, file at most DRAIN_BATCH blocks.
 * On the pipeline bench 32/64 gave a ~1.2 us drain in 3% of calls (p99);
 * 4/8 gives ~270 ns with the same median. */
#define DRAIN_EVERY  4
#define DRAIN_BATCH  8

enum { SPAN_SMALL = 1, SPAN_LARGE = 2 };

typedef struct span span;
struct span {
    uint32_t magic;
    uint8_t  kind;
    uint8_t  cls;
    bool     full;        /* exhausted and parked off the class list */
    uint32_t block_size;
    uint32_t div;         /* ceil(2^32 / block_size), see block_of() */
    uint32_t used;        /* blocks out, including ones waiting in an inbox */
    ca_heap *heap;        /* owner (small spans) */
    span    *next, *prev; /* owner's class list, or the pool */
    void    *free;        /* blocks the owner has taken back */
    char    *bump;        /* next never-used block; user pointer for large */
    char    *end;         /* end of the block area / of the run */
    size_t   bytes;       /* size of the whole run */
};

_Static_assert(sizeof(span) <= HEADER_SIZE, "span header outgrew HEADER_SIZE");

/* Per-heap counters have one writer, so a relaxed load+store is enough (no
 * atomic RMW); other threads only read them for stats. */
typedef _Atomic uint64_t counter;
#define STAT_ADD(c, v) atomic_store_explicit(&(c), atomic_load_explicit(&(c), memory_order_relaxed) + (v), memory_order_relaxed)
#define STAT_SUB(c, v) atomic_store_explicit(&(c), atomic_load_explicit(&(c), memory_order_relaxed) - (v), memory_order_relaxed)
#define LOAD(c)        atomic_load_explicit(&(c), memory_order_relaxed)

struct ca_heap {
    span    *spans[NUM_CLASSES];  /* spans with room; the head is current */
    void    *pending;             /* taken from the inbox, not yet filed */
    uint32_t until_drain;
    struct {
        counter allocs, frees, remote_in, remote_out, bytes, spans;
    } stats;
    ca_heap *next_all;            /* registry of every heap, for stats */
    ca_heap *next_free;           /* parked-heap list */

    /* The only field other threads write; on its own cache line. */
    _Alignas(64) _Atomic(void *) inbox;
};

static struct {
    _Atomic int heap_lock;        /* heap creation and the parked list */
    ca_heap    *parked;
    char       *heap_chunk;
    size_t      heap_room;
    _Atomic(ca_heap *) all;

    _Atomic int pool_lock;
    span       *pool[POOL_RUNS + 1];
    size_t      pool_bytes;

    _Atomic uint64_t heaps, mapped, large_allocs, large_bytes, orphan_frees, invalid;
} g;

static void lock(_Atomic int *l)
{
    while (atomic_exchange_explicit(l, 1, memory_order_acquire))
        while (atomic_load_explicit(l, memory_order_relaxed))
            cpu_relax();
}

static void unlock(_Atomic int *l)
{
    atomic_store_explicit(l, 0, memory_order_release);
}

/* ---- size classes: 16..128 by 16, then 4 per doubling up to 8 KiB ---- */

static inline unsigned class_of(size_t n)
{
    if (n <= 128)
        return n ? (unsigned)((n - 1) >> 4) : 0;
    unsigned lg = 63 - (unsigned)__builtin_clzll(n - 1);   /* 7..12 */
    return 8 + ((lg - 7) << 2) + (unsigned)((n - 1) >> (lg - 2)) - 4;
}

static inline size_t class_size(unsigned c)
{
    if (c < 8)
        return (size_t)(c + 1) * 16;
    unsigned lg = 7 + ((c - 8) >> 2);
    return ((size_t)1 << lg) + (size_t)(((c - 8) & 3) + 1) * ((size_t)1 << (lg - 2));
}

static inline span *span_of(const void *p)
{
    return (span *)((uintptr_t)p & ~(uintptr_t)(CA_SPAN_SIZE - 1));
}

/* Map a pointer inside a block (aligned allocations return interior
 * pointers) to the block start. off < 2^16 and block_size <= 2^13, so
 * multiplying by ceil(2^32 / size) is an exact floor division. */
static inline void *block_of(const span *s, const void *p)
{
    char *start = (char *)s + HEADER_SIZE;
    uint32_t off = (uint32_t)((const char *)p - start);
    uint32_t idx = (uint32_t)(((uint64_t)off * s->div) >> 32);
    return start + (size_t)idx * s->block_size;
}

/* ---- runs of spans, with a pool of empty ones ---- */

static span *run_take(size_t spans)
{
    if (spans <= POOL_RUNS) {
        lock(&g.pool_lock);
        span *s = g.pool[spans];
        if (s) {
            g.pool[spans] = s->next;
            g.pool_bytes -= s->bytes;
        }
        unlock(&g.pool_lock);
        if (s)
            return s;
    }
    size_t bytes = spans << CA_SPAN_SHIFT;
    span *s = ca_pages_alloc(bytes);
    if (!s)
        return NULL;
    atomic_fetch_add_explicit(&g.mapped, bytes, memory_order_relaxed);
    s->bytes = bytes;
    return s;
}

static void run_unmap(span *s)
{
    atomic_fetch_sub_explicit(&g.mapped, s->bytes, memory_order_relaxed);
    ca_pages_free(s, s->bytes);
}

static void run_give(span *s)
{
    size_t spans = s->bytes >> CA_SPAN_SHIFT;
    s->magic = 0;   /* so a stale pointer into a pooled run is rejected */

    if (spans <= POOL_RUNS) {
        lock(&g.pool_lock);
        bool kept = g.pool_bytes + s->bytes <= CA_POOL_BYTES;
        if (kept) {
            s->next = g.pool[spans];
            g.pool[spans] = s;
            g.pool_bytes += s->bytes;
        }
        unlock(&g.pool_lock);
        if (kept)
            return;
    }
    run_unmap(s);
}

static void pool_flush(void)
{
    span *runs[POOL_RUNS + 1];
    lock(&g.pool_lock);
    for (int i = 0; i <= POOL_RUNS; i++) {
        runs[i] = g.pool[i];
        g.pool[i] = NULL;
    }
    g.pool_bytes = 0;
    unlock(&g.pool_lock);

    for (int i = 0; i <= POOL_RUNS; i++) {
        while (runs[i]) {
            span *s = runs[i];
            runs[i] = s->next;
            run_unmap(s);
        }
    }
}

/* ---- small spans ---- */

static void list_push(ca_heap *h, span *s)
{
    s->prev = NULL;
    s->next = h->spans[s->cls];
    if (s->next)
        s->next->prev = s;
    h->spans[s->cls] = s;
}

static void list_remove(ca_heap *h, span *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        h->spans[s->cls] = s->next;
    if (s->next)
        s->next->prev = s->prev;
}

static span *span_new(ca_heap *h, unsigned cls)
{
    span *s = run_take(1);
    if (!s)
        return NULL;
    size_t bs = class_size(cls);
    s->magic = SPAN_MAGIC;
    s->kind = SPAN_SMALL;
    s->cls = (uint8_t)cls;
    s->full = false;
    s->block_size = (uint32_t)bs;
    s->div = (uint32_t)((((uint64_t)1 << 32) + bs - 1) / bs);
    s->used = 0;
    s->heap = h;
    s->free = NULL;
    s->bump = (char *)s + HEADER_SIZE;
    s->end = s->bump + (CA_SPAN_SIZE - HEADER_SIZE) / bs * bs;
    list_push(h, s);
    STAT_ADD(h->stats.spans, 1);
    return s;
}

static void span_retire(ca_heap *h, span *s)
{
    list_remove(h, s);
    STAT_SUB(h->stats.spans, 1);
    run_give(s);
}

static inline void *span_pop(span *s)
{
    void *b = s->free;
    if (b) {
        s->free = *(void **)b;
    } else if (s->bump < s->end) {
        b = s->bump;
        s->bump += s->block_size;
    } else {
        return NULL;
    }
    s->used++;
    return b;
}

/* Return a block to its span (local free, or drained from the inbox). */
static void file_block(ca_heap *h, span *s, void *b)
{
    *(void **)b = s->free;
    s->free = b;
    s->used--;
    STAT_ADD(h->stats.frees, 1);
    STAT_SUB(h->stats.bytes, s->block_size);

    if (unlikely(s->full)) {
        s->full = false;
        list_push(h, s);
    } else if (unlikely(s->used == 0) && (s->prev || s->next)) {
        /* Empty: keep one span per class, recycle the rest. */
        span_retire(h, s);
    }
}

/* File up to DRAIN_BATCH blocks that other heaps sent back. */
static void reclaim(ca_heap *h)
{
    h->until_drain = DRAIN_EVERY;
    void *b = h->pending;
    if (!b) {
        if (!atomic_load_explicit(&h->inbox, memory_order_relaxed))
            return;
        b = atomic_exchange_explicit(&h->inbox, NULL, memory_order_acquire);
    }
    unsigned n = 0;
    while (b && n < DRAIN_BATCH) {
        void *next = *(void **)b;
        file_block(h, span_of(b), b);
        b = next;
        n++;
    }
    h->pending = b;
    STAT_ADD(h->stats.remote_in, n);
}

static void reclaim_all(ca_heap *h)
{
    do
        reclaim(h);
    while (h->pending || atomic_load_explicit(&h->inbox, memory_order_relaxed));
}

/* Drain the inbox, then retire every empty span, including a class's last. */
static void heap_shed(ca_heap *h)
{
    reclaim_all(h);
    for (unsigned c = 0; c < NUM_CLASSES; c++) {
        span *s = h->spans[c];
        while (s) {
            span *next = s->next;
            if (s->used == 0)
                span_retire(h, s);
            s = next;
        }
    }
}

/* ---- large allocations: a run of whole spans, header first ---- */

static void *large_alloc(size_t size, size_t align)
{
    /* The user pointer must stay in the first span so the mask finds the header. */
    size_t off = align > HEADER_SIZE ? align : HEADER_SIZE;
    if (off >= CA_SPAN_SIZE || size > SIZE_MAX - off - CA_SPAN_SIZE)
        return NULL;

    span *s = run_take((off + size + CA_SPAN_SIZE - 1) >> CA_SPAN_SHIFT);
    if (!s)
        return NULL;
    s->magic = SPAN_MAGIC;
    s->kind = SPAN_LARGE;
    s->heap = NULL;
    s->bump = (char *)s + off;
    s->end = (char *)s + s->bytes;
    atomic_fetch_add_explicit(&g.large_allocs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g.large_bytes, s->bytes, memory_order_relaxed);
    return s->bump;
}

static void large_free(span *s)
{
    atomic_fetch_sub_explicit(&g.large_allocs, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g.large_bytes, s->bytes, memory_order_relaxed);
    run_give(s);
}

/* ---- allocation paths ---- */

static inline void *handed_out(ca_heap *h, span *s, void *b)
{
    STAT_ADD(h->stats.allocs, 1);
    STAT_ADD(h->stats.bytes, s->block_size);
    return b;
}

static void *alloc_slow(ca_heap *h, unsigned cls)
{
    reclaim(h);   /* blocks coming home may refill this class */

    for (span *s; (s = h->spans[cls]) != NULL;) {
        void *b = span_pop(s);
        if (b)
            return handed_out(h, s, b);
        list_remove(h, s);   /* off the list until a free returns a block */
        s->full = true;
    }

    span *s = span_new(h, cls);
    return s ? handed_out(h, s, span_pop(s)) : NULL;
}

void *ca_alloc(ca_heap *h, size_t size)
{
    if (unlikely(size > CA_MAX_SMALL))
        return large_alloc(size, CA_MIN_ALIGN);
    if (unlikely(--h->until_drain == 0))
        reclaim(h);

    unsigned cls = class_of(size);
    span *s = h->spans[cls];
    if (likely(s != NULL)) {
        void *b = span_pop(s);
        if (likely(b != NULL))
            return handed_out(h, s, b);
    }
    return alloc_slow(h, cls);
}

void *ca_alloc_aligned(ca_heap *h, size_t size, size_t align)
{
    if (align <= CA_MIN_ALIGN)
        return ca_alloc(h, size);
    if (align & (align - 1))
        return NULL;

    /* Blocks are 16-aligned, so align - 16 bytes of slack always fit an
     * aligned start. */
    if (size <= CA_MAX_SMALL && align <= CA_MAX_SMALL && size + align - CA_MIN_ALIGN <= CA_MAX_SMALL) {
        char *p = ca_alloc(h, size + align - CA_MIN_ALIGN);
        return p ? (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1)) : NULL;
    }
    return large_alloc(size, align);
}

void ca_free(ca_heap *h, void *p)
{
    if (!p)
        return;
    span *s = span_of(p);
    if (unlikely(s->magic != SPAN_MAGIC || (char *)p < (char *)s + HEADER_SIZE)) {
        atomic_fetch_add_explicit(&g.invalid, 1, memory_order_relaxed);
        return;
    }
    if (unlikely(s->kind == SPAN_LARGE)) {
        large_free(s);
        return;
    }

    void *b = block_of(s, p);
    if (likely(s->heap == h)) {
        file_block(h, s, b);
        return;
    }

    /* Cross-core free: one CAS onto the owner's inbox. */
    ca_heap *owner = s->heap;
    void *head = atomic_load_explicit(&owner->inbox, memory_order_relaxed);
    do
        *(void **)b = head;
    while (!atomic_compare_exchange_weak_explicit(&owner->inbox, &head, b,
                                                  memory_order_release, memory_order_relaxed));
    if (h)
        STAT_ADD(h->stats.remote_out, 1);
    else
        atomic_fetch_add_explicit(&g.orphan_frees, 1, memory_order_relaxed);
}

size_t ca_usable_size(const void *p)
{
    if (!p)
        return 0;
    const span *s = span_of(p);
    if (s->magic != SPAN_MAGIC)
        return 0;
    if (s->kind == SPAN_LARGE)
        return (size_t)(s->end - (const char *)p);
    return (size_t)((char *)block_of(s, p) + s->block_size - (const char *)p);
}

void *ca_realloc(ca_heap *h, void *p, size_t size)
{
    if (!p)
        return ca_alloc(h, size);
    size_t have = ca_usable_size(p);
    if (size <= have && size >= have / 2)
        return p;

    void *q = ca_alloc(h, size);
    if (q) {
        __builtin_memcpy(q, p, have < size ? have : size);
        ca_free(h, p);
    }
    return q;
}

ca_heap *ca_heap_acquire(void)
{
    lock(&g.heap_lock);
    ca_heap *h = g.parked;
    if (h) {
        g.parked = h->next_free;
        unlock(&g.heap_lock);
        return h;
    }

    /* Heaps are never freed; carve them out of a shared span. */
    if (g.heap_room < sizeof(ca_heap)) {
        char *chunk = ca_pages_alloc(CA_SPAN_SIZE);
        if (!chunk) {
            unlock(&g.heap_lock);
            return NULL;
        }
        atomic_fetch_add_explicit(&g.mapped, CA_SPAN_SIZE, memory_order_relaxed);
        g.heap_chunk = chunk;
        g.heap_room = CA_SPAN_SIZE;
    }
    h = (ca_heap *)g.heap_chunk;
    g.heap_chunk += sizeof(ca_heap);
    g.heap_room -= sizeof(ca_heap);

    __builtin_memset(h, 0, sizeof *h);
    atomic_init(&h->inbox, NULL);
    h->until_drain = DRAIN_EVERY;
    h->next_all = atomic_load_explicit(&g.all, memory_order_relaxed);
    atomic_store_explicit(&g.all, h, memory_order_release);
    atomic_fetch_add_explicit(&g.heaps, 1, memory_order_relaxed);
    unlock(&g.heap_lock);
    return h;
}

void ca_heap_release(ca_heap *h)
{
    heap_shed(h);
    lock(&g.heap_lock);
    h->next_free = g.parked;
    g.parked = h;
    unlock(&g.heap_lock);
}

void ca_trim(ca_heap *h)
{
    if (h)
        heap_shed(h);

    /* Parked heaps have no owner; the lock keeps them from being acquired meanwhile. */
    lock(&g.heap_lock);
    for (ca_heap *p = g.parked; p; p = p->next_free)
        heap_shed(p);
    unlock(&g.heap_lock);

    pool_flush();
}

ca_stats ca_stats_get(void)
{
    ca_stats st = {0};
    for (ca_heap *h = atomic_load_explicit(&g.all, memory_order_acquire); h; h = h->next_all) {
        st.allocs += LOAD(h->stats.allocs);
        st.frees += LOAD(h->stats.frees);
        st.remote_frees += LOAD(h->stats.remote_out);
        st.small_bytes += LOAD(h->stats.bytes);
        st.spans += LOAD(h->stats.spans);
    }
    st.remote_frees += LOAD(g.orphan_frees);
    st.heaps = LOAD(g.heaps);
    st.large_allocs = LOAD(g.large_allocs);
    st.large_bytes = LOAD(g.large_bytes);
    st.mapped_bytes = LOAD(g.mapped);
    st.invalid_frees = LOAD(g.invalid);

    lock(&g.pool_lock);
    st.pool_bytes = g.pool_bytes;
    unlock(&g.pool_lock);
    return st;
}

void ca_lock_all(void)
{
    lock(&g.heap_lock);
    lock(&g.pool_lock);
}

void ca_unlock_all(void)
{
    unlock(&g.pool_lock);
    unlock(&g.heap_lock);
}
