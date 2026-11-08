/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
/* Where a blocked thread sleeps, its own and reused across waits. Windows: an auto-reset event,
 * so a wake-up set before the thread sleeps stays set (none is lost); one SetEvent and one wait
 * per wake-up, where a winpthreads condition variable is two Win32 semaphores, a critical section
 * and a gate, made and destroyed for every blocking wait before. Elsewhere: a condition variable
 * on `lock`. */
#ifdef _WIN32
typedef HANDLE Parker;
#else
typedef pthread_cond_t *Parker;
#endif
typedef struct Waiter {
    int32_t need, result;
    int done;
    Parker parker;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
/* One lock for every semaphore, held only for short state changes: nothing below prints,
 * allocates or creates host objects while holding it. Killzone's job scheduler parks its
 * workers on kernel semaphores (JobSchedulerCPU::WaitForSemaphore in Guerrilla's 2013 PS4
 * postmortem): a 53-minute session made 85,441 semaphores and 6.9 million wait/signal pairs.
 * Each creation used to print under this lock and each blocking wait created and destroyed a
 * condition variable (two Win32 semaphore objects in winpthreads), stalling every other job
 * wake-up meanwhile. IDs hash into buckets: the long-lived job semaphores were created first,
 * so a single newest-first list put them behind every short-lived one. */
enum { BUCKETS = 256, LOGGED_CREATIONS = 8 };
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry[BUCKETS];
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema **bucket(uint32_t id) { return &registry[id&(BUCKETS-1)]; }
static Sema *find(uint32_t id) {
    for (Sema *s=*bucket(id);s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* GOW3_TRACE_SEMA: every creation and blocking wait (deadlock diagnosis). Read once. */
static int trace_sema(void) {
    static int value=-1;
    if (value<0) value=getenv("GOW3_TRACE_SEMA")!=NULL;
    return value;
}
/* Deadlines of kernel semaphore waits (also the Linux condition variables' clock). */
#define WAIT_CLOCK CLOCK_MONOTONIC
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
/* A thread waits on at most one semaphore at a time: its parker is made once (outside the
 * lock) and freed when the thread ends. */
static pthread_key_t parker_key;
static pthread_once_t parker_once=PTHREAD_ONCE_INIT;
static void parker_free(void *p) {
#ifdef _WIN32
    if (p) CloseHandle((HANDLE)p);
#else
    if (p) { pthread_cond_destroy(p); free(p); }
#endif
}
static void parker_key_init(void) { host_check(pthread_key_create(&parker_key,parker_free)); }
static Parker thread_parker(void) {
    host_check(pthread_once(&parker_once,parker_key_init));
    void *existing=pthread_getspecific(parker_key);
    if (existing) return (Parker)existing;
#ifdef _WIN32
    Parker parker=CreateEventW(NULL,FALSE,FALSE,NULL);
    if (!parker) { fprintf(stderr,"STOP: semaphore wait event (%lu)\n",GetLastError()); exit(21); }
#else
    Parker parker=malloc(sizeof(*parker));
    if (!parker) { fputs("STOP: out of memory for a semaphore wait\n",stderr); exit(21); }
    pthread_condattr_t attr;
    host_check(pthread_condattr_init(&attr));
    host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
    host_check(pthread_cond_init(parker,&attr));
    host_check(pthread_condattr_destroy(&attr));
#endif
    host_check(pthread_setspecific(parker_key,(void *)parker));
    return parker;
}
/* Requires `lock`. The waiter may run once `lock` is released. */
static void wake(Waiter *w) {
    w->done=1;
#ifdef _WIN32
    if (!SetEvent(w->parker)) { fprintf(stderr,"STOP: semaphore wake (%lu)\n",GetLastError()); exit(21); }
#else
    host_check(pthread_cond_signal(w->parker));
#endif
}
/* Requires `lock`, which is released while sleeping. Returns ETIMEDOUT once `deadline` (when
 * `timed`) has passed, otherwise 0 after a wake-up that may be spurious: callers check done. */
static int park(Waiter *w,int timed,uint64_t deadline) {
#ifdef _WIN32
    /* Whole milliseconds rounded down, then the rest by yielding: Windows rounds a timed wait up
     * to its timer tick (up to 15.6 ms), which made a 50 us timeout sleep that long. */
    DWORD ms=INFINITE;
    if (timed) {
        const uint64_t now=now_ns();
        if (now>=deadline) return ETIMEDOUT;
        ms=(DWORD)((deadline-now)/1000000);
    }
    pthread_mutex_unlock(&lock);
    const DWORD r=WaitForSingleObject(w->parker,ms);
    if (r==WAIT_TIMEOUT && !ms) SwitchToThread();
    pthread_mutex_lock(&lock);
    if (r==WAIT_FAILED) { fprintf(stderr,"STOP: semaphore wait (%lu)\n",GetLastError()); exit(21); }
    return 0;
#else
    if (!timed) return pthread_cond_wait(w->parker,&lock);
    const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    return pthread_cond_timedwait(w->parker,&lock,&end);
#endif
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    pthread_mutex_lock(&lock);
    if (next_id==UINT32_MAX) { pthread_mutex_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    Sema **head=bucket(s->id);
    s->next=*head; *head=s; *out=s->id;
    const uint32_t id=s->id;
    const size_t number=++created;
    pthread_mutex_unlock(&lock);
    if (number<=LOGGED_CREATIONS || trace_sema())
        printf("Runtime: semaphore id=%u '%s', attr=%u, initial=%d, max=%d%s\n",id,name,attr,initial,maximum,
               number==LOGGED_CREATIONS && !trace_sema() ? " (later ones: GOW3_TRACE_SEMA=1)" : "");
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    const Parker parker=block ? thread_parker() : (Parker)NULL;
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (trace_sema()) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need,.parker=parker};
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        while (!w.done) {
            int e=park(&w,timeout!=NULL,deadline);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        if (!--s->active && s->deleted) free(s);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next;
            wake(w);
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; ++count;
        wake(w);
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema **p=bucket(id);
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* POSIX semaphores (libScePosix): the guest sem_t holds a pointer to the host object.
 * Failures return -1 with the guest errno (FreeBSD values). sem_timedwait's deadline is
 * absolute CLOCK_REALTIME, which is also the condition variable's clock. */
typedef struct { pthread_mutex_t lock; pthread_cond_t posted; uint32_t value; } PosixSem;
typedef struct { int64_t sec, nsec; } GuestTimespec;
#define POSIX_SEM_VALUE_MAX 0x7fffffff
static int32_t posix_fail(int32_t error) { *runtime_errno()=error; return -1; }
static ABI int32_t psem_init(PosixSem **sem,int32_t shared,uint32_t value) {
    (void)shared;
    if (!sem || value>POSIX_SEM_VALUE_MAX) return posix_fail(22);
    PosixSem *s=calloc(1,sizeof(*s));
    if (!s) return posix_fail(28);
    host_check(pthread_mutex_init(&s->lock,NULL));
    host_check(pthread_cond_init(&s->posted,NULL));
    s->value=value; *sem=s;
    return 0;
}
static ABI int32_t psem_destroy(PosixSem **sem) {
    if (!sem || !*sem) return posix_fail(22);
    host_check(pthread_cond_destroy(&(*sem)->posted));
    host_check(pthread_mutex_destroy(&(*sem)->lock));
    free(*sem); *sem=NULL;
    return 0;
}
static int32_t psem_take(PosixSem **sem,const GuestTimespec *deadline) {
    if (!sem || !*sem) return posix_fail(22);
    if (deadline && (deadline->nsec<0 || deadline->nsec>=1000000000)) return posix_fail(22);
    PosixSem *s=*sem;
    struct timespec end={0};
    if (deadline) { end.tv_sec=(time_t)deadline->sec; end.tv_nsec=(long)deadline->nsec; }
    pthread_mutex_lock(&s->lock);
    while (!s->value) {
        int e=deadline ? pthread_cond_timedwait(&s->posted,&s->lock,&end) : pthread_cond_wait(&s->posted,&s->lock);
        if (e==ETIMEDOUT && !s->value) { pthread_mutex_unlock(&s->lock); return posix_fail(60); }
        if (e!=ETIMEDOUT) host_check(e);
    }
    --s->value;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t psem_wait(PosixSem **sem) { return psem_take(sem,NULL); }
static ABI int32_t psem_timedwait(PosixSem **sem,const GuestTimespec *deadline) {
    return deadline ? psem_take(sem,deadline) : posix_fail(22);
}
static ABI int32_t psem_post(PosixSem **sem) {
    if (!sem || !*sem) return posix_fail(22);
    PosixSem *s=*sem;
    pthread_mutex_lock(&s->lock);
    if (s->value==POSIX_SEM_VALUE_MAX) { pthread_mutex_unlock(&s->lock); return posix_fail(84); } /* EOVERFLOW */
    ++s->value;
    host_check(pthread_cond_signal(&s->posted));
    pthread_mutex_unlock(&s->lock);
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    pthread_mutex_unlock(&lock);
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#libkernel")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#libkernel")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#libkernel")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#libkernel")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#libkernel")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#libkernel")) return (uintptr_t)sem_delete;
    /* libScePosix imports arrive here spelled #libkernel (runtime.c canonical_name). */
    if (!strcmp(name,"pDuPEf3m4fI#libkernel")) return (uintptr_t)psem_init;
    if (!strcmp(name,"cDW233RAwWo#libkernel")) return (uintptr_t)psem_destroy;
    if (!strcmp(name,"YCV5dGGBcCo#libkernel")) return (uintptr_t)psem_wait;
    if (!strcmp(name,"w5IHyvahg-o#libkernel")) return (uintptr_t)psem_timedwait;
    if (!strcmp(name,"IKP8typ0QUk#libkernel")) return (uintptr_t)psem_post;
    return 0;
}
void runtime_sema_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    pthread_mutex_unlock(&lock);
}

