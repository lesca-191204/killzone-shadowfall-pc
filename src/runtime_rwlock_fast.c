/* Experimental PS4 rwlock metadata: pointer-hashed registries, one bounded
 * spare holder per lock. Native pthread rwlocks and guest semantics are retained.
 * GOW3_RWLOCK_FAST=1 selects this backend in runtime_rwlock.c. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef struct Holder {
    pthread_t thread;
    unsigned readers, writer, pending;
    struct Holder *next;
} Holder;
typedef struct Rwlock {
    pthread_rwlock_t native;
    Holder *holders, *spare;
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;
typedef struct {
    pthread_mutex_t mutex;
    Rwlock *locks;
    size_t created, reads, writes, unlocks;
} Shard;
enum { SHARDS=256 };
static Shard shards[SHARDS];
static pthread_once_t initialized=PTHREAD_ONCE_INIT;
static pthread_mutex_t lazy_init=PTHREAD_MUTEX_INITIALIZER;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;

static void initialize(void) {
    for (unsigned i=0;i<SHARDS;++i) {
        int e=pthread_mutex_init(&shards[i].mutex,NULL);
        if (e) { fprintf(stderr,"STOP: rwlock shard init error %d\n",e); exit(21); }
    }
}
static Shard *shard_for(Rwlock *pointer) {
    uint64_t x=(uintptr_t)pointer;
    x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33;
    return &shards[x & (SHARDS-1)];
}
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* Hashing does not dereference untrusted handles. Membership and all metadata,
 * including lifetime pins, are protected by the selected shard mutex. */
static Rwlock *find_locked(Shard *s,Rwlock *handle) {
    for (Rwlock *r=s->locks;r;r=r->next) if (r==handle) return r;
    return NULL;
}
static int create(Rwlock **out) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    int e=pthread_rwlock_init(&r->native,NULL);
    if (e) { free(r); return e; }
    Shard *s=shard_for(r);
    pthread_mutex_lock(&s->mutex);
    r->next=s->locks; s->locks=r; ++s->created;
    __atomic_store_n(out,r,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}
static ABI int32_t rw_init(Rwlock **out,void *const *attr,const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    /* Explicit init accepts uninitialized storage: never read *out. */
    return error(create(out));
}
static void prune_locked(Rwlock *r,Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link=&r->holders;
    while (*link!=h) link=&(*link)->next;
    *link=h->next;
    if (!r->spare) { h->next=NULL; r->spare=h; }
    else free(h);
}
enum Operation { READ,WRITE,TRY_READ,TRY_WRITE,TIMED_READ,TIMED_WRITE };
static int32_t acquire(Rwlock **handle,enum Operation op,const GuestTime *time) {
    if (!handle) return error(EINVAL);
    Rwlock *value=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    if (!value) {
        /* Only zero-handle initialization is serialized globally. */
        pthread_mutex_lock(&lazy_init);
        int e=0;
        if (!__atomic_load_n(handle,__ATOMIC_ACQUIRE)) e=create(handle);
        value=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&lazy_init);
        if (e) return error(e);
    }
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    Shard *s=shard_for(value);
    pthread_mutex_lock(&s->mutex);
    Rwlock *r=find_locked(s,value);
    if (!r) { pthread_mutex_unlock(&s->mutex); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (h && (h->writer || (writer && h->readers))) {
        pthread_mutex_unlock(&s->mutex); return error(attempt ? EBUSY : EDEADLK);
    }
    if (h && h->readers==UINT_MAX) { pthread_mutex_unlock(&s->mutex); return error(EAGAIN); }
    if (!h) {
        h=r->spare;
        if (h) { r->spare=NULL; memset(h,0,sizeof(*h)); }
        else h=calloc(1,sizeof(*h));
        if (!h) { pthread_mutex_unlock(&s->mutex); return error(ENOMEM); }
        h->thread=pthread_self(); h->next=r->holders; r->holders=h;
    }
    ++r->inflight; ++h->pending;
    pthread_mutex_unlock(&s->mutex);
    int e;
    if (attempt || timed) e=writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
    else e=writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
            e=writer ? pthread_rwlock_timedwrlock(&r->native,&deadline) : pthread_rwlock_timedrdlock(&r->native,&deadline);
        }
    }
    pthread_mutex_lock(&s->mutex);
    --r->inflight; --h->pending;
    if (!e) { if (writer) { h->writer=1; ++s->writes; } else { ++h->readers; ++s->reads; } }
    prune_locked(r,h);
    pthread_mutex_unlock(&s->mutex);
    return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Rwlock *value=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    Shard *s=shard_for(value);
    pthread_mutex_lock(&s->mutex);
    Rwlock *r=find_locked(s,value);
    if (!r) { pthread_mutex_unlock(&s->mutex); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (!h || (!h->readers && !h->writer)) {
        /* Keep the stock FreeBSD-style cross-thread read release: Killzone's
         * asset streamer depends on it. Write holds remain thread-owned. */
        Holder *owner=r->holders;
        while (owner && !owner->readers) owner=owner->next;
        for (Holder *w=r->holders;w;w=w->next) if (w->writer) owner=NULL;
        if (!owner) { pthread_mutex_unlock(&s->mutex); return error(EPERM); }
        int e=pthread_rwlock_unlock(&r->native);
        if (!e) { --owner->readers; ++s->unlocks; }
        prune_locked(r,owner);
        pthread_mutex_unlock(&s->mutex);
        return error(e);
    }
    int e=pthread_rwlock_unlock(&r->native);
    if (!e) { if (h->writer) h->writer=0; else --h->readers; ++s->unlocks; }
    prune_locked(r,h);
    pthread_mutex_unlock(&s->mutex);
    return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Rwlock *value=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    if (!value) return 0;
    Shard *s=shard_for(value);
    pthread_mutex_lock(&s->mutex);
    Rwlock *r=find_locked(s,value);
    if (!r) { pthread_mutex_unlock(&s->mutex); return error(EINVAL); }
    if (r->inflight || r->holders) { pthread_mutex_unlock(&s->mutex); return error(EBUSY); }
    int e=pthread_rwlock_destroy(&r->native);
    if (!e) {
        Rwlock **link=&s->locks;
        while (*link!=r) link=&(*link)->next;
        *link=r->next;
        free(r->spare); free(r);
        __atomic_store_n(handle,(Rwlock *)(uintptr_t)1,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s->mutex);
    return error(e);
}
uintptr_t runtime_rwlock_fast_resolve(const char *name) {
    pthread_once(&initialized,initialize);
    if (!strcmp(name,"6ULAa0fq4jA#libkernel")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#libkernel")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#libkernel")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#libkernel")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#libkernel")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#libkernel")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#libkernel")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#libkernel")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#libkernel")) return (uintptr_t)rw_timedwrite;
    return 0;
}
void runtime_rwlock_fast_report(void) {
    pthread_once(&initialized,initialize);
    size_t created=0,reads=0,writes=0,unlocks=0;
    for (unsigned i=0;i<SHARDS;++i) {
        Shard *s=&shards[i]; pthread_mutex_lock(&s->mutex);
        created+=s->created; reads+=s->reads; writes+=s->writes; unlocks+=s->unlocks;
        pthread_mutex_unlock(&s->mutex);
    }
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu (sharded-256)\n",created,reads,writes,unlocks);
}
