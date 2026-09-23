#include "pipeline.h"

/* Header on every message; lets the consumer detect reused or clobbered memory. */
typedef struct {
    uint64_t seq;
    uint32_t size;
    uint32_t check;
} msg_header;

static inline uint32_t checksum(uint64_t seq, uint32_t size)
{
    return (uint32_t)((seq * 0x9E3779B97F4A7C15ull) >> 32) ^ size;
}

uint64_t pipe_ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_relax(void)
{
    __builtin_ia32_pause();
}

static uint64_t next_rand(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

/* Market-data shaped: mostly small updates, now and then a snapshot. */
static uint32_t message_size(uint64_t r)
{
    if ((r & 63) == 0)
        return 1024 + (uint32_t)((r >> 8) & 3071);   /* 1-4 KiB */
    return 32 + (uint32_t)((r >> 8) & 255);          /* 32-287 B */
}

/* ---- histogram ---- */

static inline unsigned bucket_of(uint64_t v)
{
    if (v < 16)
        return (unsigned)v;
    unsigned lg = 63 - (unsigned)__builtin_clzll(v);
    return 16 + ((lg - 4) << 4) + (unsigned)((v >> (lg - 4)) & 15);
}

static inline uint64_t bucket_floor(unsigned b)
{
    if (b < 16)
        return b;
    unsigned lg = ((b - 16) >> 4) + 4;
    return (uint64_t)(16 + ((b - 16) & 15)) << (lg - 4);
}

static inline void record(pipe_hist *h, uint64_t ticks)
{
    h->count++;
    h->bucket[bucket_of(ticks)]++;
    if (ticks > h->max)
        h->max = ticks;
}

uint64_t pipe_quantile(const pipe_hist *h, uint32_t per_million)
{
    if (h->count == 0)
        return 0;
    uint64_t target = (h->count * per_million + 999999) / 1000000;
    if (target == 0)
        target = 1;
    uint64_t seen = 0;
    for (unsigned b = 0; b < PIPE_BUCKETS; b++) {
        seen += h->bucket[b];
        if (seen >= target)
            return bucket_floor(b);
    }
    return h->max;
}

static pipe_row summarize(const pipe_hist *h)
{
    return (pipe_row){
        .p50 = pipe_quantile(h, 500000),
        .p99 = pipe_quantile(h, 990000),
        .p999 = pipe_quantile(h, 999000),
        .p9999 = pipe_quantile(h, 999900),
        .max = h->max,
    };
}

void pipe_summarize(const pipeline *p, pipe_report *r)
{
    r->alloc = summarize(&p->alloc_ticks);
    r->free = summarize(&p->free_ticks);
    r->messages = p->free_ticks.count;
    r->corrupt = p->corrupt;
    r->failed = atomic_load_explicit(&p->failed, memory_order_relaxed);
}

size_t pipe_size(void)
{
    return sizeof(pipeline);
}

/* ---- pipeline ---- */

void pipe_init(pipeline *p, pipe_side producer, pipe_side consumer, uint64_t seed)
{
    __builtin_memset(p, 0, sizeof *p);
    atomic_init(&p->head, 0);
    atomic_init(&p->tail, 0);
    p->producer = producer;
    p->consumer = consumer;
    p->rng = seed | 1;
}

void pipe_reset_stats(pipeline *p)
{
    __builtin_memset(&p->alloc_ticks, 0, sizeof p->alloc_ticks);
    __builtin_memset(&p->free_ticks, 0, sizeof p->free_ticks);
}

int pipe_produce(pipeline *p)
{
    uint64_t head = atomic_load_explicit(&p->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&p->tail, memory_order_acquire) == PIPE_RING)
        return 0;

    uint32_t size = message_size(next_rand(&p->rng));
    uint64_t t0 = pipe_ticks();
    msg_header *m = p->producer.alloc(p->producer.ctx, size);
    uint64_t t1 = pipe_ticks();
    if (!m) {
        atomic_store_explicit(&p->failed, 1, memory_order_relaxed);
        return 0;
    }
    record(&p->alloc_ticks, t1 - t0);

    m->seq = p->seq;
    m->size = size;
    m->check = checksum(p->seq, size);
    ((volatile char *)m)[size - 1] = (char)p->seq;   /* touch the last byte too */
    p->seq++;

    p->slot[head % PIPE_RING] = m;
    atomic_store_explicit(&p->head, head + 1, memory_order_release);
    return 1;
}

int pipe_consume(pipeline *p)
{
    uint64_t tail = atomic_load_explicit(&p->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&p->head, memory_order_acquire))
        return 0;

    msg_header *m = p->slot[tail % PIPE_RING];
    atomic_store_explicit(&p->tail, tail + 1, memory_order_release);

    if (m->seq != p->expect || m->check != checksum(m->seq, m->size) ||
        ((volatile char *)m)[m->size - 1] != (char)m->seq)
        p->corrupt++;
    p->expect++;

    uint64_t t0 = pipe_ticks();
    p->consumer.free(p->consumer.ctx, m);
    uint64_t t1 = pipe_ticks();
    record(&p->free_ticks, t1 - t0);
    return 1;
}

void pipe_run_interleaved(pipeline *p, uint64_t messages)
{
    uint64_t sent = 0, done = 0, sched = p->rng ^ 0x5DEECE66Dull;
    while (done < messages && !atomic_load_explicit(&p->failed, memory_order_relaxed)) {
        uint64_t burst = 1 + (next_rand(&sched) & 31);
        while (burst-- && sent < messages && pipe_produce(p))
            sent++;
        burst = 1 + (next_rand(&sched) & 31);
        while (burst-- && pipe_consume(p))
            done++;
    }
}

void pipe_run_producer(pipeline *p, uint64_t messages)
{
    for (uint64_t sent = 0; sent < messages && !atomic_load_explicit(&p->failed, memory_order_relaxed);) {
        if (pipe_produce(p))
            sent++;
        else
            cpu_relax();
    }
}

void pipe_run_consumer(pipeline *p, uint64_t messages)
{
    for (uint64_t done = 0; done < messages && !atomic_load_explicit(&p->failed, memory_order_relaxed);) {
        if (pipe_consume(p))
            done++;
        else
            cpu_relax();
    }
}
