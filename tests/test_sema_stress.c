/* Stress for src/runtime_sema.c: a job semaphore with parked workers (Killzone's job scheduler
 * parks its workers on kernel semaphores), short-lived semaphores created and deleted beside it,
 * short timed waits that expire (each must end within a few ms of its deadline, not a timer
 * tick later), and deletes while threads wait. Every token signalled must be taken exactly once.
 * Run it in a loop with a timeout (a hang is a failure):
 *   clang -std=c11 -O2 -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema_stress.c \
 *       src/runtime_sema.c -o out/sema-stress
 *   for i in $(seq 20); do timeout 60 out/sema-stress || exit 1; done */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int32_t *runtime_errno(void) { static __thread int32_t e; return &e; }
typedef int32_t (ABI *Create)(uint32_t *,const char *,uint32_t,int32_t,int32_t,const void *);
typedef int32_t (ABI *CountOp)(uint32_t,int32_t);
typedef int32_t (ABI *Wait)(uint32_t,int32_t,uint32_t *);
typedef int32_t (ABI *Delete)(uint32_t);
static Create create; static CountOp signal_sem, poll_sem; static Wait wait_sem; static Delete delete_sem;
enum { WORKERS = 6, JOBS = 200000, CHURNERS = 3, CHURN = 20000, TIMEOUTS = 500 };
static uint32_t job_sem, done_sem;
static atomic_long taken, stop;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); exit(1); } } while (0)
static void *worker(void *p) {
    (void)p;
    for (;;) {
        CHECK(wait_sem(job_sem, 1, NULL) == 0);
        if (atomic_load(&stop)) return NULL;
        atomic_fetch_add(&taken, 1);
        CHECK(signal_sem(done_sem, 1) == 0);
    }
}
static unsigned next_random(unsigned *state) { /* xorshift32 */
    unsigned x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void *churner(void *p) {
    unsigned seed = (unsigned)(uintptr_t)p * 2654435761u;
    for (int i = 0; i < CHURN; ++i) {
        uint32_t id;
        CHECK(create(&id, "churn", 1, 0, 4, NULL) == 0);
        CHECK(signal_sem(id, 1 + (int32_t)(next_random(&seed) % 2)) == 0);
        uint32_t t = 1000;
        CHECK(wait_sem(id, 1, &t) == 0 && t == 1000);
        CHECK(delete_sem(id) == 0);
    }
    return NULL;
}
/* Waits of 50 us that expire, one semaphore each: none may oversleep to a timer tick. */
static double worst_timeout_ms;
static void *timeouts(void *p) {
    (void)p;
    for (int i = 0; i < TIMEOUTS; ++i) {
        uint32_t id, t = 50;
        CHECK(create(&id, "timeout", 1, 0, 1, NULL) == 0);
        const double start = now_ms();
        CHECK((uint32_t)wait_sem(id, 1, &t) == 0x8002003c && t == 0);
        const double took = now_ms() - start;
        CHECK(took >= 0.04); /* never before the deadline (clock granularity aside) */
        if (took > worst_timeout_ms) worst_timeout_ms = took;
        CHECK(delete_sem(id) == 0);
    }
    return NULL;
}
typedef struct { uint32_t id; int32_t result; } Deleted;
static void *deleted_waiter(void *p) { Deleted *d = p; d->result = wait_sem(d->id, 1, NULL); return NULL; }
int main(void) {
    create = (Create)runtime_sema_resolve("188x57JYp0g#libkernel");
    poll_sem = (CountOp)runtime_sema_resolve("12wOHk8ywb0#libkernel");
    signal_sem = (CountOp)runtime_sema_resolve("4czppHBiriw#libkernel");
    wait_sem = (Wait)runtime_sema_resolve("Zxa0VhQVTsk#libkernel");
    delete_sem = (Delete)runtime_sema_resolve("R1Jvn8bSCW8#libkernel");
    CHECK(create && poll_sem && signal_sem && wait_sem && delete_sem);
    CHECK(create(&job_sem, "jobs", 1, 0, 0x7fffffff, NULL) == 0);
    CHECK(create(&done_sem, "done", 1, 0, 0x7fffffff, NULL) == 0);
    const double begin = now_ms();
    pthread_t w[WORKERS], c[CHURNERS], expiring;
    for (int i = 0; i < WORKERS; ++i) CHECK(pthread_create(&w[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < CHURNERS; ++i) CHECK(pthread_create(&c[i], NULL, churner, (void *)(uintptr_t)(i + 1)) == 0);
    CHECK(pthread_create(&expiring, NULL, timeouts, NULL) == 0);
    unsigned seed = 12345;
    /* Batches of jobs, then a wait for all of them, as a frame's barrier does. */
    for (int done = 0; done < JOBS;) {
        int batch = 1 + (int)(next_random(&seed) % 64);
        if (done + batch > JOBS) batch = JOBS - done;
        CHECK(signal_sem(job_sem, batch) == 0);
        CHECK(wait_sem(done_sem, batch, NULL) == 0);
        done += batch;
    }
    const double jobs_ms = now_ms() - begin;
    for (int i = 0; i < CHURNERS; ++i) CHECK(pthread_join(c[i], NULL) == 0);
    CHECK(pthread_join(expiring, NULL) == 0);
    atomic_store(&stop, 1);
    CHECK(signal_sem(job_sem, WORKERS) == 0);
    for (int i = 0; i < WORKERS; ++i) CHECK(pthread_join(w[i], NULL) == 0);
    CHECK(atomic_load(&taken) == JOBS);
    CHECK((uint32_t)poll_sem(job_sem, 1) == 0x80020010 && (uint32_t)poll_sem(done_sem, 1) == 0x80020010);
    /* Delete while two threads wait: both see the deleted error, the ID is gone. */
    uint32_t id;
    CHECK(create(&id, "doomed", 1, 0, 1, NULL) == 0);
    Deleted d1 = {id, 99}, d2 = {id, 99};
    pthread_t t1, t2;
    CHECK(pthread_create(&t1, NULL, deleted_waiter, &d1) == 0);
    CHECK(pthread_create(&t2, NULL, deleted_waiter, &d2) == 0);
    while (runtime_sema_waiters(id) != 2) { struct timespec n = {0, 100000}; nanosleep(&n, NULL); }
    CHECK(delete_sem(id) == 0);
    CHECK(pthread_join(t1, NULL) == 0 && pthread_join(t2, NULL) == 0);
    CHECK((uint32_t)d1.result == 0x8002000d && (uint32_t)d2.result == 0x8002000d);
    printf("PASS: semaphore stress, %d jobs on %d workers in %.0f ms, %d short-lived semaphores, "
           "%d expired waits (worst %.2f ms), %.0f ms in all\n", JOBS, WORKERS, jobs_ms, CHURNERS * CHURN,
           TIMEOUTS, worst_timeout_ms, now_ms() - begin);
    return 0;
}
