/* Original runtime rwlock test cases plus isolated stress/benchmark tests. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int32_t (ABI *HandleOp)(void **);
typedef int32_t (ABI *MutexInit)(void **, void **, const char *);
#define GET(type,name) ((type)runtime_rwlock_resolve(name))
static HandleOp rw_read, rw_write, rw_tryread, rw_trywrite, rw_unlock, rw_destroy;
typedef struct { int64_t seconds, nanoseconds; } TestTime;
typedef int32_t (ABI *TimedLock)(void **, const TestTime *);
static void rw_setup(void) {
    rw_read=GET(HandleOp,"Ox9i0c7L5w0#libkernel"); rw_write=GET(HandleOp,"mqdNorrB+gI#libkernel");
    rw_tryread=GET(HandleOp,"XD3mDeybCnk#libkernel"); rw_trywrite=GET(HandleOp,"bIHoZCTomsI#libkernel");
    rw_unlock=GET(HandleOp,"+L98PIbGttk#libkernel"); rw_destroy=GET(HandleOp,"BB+kb08Tl9A#libkernel");
    assert(rw_read && rw_write && rw_tryread && rw_trywrite && rw_unlock && rw_destroy);
}
static void rw_lifecycle(void) {
    rw_setup();
    MutexInit init=GET(MutexInit,"6ULAa0fq4jA#libkernel");
    void *lock;
    assert((uint32_t)init(NULL,NULL,NULL)==0x80020016);
    assert(init(&lock,NULL,"rw-test")==0);
    assert(rw_read(&lock)==0 && rw_read(&lock)==0);
    assert((uint32_t)rw_trywrite(&lock)==0x80020010);
    assert((uint32_t)rw_write(&lock)==0x8002000b);
    assert((uint32_t)rw_destroy(&lock)==0x80020010);
    assert(rw_unlock(&lock)==0 && rw_unlock(&lock)==0);
    assert((uint32_t)rw_unlock(&lock)==0x80020001);
    assert(rw_write(&lock)==0);
    assert((uint32_t)rw_tryread(&lock)==0x80020010);
    assert((uint32_t)rw_read(&lock)==0x8002000b);
    assert(rw_unlock(&lock)==0 && rw_destroy(&lock)==0);
    assert((uintptr_t)lock==1);
    assert((uint32_t)rw_read(&lock)==0x80020016);
    assert((uint32_t)rw_destroy(&lock)==0x80020016);
    lock=NULL;
    assert(rw_destroy(&lock)==0);
    assert(rw_read(&lock)==0 && rw_unlock(&lock)==0 && rw_destroy(&lock)==0);
    lock=(void *)(uintptr_t)0xdead;
    assert((uint32_t)rw_tryread(&lock)==0x80020016);
}
typedef struct {
    void *lock;
    pthread_barrier_t ready, release, writer_ready;
    _Atomic int value;
} RwFixture;
static void barrier(pthread_barrier_t *b) {
    int e=pthread_barrier_wait(b); assert(e==0 || e==PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *reader_worker(void *context) {
    RwFixture *f=context;
    assert(rw_read(&f->lock)==0);
    barrier(&f->ready); barrier(&f->release);
    assert(atomic_load(&f->value)==0);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void *writer_worker(void *context) {
    RwFixture *f=context;
    assert((uint32_t)rw_trywrite(&f->lock)==0x80020010);
    barrier(&f->writer_ready);
    assert(rw_write(&f->lock)==0);
    atomic_store(&f->value,1);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void *holding_writer(void *context) {
    RwFixture *f=context;
    assert(rw_write(&f->lock)==0);
    barrier(&f->ready); barrier(&f->release);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void fixture_init(RwFixture *f) {
    assert(pthread_barrier_init(&f->ready,NULL,2)==0);
    assert(pthread_barrier_init(&f->release,NULL,2)==0);
    assert(pthread_barrier_init(&f->writer_ready,NULL,2)==0);
}
static void fixture_destroy(RwFixture *f) {
    assert(rw_destroy(&f->lock)==0);
    assert(pthread_barrier_destroy(&f->ready)==0);
    assert(pthread_barrier_destroy(&f->release)==0);
    assert(pthread_barrier_destroy(&f->writer_ready)==0);
}
static void rw_concurrency(void) {
    rw_setup();
    RwFixture f={0}; fixture_init(&f);
    pthread_t reader,writer;
    assert(rw_read(&f.lock)==0);
    assert(pthread_create(&reader,NULL,reader_worker,&f)==0);
    barrier(&f.ready); /* two threads now hold read locks at once */
    assert(rw_unlock(&f.lock)==0);
    assert(pthread_create(&writer,NULL,writer_worker,&f)==0);
    barrier(&f.writer_ready);
    assert((uint32_t)rw_destroy(&f.lock)==0x80020010);
    assert(atomic_load(&f.value)==0);
    barrier(&f.release);
    assert(pthread_join(reader,NULL)==0 && pthread_join(writer,NULL)==0);
    assert(atomic_load(&f.value)==1);
    fixture_destroy(&f);
}
static void *leaving_reader(void *context) {
    RwFixture *f=context;
    assert(rw_read(&f->lock)==0); return NULL;
}
/* FreeBSD lets another thread drop a read hold (Killzone relies on it); write holds stay owned. */
static void rw_cross_thread(void) {
    rw_setup();
    RwFixture f={0}; fixture_init(&f);
    pthread_t reader; assert(pthread_create(&reader,NULL,leaving_reader,&f)==0);
    assert(pthread_join(reader,NULL)==0);
    assert((uint32_t)rw_trywrite(&f.lock)==0x80020010);
    assert(rw_unlock(&f.lock)==0);
    assert((uint32_t)rw_unlock(&f.lock)==0x80020001);
    assert(rw_write(&f.lock)==0 && rw_unlock(&f.lock)==0);
    fixture_destroy(&f);
}
static void rw_timeouts(void) {
    rw_setup();
    TimedLock read_timed=GET(TimedLock,"iPtZRWICjrM#libkernel"), write_timed=GET(TimedLock,"adh--6nIqTk#libkernel");
    RwFixture f={0}; fixture_init(&f);
    pthread_t writer; assert(pthread_create(&writer,NULL,holding_writer,&f)==0);
    barrier(&f.ready);
    TestTime expired={0,0},invalid={0,1000000000};
    assert((uint32_t)read_timed(&f.lock,&expired)==0x8002003c);
    assert((uint32_t)write_timed(&f.lock,&expired)==0x8002003c);
    assert((uint32_t)read_timed(&f.lock,&invalid)==0x80020016);
    assert((uint32_t)rw_unlock(&f.lock)==0x80020001);
    barrier(&f.release); assert(pthread_join(writer,NULL)==0);
    assert(read_timed(&f.lock,&invalid)==0); /* available lock ignores deadline */
    assert(rw_unlock(&f.lock)==0);
    fixture_destroy(&f);
}
/* Extra tests below supplement the unchanged rwlock cases from test_runtime.c. */
enum { WORKERS=6 };
static void copied_handles(void) {
    void *lock=NULL;
    assert(rw_read(&lock)==0);
    void *alias=lock;
    assert(rw_unlock(&alias)==0 && rw_destroy(&alias)==0);
    assert((uint32_t)rw_tryread(&lock)==0x80020016);
    lock=NULL;
    assert(rw_read(&lock)==0 && rw_unlock(&lock)==0 && rw_destroy(&lock)==0);
}
typedef struct { void *lock; pthread_barrier_t start,held; } Lazy;
static void *lazy_reader(void *argument) {
    Lazy *f=argument;
    barrier(&f->start);
    assert(rw_read(&f->lock)==0);
    barrier(&f->held);
    assert(rw_unlock(&f->lock)==0);
    return NULL;
}
static void lazy_concurrency(void) {
    Lazy f={0}; pthread_t threads[WORKERS];
    assert(pthread_barrier_init(&f.start,NULL,WORKERS)==0);
    assert(pthread_barrier_init(&f.held,NULL,WORKERS)==0);
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_create(&threads[i],NULL,lazy_reader,&f)==0);
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_join(threads[i],NULL)==0);
    assert(rw_destroy(&f.lock)==0);
    assert(pthread_barrier_destroy(&f.start)==0 && pthread_barrier_destroy(&f.held)==0);
}
typedef struct { void *lock; unsigned value; } Shared;
static void *shared_writer(void *argument) {
    Shared *s=argument;
    for (unsigned i=0;i<2000;++i) {
        assert(rw_write(&s->lock)==0);
        ++s->value;
        assert(rw_unlock(&s->lock)==0);
        assert(rw_read(&s->lock)==0);
        assert(s->value>0);
        assert(rw_unlock(&s->lock)==0);
    }
    return NULL;
}
static void shared_stress(void) {
    Shared s={0}; pthread_t threads[WORKERS];
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_create(&threads[i],NULL,shared_writer,&s)==0);
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_join(threads[i],NULL)==0);
    assert(s.value==WORKERS*2000 && rw_destroy(&s.lock)==0);
}
typedef struct { void *lock; pthread_barrier_t *start; } DestroyRace;
static void *racing_reader(void *argument) {
    DestroyRace *f=argument;
    barrier(f->start);
    for (unsigned i=0;i<4000;++i) {
        int32_t e=rw_read(&f->lock);
        assert(e==0 || (uint32_t)e==0x80020016);
        if (!e) assert(rw_unlock(&f->lock)==0);
    }
    return NULL;
}
static void lifetime_stress(void) {
    void *lock=NULL;
    assert(rw_read(&lock)==0 && rw_unlock(&lock)==0);
    pthread_barrier_t start;
    assert(pthread_barrier_init(&start,NULL,WORKERS+1)==0);
    pthread_t threads[WORKERS]; DestroyRace args[WORKERS];
    for (unsigned i=0;i<WORKERS;++i) {
        args[i]=(DestroyRace){lock,&start};
        assert(pthread_create(&threads[i],NULL,racing_reader,&args[i])==0);
    }
    barrier(&start);
    int32_t e=rw_destroy(&lock);
    assert(e==0 || (uint32_t)e==0x80020010);
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_join(threads[i],NULL)==0);
    if (e) assert(rw_destroy(&lock)==0);
    assert(pthread_barrier_destroy(&start)==0);
}
typedef struct { void *lock; pthread_barrier_t *start; unsigned loops; } Bench;
static void *bench_worker(void *argument) {
    Bench *f=argument; barrier(f->start);
    for (unsigned i=0;i<f->loops;++i) {
        assert(rw_read(&f->lock)==0);
        assert(rw_unlock(&f->lock)==0);
    }
    return NULL;
}
static double monotonic_seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return now.tv_sec+now.tv_nsec/1e9;
}
static void benchmark(unsigned loops) {
    enum { LOCKS=3105 };
    void *locks[LOCKS];
    MutexInit init=GET(MutexInit,"6ULAa0fq4jA#libkernel");
    for (unsigned i=0;i<LOCKS;++i) assert(init(&locks[i],NULL,"bench")==0);
    pthread_barrier_t start; pthread_t threads[WORKERS]; Bench args[WORKERS];
    assert(pthread_barrier_init(&start,NULL,WORKERS+1)==0);
    for (unsigned i=0;i<WORKERS;++i) {
        args[i]=(Bench){locks[i],&start,loops};
        assert(pthread_create(&threads[i],NULL,bench_worker,&args[i])==0);
    }
    double begin=monotonic_seconds(); barrier(&start);
    for (unsigned i=0;i<WORKERS;++i) assert(pthread_join(threads[i],NULL)==0);
    double elapsed=monotonic_seconds()-begin;
    printf("BENCH: %u independent read/unlock pairs, %u registered locks, %.6f s, %.0f pairs/s\n",
           WORKERS*loops,LOCKS,elapsed,WORKERS*loops/elapsed);
    for (unsigned i=0;i<LOCKS;++i) assert(rw_destroy(&locks[i])==0);
    assert(pthread_barrier_destroy(&start)==0);
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--report-first")) {
        runtime_rwlock_report();
        puts("PASS: report before import resolution");
        return 0;
    }
    rw_setup();
    if (argc>1 && !strcmp(argv[1],"--bench")) { benchmark(10000); return 0; }
    rw_lifecycle(); puts("PASS: lifecycle and ownership");
    rw_concurrency(); puts("PASS: readers exclude writer");
    rw_cross_thread(); puts("PASS: cross-thread read release");
    rw_timeouts(); puts("PASS: timed and invalid-deadline behavior");
    copied_handles(); puts("PASS: copied and stale handles");
    lazy_concurrency(); puts("PASS: simultaneous lazy initialization");
    shared_stress(); puts("PASS: shared reader/writer stress");
    lifetime_stress(); puts("PASS: destruction with concurrent handle use");
    runtime_rwlock_report();
    return 0;
}
