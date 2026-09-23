/*
 * Pipeline latency on Linux, producer and consumer pinned to two cores.
 *
 *   build/pipeline [messages]                              system malloc
 *   LD_PRELOAD=build/libclusteralloc.so build/pipeline     clusteralloc
 */
#define _GNU_SOURCE
#include "pipeline.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void *sys_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void  sys_free(void *ctx, void *p)   { (void)ctx; free(p); }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static double ticks_per_ns(void)
{
    uint64_t n0 = now_ns(), t0 = pipe_ticks();
    while (now_ns() - n0 < 50000000)   /* 50 ms */
        ;
    return (double)(pipe_ticks() - t0) / (double)(now_ns() - n0);
}

typedef struct {
    pipeline *pipe;
    uint64_t messages;
    int cpu;
    int consume;
    pthread_barrier_t *start;
} job;

static void *run(void *arg)
{
    job *j = arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(j->cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        fprintf(stderr, "warning: could not pin to cpu %d\n", j->cpu);

    pthread_barrier_wait(j->start);
    if (j->consume)
        pipe_run_consumer(j->pipe, j->messages);
    else
        pipe_run_producer(j->pipe, j->messages);
    return NULL;
}

static void run_pair(pipeline *p, uint64_t messages, int producer_cpu, int consumer_cpu)
{
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, 2);
    job jobs[2] = {
        { p, messages, producer_cpu, 0, &start },
        { p, messages, consumer_cpu, 1, &start },
    };
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, run, &jobs[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&start);
}

static void row(const char *name, const pipe_row *r, double tpn)
{
    printf("  %-12s %8.0f %8.0f %8.0f %8.0f %10.0f\n", name,
           r->p50 / tpn, r->p99 / tpn, r->p999 / tpn, r->p9999 / tpn, r->max / tpn);
}

int main(int argc, char **argv)
{
    uint64_t messages = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000000;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    /* CPU 1 is often CPU 0's hyperthread sibling, so prefer CPU 2. */
    int producer_cpu = 0, consumer_cpu = cpus > 2 ? 2 : 1;
    const char *name = dlsym(RTLD_DEFAULT, "ca_stats_get") ? "clusteralloc" : "system malloc";
    double tpn = ticks_per_ns();

    pipeline *p = aligned_alloc(64, sizeof *p);
    if (!p)
        return 1;
    pipe_side side = { sys_alloc, sys_free, NULL };
    pipe_init(p, side, side, 42);

    run_pair(p, messages / 10, producer_cpu, consumer_cpu);   /* warm up */
    pipe_reset_stats(p);
    uint64_t t0 = now_ns();
    run_pair(p, messages, producer_cpu, consumer_cpu);
    double secs = (double)(now_ns() - t0) / 1e9;

    pipe_report r;
    pipe_summarize(p, &r);
    printf("%s: %llu messages, cpu %d allocates, cpu %d frees\n", name,
           (unsigned long long)r.messages, producer_cpu, consumer_cpu);
    printf("  %-12s %8s %8s %8s %8s %10s\n", "(ns)", "p50", "p99", "p99.9", "p99.99", "max");
    row("alloc", &r.alloc, tpn);
    row("remote free", &r.free, tpn);
    printf("  %.1f M messages/s, %llu corrupt\n", messages / secs / 1e6,
           (unsigned long long)r.corrupt);

    int bad = r.corrupt || r.failed;
    free(p);
    return bad;
}
