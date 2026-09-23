/*
 * Pipeline benchmark: a producer allocates messages and passes them through
 * a ring to a consumer that frees them.
 *
 * Used by bench/linux.c (two pinned threads) and the kernel (one CPU).
 * Freestanding: no libc, no floating point.
 */
#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define PIPE_RING    256
#define PIPE_BUCKETS 1024

typedef struct {
    void *(*alloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *p);
    void *ctx;
} pipe_side;

/* Log-linear histogram of TSC ticks: 16 steps per power of two (~6%). */
typedef struct {
    uint64_t count, max;
    uint64_t bucket[PIPE_BUCKETS];
} pipe_hist;

typedef struct {
    _Alignas(64) _Atomic uint64_t head;   /* producer writes */
    _Alignas(64) _Atomic uint64_t tail;   /* consumer writes */
    _Alignas(64) void *slot[PIPE_RING];

    _Alignas(64) pipe_side producer;      /* producer-only state */
    uint64_t  seq, rng;
    _Atomic int failed;                   /* allocation failed; both sides stop */
    pipe_hist alloc_ticks;

    _Alignas(64) pipe_side consumer;      /* consumer-only state */
    uint64_t  expect, corrupt;
    pipe_hist free_ticks;
} pipeline;

void pipe_init(pipeline *p, pipe_side producer, pipe_side consumer, uint64_t seed);
void pipe_reset_stats(pipeline *p);

int pipe_produce(pipeline *p);   /* 1 if a message went out */
int pipe_consume(pipeline *p);   /* 1 if a message was taken and freed */

/* Both sides on the calling CPU, in random bursts. */
void pipe_run_interleaved(pipeline *p, uint64_t messages);
/* One side per thread. */
void pipe_run_producer(pipeline *p, uint64_t messages);
void pipe_run_consumer(pipeline *p, uint64_t messages);

uint64_t pipe_quantile(const pipe_hist *h, uint32_t per_million);
uint64_t pipe_ticks(void);

/* Results in TSC ticks; each host converts to ns itself. */
typedef struct {
    uint64_t p50, p99, p999, p9999, max;
} pipe_row;

typedef struct {
    pipe_row alloc, free;
    uint64_t messages, corrupt;
    int      failed;
} pipe_report;

void pipe_summarize(const pipeline *p, pipe_report *r);
size_t pipe_size(void);   /* for hosts that cannot see the struct (Rust) */

#endif
