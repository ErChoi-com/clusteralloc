/*
 * Stress test for malloc, aimed at cross-thread frees.
 *
 *   build/stress                                     system malloc (control)
 *   LD_PRELOAD=build/libclusteralloc.so build/stress
 *
 * Threads allocate stamped blocks, realloc some, and hand a share to other
 * threads, which verify the stamp and free them. Under clusteralloc it also
 * checks that all memory is accounted for at the end.
 */
#define _GNU_SOURCE
#include "../alloc/alloc.h"

#include <dlfcn.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define ROUNDS  400000
#define SLOTS   1024
#define MAILBOX 4096

typedef struct {
    pthread_mutex_t lock;
    size_t count;
    unsigned char *items[MAILBOX];
} mailbox;

static mailbox boxes[THREADS];
static pthread_barrier_t all_sent;
static unsigned long long corrupt;

static uint64_t next_rand(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* First 8 bytes hold the size, the rest a pattern derived from it. */
static void stamp(unsigned char *p, size_t size)
{
    memcpy(p, &size, sizeof size);
    for (size_t i = sizeof size; i < size; i++)
        p[i] = (unsigned char)(size * 31 + i);
}

static void check(unsigned char *p)
{
    size_t size;
    memcpy(&size, p, sizeof size);
    for (size_t i = sizeof size; i < size; i++)
        if (p[i] != (unsigned char)(size * 31 + i)) {
            __atomic_fetch_add(&corrupt, 1, __ATOMIC_RELAXED);
            return;
        }
}

static unsigned char *make(uint64_t *rng)
{
    uint64_t r = next_rand(rng);
    size_t size;
    switch (r & 15) {
    case 0:  size = 8192 + (r >> 8) % 300000; break;   /* large */
    case 1:
    case 2:  size = 129 + (r >> 8) % 8064; break;      /* medium */
    default: size = 8 + (r >> 8) % 121; break;         /* small */
    }

    unsigned char *p;
    if ((r >> 40) % 16 == 0) {
        size_t align = (size_t)32 << ((r >> 48) % 8);    /* 32 B .. 4 KiB */
        if (posix_memalign((void **)&p, align, size))
            p = NULL;
        else if ((uintptr_t)p % align)
            __atomic_fetch_add(&corrupt, 1, __ATOMIC_RELAXED);
    } else {
        p = malloc(size);
    }
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    stamp(p, size);
    return p;
}

static void post(int to, unsigned char *p)
{
    mailbox *m = &boxes[to];
    pthread_mutex_lock(&m->lock);
    int full = m->count == MAILBOX;
    if (!full)
        m->items[m->count++] = p;
    pthread_mutex_unlock(&m->lock);
    if (full) {
        check(p);
        free(p);
    }
}

static void collect(int me)
{
    static __thread unsigned char *batch[MAILBOX];
    mailbox *m = &boxes[me];
    pthread_mutex_lock(&m->lock);
    size_t n = m->count;
    memcpy(batch, m->items, n * sizeof *batch);
    m->count = 0;
    pthread_mutex_unlock(&m->lock);
    for (size_t i = 0; i < n; i++) {
        check(batch[i]);
        free(batch[i]);
    }
}

static void *worker(void *arg)
{
    int me = (int)(intptr_t)arg;
    uint64_t rng = 0x9E3779B97F4A7C15ull * (uint64_t)(me + 1);
    unsigned char *live[SLOTS] = {0};

    for (int round = 0; round < ROUNDS; round++) {
        uint64_t r = next_rand(&rng);
        size_t slot = r % SLOTS;
        unsigned char *p = live[slot];

        if (p && (r >> 20) % 8 == 0) {
            /* Resize instead of freeing; the common prefix must survive. */
            size_t old;
            memcpy(&old, p, sizeof old);
            size_t size = 8 + (r >> 24) % (old * 2);
            size_t keep = old < size ? old : size;
            unsigned char *q = realloc(p, size);
            for (size_t i = sizeof old; i < keep; i++)
                if (q[i] != (unsigned char)(old * 31 + i)) {
                    __atomic_fetch_add(&corrupt, 1, __ATOMIC_RELAXED);
                    break;
                }
            stamp(q, size);
            live[slot] = q;
            continue;
        }
        if (p) {
            if ((r >> 32) % 4 == 0) {
                post((me + 1 + (int)((r >> 36) % (THREADS - 1))) % THREADS, p);
            } else {
                check(p);
                free(p);
            }
        }
        live[slot] = make(&rng);
        if (round % 64 == 0)
            collect(me);
    }

    for (size_t i = 0; i < SLOTS; i++)
        if (live[i]) {
            check(live[i]);
            free(live[i]);
        }
    pthread_barrier_wait(&all_sent);
    collect(me);
    return NULL;
}

static void *idle(void *arg)
{
    return arg;
}

int main(void)
{
    ca_stats (*stats)(void) = (ca_stats (*)(void))dlsym(RTLD_DEFAULT, "ca_stats_get");
    printf("stress: %d threads x %d rounds (%s)\n", THREADS, ROUNDS,
           stats ? "clusteralloc" : "system malloc");

    /* glibc mallocs a little per cached thread stack. Run a throwaway set of
     * threads first so that is in the "before" snapshot. */
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, idle, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);
    ca_stats before = stats ? stats() : (ca_stats){0};

    pthread_barrier_init(&all_sent, NULL, THREADS);
    for (int i = 0; i < THREADS; i++) {
        pthread_mutex_init(&boxes[i].lock, NULL);
        pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
    }
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("  %llu corrupt blocks\n", corrupt);
    int bad = corrupt != 0;

    if (stats) {
        /* All worker memory must be back, wherever it was freed. */
        malloc_trim(0);
        ca_stats s = stats();
        printf("  %llu heaps, %llu allocs, %llu remote frees, %llu invalid frees\n",
               (unsigned long long)s.heaps, (unsigned long long)s.allocs,
               (unsigned long long)s.remote_frees, (unsigned long long)s.invalid_frees);
        printf("  live before %llu B + %llu large, after %llu B + %llu large, %llu KiB still mapped\n",
               (unsigned long long)before.small_bytes, (unsigned long long)before.large_allocs,
               (unsigned long long)s.small_bytes, (unsigned long long)s.large_allocs,
               (unsigned long long)(s.mapped_bytes >> 10));
        bad |= s.small_bytes != before.small_bytes || s.large_allocs != before.large_allocs ||
               s.invalid_frees != 0 || s.remote_frees == 0;
    }
    puts(bad ? "FAIL" : "ok");
    return bad;
}
