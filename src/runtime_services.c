/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define USER_ID 1
#define USER_NAME "Player"
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }
static void net_trace_line(FILE *out,const char *api,int32_t result) {
    fprintf(out,"NETTRACE %s result=%d\n",api,result);
}

/* ---- UserService ---- */
static int np_private_auth_enabled(void);
static const char *local_user_slot(void) {
    if (!np_private_auth_enabled()) return USER_NAME;
    const char *player=getenv("KZSF_PRIVATE_PLAYER");
    if (!player || !*player) return USER_NAME;
    if (!strcmp(player,"Player1")) return "Player1";
    if (!strcmp(player,"Player2")) return "Player2";
    return NULL; /* Reject an invalid configured identity rather than aliasing another player. */
}
static const char *local_user_name(void) {
    const char *slot=local_user_slot();
    if (!slot || !np_private_auth_enabled()) return slot;
    const char *name=getenv("KZSF_PRIVATE_NICKNAME");
    if (!name || !*name) return slot;
    if (!strcmp(slot,USER_NAME)) return NULL;
    size_t size=strlen(name);
    if (size<3 || size>16) return NULL;
    for (size_t i=0;i<size;++i) {
        unsigned char c=(unsigned char)name[i];
        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') ||
              (c>='0' && c<='9') || c=='_' || c=='-')) return NULL;
    }
    return name;
}
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *local=local_user_name();
    if (!local) return USER_INVALID_ARGUMENT;
    if (strlen(local)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,local); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) {
    const char *v=getenv("GOW3_LANGUAGE");
    int id=v ? atoi(v) : 1;
    static int logged;
    if (!logged) { logged=1; printf("Runtime: game asked for the system language: %d\n",id); }
    return id;
}
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t safe_area(float *info) { if (!info) return SYSTEM_PARAMETER; *info=1.0f; return 0; } /* full screen */
static ABI int32_t disable_music_player(void) { return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
/* Sockets as on a PS4 with no connection: creating and configuring them works, datagrams go
 * nowhere, nothing is ever received, stream connects fail. Killzone's network client (Turbine)
 * aborts on an assertion when socket creation itself fails (multiplayer tab, R1 in the menu);
 * shadPS4's working sockets let it fall back to bot matches. */
#define NET_EWOULDBLOCK ((int32_t)0x80410123)
static ABI int32_t net_socket(const char *name,int family,int type,int protocol) {
    (void)name; (void)protocol;
    if (family!=2) { net_errno=47; return (int32_t)0x8041012F; } /* EAFNOSUPPORT */
    printf("Runtime: Net: socket created (type %d), nothing is reachable\n",type);
    return 0x100+new_id()*2+(type==1 || type==10); /* low bit: stream socket */
}
static ABI int32_t net_ok(void) { return 0; }
static ABI int32_t net_connect(int s) {
    if (!(s&1)) return 0; /* datagram: only sets the peer */
    net_errno=51; return NET_ENETUNREACH;
}
static ABI int32_t net_send(int s,const void *buf,uint64_t len) { (void)s; (void)buf; return (int32_t)len; }
static ABI int32_t net_would_block(void) { net_errno=35; return NET_EWOULDBLOCK; }
static ABI int32_t net_getsockname(int s,unsigned char *addr,uint32_t *len) {
    (void)s;
    if (!addr || !len || *len<16) { net_errno=22; return NET_EINVAL; }
    memset(addr,0,16); addr[0]=16; addr[1]=2; /* sin_len, AF_INET; port 0, INADDR_ANY */
    *len=16; return 0;
}
static ABI int32_t net_getsockopt(int s,int level,int name,void *value,uint32_t *len) {
    (void)s; (void)level; (void)name;
    if (value && len && *len) memset(value,0,*len);
    return 0;
}
static ABI int32_t net_epoll_wait(int id,void *events,int max,int32_t timeout_us) {
    (void)id; (void)events; (void)max;
    uint64_t wait_ns=(uint64_t)(timeout_us>100000 || timeout_us<0 ? 100000 : timeout_us)*1000;
#ifdef _WIN32
    compat_sleep_ns(wait_ns);
#else
    struct timespec t={0,(long)wait_ns}; nanosleep(&t,NULL);
#endif
    return 0; /* no events */
}
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI uint64_t net_htonll(uint64_t v) { return __builtin_bswap64(v); }
/* No sockets exist: a count query answers 0, a listing fills nothing. */
static ABI int32_t net_sock_info(int32_t s,void *info,int32_t n,int32_t flags) {
    (void)flags;
    if (!info) { if (s<0) return 0; net_errno=22; return NET_EINVAL; }
    if (n<=0) { net_errno=22; return NET_EINVAL; }
    if (s>=0) { net_errno=9; return (int32_t)0x80410109; } /* EBADF */
    return 0;
}
/* libScePosix sockets: -1 with the guest errno, the network is unreachable. */
static ABI int32_t posix_unreachable(void) { *runtime_errno()=51; return -1; }
static ABI int32_t netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    const char *probe=getenv("KZSF_PROBE_LINK");
    /* Diagnostic override only; UDP is enabled separately by KZSF_PROBE_UDP. */
    *state=probe && !strcmp(probe,"1") ? 3 : 0;
    return 0;
}
static ABI int32_t netctl_info(int code,void *info) {
    const char *endpoint=getenv("KZSF_PRIVATE_HTTP");
    if (code==3 && endpoint && !strcmp(endpoint,"http://127.0.0.1:17800")) {
        if (!info) return NET_CTL_INVALID_ADDR;
        /* Turbine reads the MTU even after an error; our private transport advertises 1500. */
        uint32_t mtu=1500; memcpy(info,&mtu,sizeof mtu); return 0;
    }
    return NET_CTL_NOT_CONNECTED;
}
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out, with an opt-in synthetic identity for local diagnostics ---- */
#define NP_USER_NOT_FOUND ((int32_t)0x80550007)
typedef struct { char data[16]; char term; char dummy[3]; } NpOnlineId;
typedef struct { NpOnlineId handle; unsigned char opt[8],reserved[8]; } NpId;
_Static_assert(sizeof(NpOnlineId)==20,"NP online ID ABI size");
_Static_assert(sizeof(NpId)==36,"NP ID ABI size");
_Static_assert(sizeof USER_NAME<=17,"Local online ID must fit 16 characters");
static int np_probe_enabled(void) {
    const char *value=getenv("KZSF_PROBE_NP");
    return value && !strcmp(value,"1");
}
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    if (user!=USER_ID) return NP_USER_NOT_FOUND;
    if (!local_user_name()) return NP_INVALID_ARGUMENT;
    *state=np_probe_enabled() ? 2 : 1; /* synthetic SIGNED_IN or SIGNED_OUT */
    return 0;
}
static ABI int32_t np_online_id(int32_t user,NpOnlineId *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    if (user!=USER_ID) return NP_USER_NOT_FOUND;
    if (!np_probe_enabled()) return NP_SIGNED_OUT;
    const char *local=local_user_name();
    if (!local) return NP_INVALID_ARGUMENT;
    memset(id,0,sizeof *id);
    memcpy(id->data,local,strlen(local));
    return 0;
}
static ABI int32_t np_id(int32_t user,NpId *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    if (user!=USER_ID) return NP_USER_NOT_FOUND;
    if (!np_probe_enabled()) return NP_SIGNED_OUT;
    NpId local={0};
    int32_t result=np_online_id(user,&local.handle);
    if (result) return result;
    *id=local; return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}
static ABI int32_t np_account_id(const void *online_id,uint64_t *account) {
    if (!online_id || !account) return NP_INVALID_ARGUMENT;
    *account=0; return NP_SIGNED_OUT;
}
#define NP_INVALID_SIZE ((int32_t)0x80550011)
#define NP_ABORTED ((int32_t)0x80550012)
#define NP_REQUEST_MAX ((int32_t)0x80550013)
#define NP_REQUEST_NOT_FOUND ((int32_t)0x80550014)
#define NP_INVALID_ID ((int32_t)0x80550015)
typedef struct { uint64_t size,affinity; int32_t priority; unsigned char padding[4]; } NpAsyncParam;
typedef struct { unsigned char content,chat,user_content; } NpParentalInfo;
typedef struct { char code[2],end,pad; } NpCountryCode;
typedef struct { char code[6],padding[10]; } NpLanguageCode;
typedef struct {
    uint64_t size; int32_t user; unsigned char padding[4];
    uint64_t features; unsigned char reserved[32];
} NpCheckPlusParam;
typedef struct { unsigned char authorized,reserved[32]; } NpCheckPlusResult;
_Static_assert(sizeof(NpAsyncParam)==24,"NP async parameter ABI size");
_Static_assert(sizeof(NpParentalInfo)==3,"NP parental info ABI size");
_Static_assert(sizeof(NpCountryCode)==4,"NP country ABI size");
_Static_assert(sizeof(NpLanguageCode)==16,"NP language ABI size");
_Static_assert(sizeof(NpCheckPlusParam)==56,"NP Plus parameter ABI size");
_Static_assert(sizeof(NpCheckPlusResult)==33,"NP Plus result ABI size");
/* Synthetic diagnostic data, unrelated to the user's real account or age. */
static const struct {
    int8_t age;
    NpParentalInfo restrictions;
    NpCountryCode country;
    NpLanguageCode language;
} np_probe_profile={.age=18,.country={.code={'u','s'}},.language={.code="en"}};
enum { NP_REQUEST_PENDING=1,NP_REQUEST_COMPLETE,NP_REQUEST_ABORTED };
typedef struct { int32_t id,result; int state; } NpManagerRequest;
static NpManagerRequest np_manager_requests[32];
static int32_t np_manager_next_id=0x20000001;
static pthread_mutex_t np_manager_lock=PTHREAD_MUTEX_INITIALIZER;
static int32_t np_validate_online_id(const NpOnlineId *online) {
    const char *name=local_user_name();
    if (!name || !online) return NP_INVALID_ARGUMENT;
    NpOnlineId local={0};
    memcpy(local.data,name,strlen(name));
    return !online->term && !memcmp(online->data,local.data,sizeof local.data) ? 0 : NP_USER_NOT_FOUND;
}
static NpManagerRequest *np_manager_find(int32_t id) {
    for (size_t i=0;i<32;++i) if (np_manager_requests[i].id==id && id>0) return &np_manager_requests[i];
    return NULL;
}
static ABI int32_t np_manager_create(const NpAsyncParam *param) {
    if (!param) return NP_INVALID_ARGUMENT;
    if (param->size!=sizeof *param) return NP_INVALID_SIZE;
    pthread_mutex_lock(&np_manager_lock);
    for (size_t i=0;i<32 && np_manager_next_id<INT32_MAX;++i) if (!np_manager_requests[i].id) {
        int32_t id=np_manager_next_id++;
        np_manager_requests[i]=(NpManagerRequest){id,0,NP_REQUEST_PENDING};
        pthread_mutex_unlock(&np_manager_lock); return id;
    }
    pthread_mutex_unlock(&np_manager_lock); return NP_REQUEST_MAX;
}
/* Successful begin holds the lock until the profile output and request result are written. */
static int32_t np_manager_begin(int32_t id,const NpOnlineId *online,NpManagerRequest **out) {
    if (!online || !id) return NP_INVALID_ARGUMENT;
    if (np_probe_enabled()) {
        int32_t error=np_validate_online_id(online);
        if (error) return error;
    }
    pthread_mutex_lock(&np_manager_lock);
    NpManagerRequest *request=np_manager_find(id);
    int32_t error=!request ? NP_REQUEST_NOT_FOUND :
        request->state==NP_REQUEST_ABORTED ? NP_ABORTED :
        request->state==NP_REQUEST_COMPLETE ? NP_INVALID_ARGUMENT :
        !np_probe_enabled() ? NP_SIGNED_OUT : 0;
    if (error) { pthread_mutex_unlock(&np_manager_lock); return error; }
    *out=request; return 0;
}
static int32_t np_manager_complete(NpManagerRequest *request) {
    request->result=0; request->state=NP_REQUEST_COMPLETE;
    pthread_mutex_unlock(&np_manager_lock); return 0;
}
/* shadPS4 np_manager.cpp declares two arguments; do not inspect a spare RDX.
 * Success means availability of this synthetic local environment, not PSN authentication. */
static ABI int32_t np_check_availability(int32_t id,const NpOnlineId *online) {
    NpManagerRequest *request;
    int32_t result=np_manager_begin(id,online,&request);
    if (result) return result;
    return np_manager_complete(request);
}
/* Guest 0x8e3476 requests feature 1. This is local test-session eligibility only;
 * it neither checks nor grants a real PSN subscription. Preserve reserved output bytes. */
static ABI int32_t np_check_plus(int32_t id,const NpCheckPlusParam *param,NpCheckPlusResult *out) {
    if (!id || !param || !out) return NP_INVALID_ARGUMENT;
    if (param->size!=sizeof *param) return NP_INVALID_SIZE;
    if (param->user==-1 || param->features!=1) return NP_INVALID_ARGUMENT;
    for (size_t i=0;i<sizeof param->reserved;++i)
        if (param->reserved[i]) return NP_INVALID_ARGUMENT;
    if (!np_private_auth_enabled()) return NP_SIGNED_OUT;
    NpOnlineId online;
    int32_t result=np_online_id(param->user,&online);
    if (result) return result;
    NpManagerRequest *request;
    result=np_manager_begin(id,&online,&request);
    if (result) return result;
    out->authorized=1;
    return np_manager_complete(request);
}
static ABI int32_t np_manager_poll(int32_t id,int32_t *result) {
    if (!result) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&np_manager_lock);
    NpManagerRequest *request=np_manager_find(id);
    int32_t error=!request ? NP_REQUEST_NOT_FOUND :
        request->state==NP_REQUEST_PENDING && np_probe_enabled() ? NP_INVALID_ID : 0;
    if (!error) *result=np_probe_enabled() ? request->result : NP_SIGNED_OUT;
    pthread_mutex_unlock(&np_manager_lock); return error;
}
static ABI int32_t np_manager_delete(int32_t id) {
    pthread_mutex_lock(&np_manager_lock);
    NpManagerRequest *request=np_manager_find(id);
    if (request) *request=(NpManagerRequest){0};
    pthread_mutex_unlock(&np_manager_lock); return request ? 0 : NP_REQUEST_NOT_FOUND;
}
static ABI int32_t np_manager_abort(int32_t id) {
    pthread_mutex_lock(&np_manager_lock);
    NpManagerRequest *request=np_manager_find(id);
    if (request && request->state!=NP_REQUEST_COMPLETE) {
        request->state=NP_REQUEST_ABORTED; request->result=NP_ABORTED;
    }
    pthread_mutex_unlock(&np_manager_lock); return request ? 0 : NP_REQUEST_NOT_FOUND;
}
static ABI int32_t np_parental_info(int32_t id,const NpOnlineId *online,int8_t *age,NpParentalInfo *info) {
    if (!age || !info) return NP_INVALID_ARGUMENT;
    NpManagerRequest *request;
    int32_t result=np_manager_begin(id,online,&request);
    if (result) return result;
    *age=np_probe_profile.age; *info=np_probe_profile.restrictions;
    return np_manager_complete(request);
}
static ABI int32_t np_account_country(const NpOnlineId *online,NpCountryCode *country) {
    if (!online || !country) return NP_INVALID_ARGUMENT;
    if (!np_probe_enabled()) return NP_SIGNED_OUT;
    int32_t error=np_validate_online_id(online);
    if (error) return error;
    *country=np_probe_profile.country; return 0;
}
static ABI int32_t np_account_language(int32_t id,const NpOnlineId *online,NpLanguageCode *language) {
    if (!language) return NP_INVALID_ARGUMENT;
    NpManagerRequest *request;
    int32_t result=np_manager_begin(id,online,&request);
    if (result) return result;
    *language=np_probe_profile.language;
    return np_manager_complete(request);
}
/* NP Auth: synthetic codes for the explicitly selected local test server only. */
#define NP_AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define NP_AUTH_INVALID_SIZE ((int32_t)0x80550302)
#define NP_AUTH_ABORTED ((int32_t)0x80550304)
#define NP_AUTH_REQUEST_MAX ((int32_t)0x80550305)
#define NP_AUTH_REQUEST_NOT_FOUND ((int32_t)0x80550306)
#define NP_AUTH_INVALID_ID ((int32_t)0x80550307)
typedef struct { char id[129]; unsigned char padding[7]; } NpClientId;
typedef struct { char code[129]; unsigned char padding[7]; } NpAuthorizationCode;
typedef struct {
    uint64_t size;
    const NpOnlineId *online;
    const NpClientId *client;
    const char *scope;
} NpAuthCodeParam;
_Static_assert(sizeof(NpClientId)==136,"NP client ID ABI size");
_Static_assert(sizeof(NpAuthorizationCode)==136,"NP authorization code ABI size");
_Static_assert(sizeof(NpAuthCodeParam)==32,"NP authorization parameter ABI size");
typedef struct { int32_t id,result; int state; } NpAuthRequest;
static NpAuthRequest np_auth_requests[16];
static int32_t np_auth_next_id=0x10000001;
static pthread_mutex_t np_auth_lock=PTHREAD_MUTEX_INITIALIZER;
static int np_private_auth_enabled(void) {
    const char *endpoint=getenv("KZSF_PRIVATE_HTTP");
    return np_probe_enabled() && endpoint && !strcmp(endpoint,"http://127.0.0.1:17800");
}
/* Caller holds np_auth_lock. Auth IDs cannot enter the Manager ID range. */
static NpAuthRequest *np_auth_find(int32_t id) {
    for (size_t i=0;i<16;++i) if (np_auth_requests[i].id==id && id>0) return &np_auth_requests[i];
    return NULL;
}
static ABI int32_t np_auth_create(const NpAsyncParam *param) {
    if (!param) return NP_AUTH_INVALID_ARGUMENT;
    if (param->size!=sizeof *param) return NP_AUTH_INVALID_SIZE;
    pthread_mutex_lock(&np_auth_lock);
    for (size_t i=0;i<16 && np_auth_next_id<0x20000000;++i) if (!np_auth_requests[i].id) {
        int32_t id=np_auth_next_id++;
        np_auth_requests[i]=(NpAuthRequest){id,0,NP_REQUEST_PENDING};
        pthread_mutex_unlock(&np_auth_lock); return id;
    }
    pthread_mutex_unlock(&np_auth_lock); return NP_AUTH_REQUEST_MAX;
}
static ABI int32_t np_auth_get_code(int32_t id,const NpAuthCodeParam *param,NpAuthorizationCode *code,int32_t *issuer) {
    if (!param || !code) return NP_AUTH_INVALID_ARGUMENT;
    if (param->size!=sizeof *param) return NP_AUTH_INVALID_SIZE;
    if (!param->online || !param->client || !param->scope) return NP_AUTH_INVALID_ARGUMENT;
    pthread_mutex_lock(&np_auth_lock);
    NpAuthRequest *request=np_auth_find(id);
    int32_t error=!request ? NP_AUTH_REQUEST_NOT_FOUND :
        request->state==NP_REQUEST_ABORTED ? NP_AUTH_ABORTED :
        request->state==NP_REQUEST_COMPLETE ? NP_AUTH_INVALID_ARGUMENT :
        !np_private_auth_enabled() ? NP_SIGNED_OUT : np_validate_online_id(param->online);
    if (!error) {
        /* Neither a PSN credential nor an identity proof: only the private server accepts it. */
        NpAuthorizationCode local={.code="KZSF_LOCAL_TEST"};
        const char *slot=local_user_slot();
        if (strcmp(slot,USER_NAME)) snprintf(local.code,sizeof local.code,"KZSF_LOCAL_TEST:%s",slot);
        *code=local;
        if (issuer) *issuer=0x100;
        request->result=0; request->state=NP_REQUEST_COMPLETE;
    }
    pthread_mutex_unlock(&np_auth_lock); return error;
}
static ABI int32_t np_auth_poll(int32_t id,int32_t *result) {
    if (!result) return NP_AUTH_INVALID_ARGUMENT;
    int enabled=np_private_auth_enabled();
    pthread_mutex_lock(&np_auth_lock);
    NpAuthRequest *request=np_auth_find(id);
    int32_t error=!request ? NP_AUTH_REQUEST_NOT_FOUND :
        request->state==NP_REQUEST_PENDING && enabled ? NP_AUTH_INVALID_ID : 0;
    if (!error) *result=enabled ? request->result : NP_SIGNED_OUT;
    pthread_mutex_unlock(&np_auth_lock); return error;
}
static ABI int32_t np_auth_delete(int32_t id) {
    pthread_mutex_lock(&np_auth_lock);
    NpAuthRequest *request=np_auth_find(id);
    if (request) *request=(NpAuthRequest){0};
    pthread_mutex_unlock(&np_auth_lock); return request ? 0 : NP_AUTH_REQUEST_NOT_FOUND;
}
static ABI int32_t np_auth_abort(int32_t id) {
    pthread_mutex_lock(&np_auth_lock);
    NpAuthRequest *request=np_auth_find(id);
    if (request && request->state!=NP_REQUEST_COMPLETE) {
        request->state=NP_REQUEST_ABORTED; request->result=NP_AUTH_ABORTED;
    }
    pthread_mutex_unlock(&np_auth_lock); return request ? 0 : NP_AUTH_REQUEST_NOT_FOUND;
}
/* Word filter (offline): comments pass unchanged and requests finish at once. */
#define NP_COMMUNITY_INSUFFICIENT_ARGUMENT ((int32_t)0x80550704)
static ABI int32_t word_filter_ctx(const void *self) { return self ? new_id() : NP_COMMUNITY_INSUFFICIENT_ARGUMENT; }
static ABI int32_t word_filter_request(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t word_filter_censor(int32_t request,const char *comment,void *option) {
    (void)request; (void)option; return comment ? 0 : NP_COMMUNITY_INSUFFICIENT_ARGUMENT;
}
static ABI int32_t word_filter_sanitize(int32_t request,const char *comment,char *out,void *option) {
    (void)request; (void)option;
    if (!comment || !out) return NP_COMMUNITY_INSUFFICIENT_ARGUMENT;
    memmove(out,comment,strnlen(comment,1024)+1);
    return 0;
}
static ABI int32_t word_filter_poll(int32_t request,int32_t *result) {
    (void)request;
    if (!result) return (int32_t)0x80550703; /* INVALID_ARGUMENT */
    *result=0; return 0; /* FINISHED */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; uint32_t button; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0},{"MsgDialog",0,0,0},{"SaveDataDialog",0,0,0},
                         {"NpProfileDialog",0,0,0},{"NpCommerceDialog",0,0,0},{"ImeDialog",0,0,0},
                         {"ErrorDialog",0,0,0},{"InvitationDialog",0,0,0},{"WebBrowserDialog",0,0,0}};
#define DIALOG_RUNNING 2
#define DIALOG_FINISHED 3
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_NOT_SUPPORTED ((int32_t)0x80B8000F)
static ABI int32_t common_init(void) { return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_FINISHED; dialogs[i].button=1; return 0; /* OK / YES */
}
/* A progress bar shows the game's own work: it runs until the game closes it. */
static int32_t dialog_open_progress(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s progress bar opened (runs until closed)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_RUNNING; dialogs[i].button=0; return 0;
}
static int32_t dialog_close(int i) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    dialogs[i].status=DIALOG_FINISHED; return 0;
}
static int32_t dialog_progress(int i, uint32_t target) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    return target ? DIALOG_PARAM_INVALID : 0;
}
/* Result: result (OK), button, reserved; SaveDataDialog's begins with the mode. */
static int32_t dialog_result(int i, unsigned char *out, size_t offset) {
    if (dialogs[i].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    uint32_t values[2]={0,dialogs[i].button};
    memcpy(out+offset,values,8);
    return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4) DIALOG(invitation,7) DIALOG(browser,8)
static ABI int32_t commerce_close(void) { return dialog_close(4); }
static ABI int32_t invitation_close(void) { return dialog_close(7); }
static ABI int32_t browser_close(void) { return dialog_close(8); }
/* ErrorDialog param: size, error code, user. The code goes to the log; the dialog finishes. */
static ABI int32_t error_init(void) { return dialog_init(6); }
static ABI int32_t error_open(const int32_t *param) {
    if (!param) return DIALOG_ARG_NULL;
    printf("Runtime: ErrorDialog for error 0x%08X\n",(uint32_t)param[1]);
    return dialog_open(6);
}
static ABI int32_t error_status(void) { return dialog_status(6); }
static ABI int32_t error_close(void) { return dialog_close(6); }
static ABI int32_t error_term(void) { return dialog_term(6); }
/* OrbisMsgDialogParam: BaseParam (48 bytes), size, mode (2 = progress bar).
 * OrbisSaveDataDialogParam: BaseParam, int32 size, mode (5 = progress bar). */
static int32_t dialog_open_mode(int i, const unsigned char *param, size_t mode_offset, uint32_t progress_mode) {
    if (!param) return DIALOG_ARG_NULL;
    uint32_t mode; memcpy(&mode,param+mode_offset,4);
    return mode==progress_mode ? dialog_open_progress(i) : dialog_open(i);
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
/* Logged: some games open a system message on fatal errors (disc read) and then hang on it.
 * Param: mode @56 (1 user, 2 progress, 3 system), userMsgParam* @64 (msg @8),
 * sysMsgParam* @80 (type @0). */
static ABI int32_t msg_open(const unsigned char *p) {
    if (p) {
        uint32_t mode; memcpy(&mode,p+56,4);
        const unsigned char *user,*sys; memcpy(&user,p+64,8); memcpy(&sys,p+80,8);
        const char *text=NULL; if (mode==1 && user) memcpy(&text,user+8,8);
        uint32_t type=0; if (mode==3 && sys) memcpy(&type,sys,4);
        fprintf(stderr,"Runtime: sceMsgDialogOpen mode %u, system type %u, text \"%.200s\" (caller 0x%llx)\n",mode,type,
                text ? text : "",(unsigned long long)(uintptr_t)__builtin_return_address(0));
    }
    return dialog_open_mode(1,p,56,2);
}
static ABI int32_t msg_status(void) { return dialog_status(1); }
static ABI int32_t msg_term(void) { return dialog_term(1); }
static ABI int32_t msg_close(void) { return dialog_close(1); }
static ABI int32_t msg_progress(uint32_t target, uint32_t value) { (void)value; return dialog_progress(1,target); }
static ABI int32_t msg_result(unsigned char *out) { return dialog_result(1,out,0); }
/* SaveDataDialog. OrbisSaveDataDialogParam: BaseParam (48), size, mode @52, dispType @56,
 * animParam @64, items @72, ..., userData @112. SaveDialogItems: userId @0, titleId @8,
 * dirName @16 (32-byte names), dirNameNum @24, newItem @32 (title @0), focusPos @40.
 * A list (mode 1) is shown for the player to choose; every other mode answers for the
 * first listed save. The result names the chosen save ("" = the new item). */
#define SAVE_MODE_LIST 1
#define SAVE_RESULT_CANCELED 1
static struct {
    uint32_t mode, result;
    void *user_data;
    char dirs[32][32];
    int dir_count, has_new, choosing;
} save_dialog;
static char save_labels[33][200], save_icons[33][720];
static void save_label(char *out, size_t size, const char *dir) {
    unsigned char param[RUNTIME_SAVE_PARAM_SIZE];
    if (runtime_savedata_param(USER_ID,dir,param)) { snprintf(out,size,"%s",dir); return; }
    int64_t mtime; memcpy(&mtime,param+1288,8);
    time_t t=(time_t)mtime; struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char when[32]; strftime(when,sizeof(when),"%Y-%m-%d %H:%M",&tm);
    const char *title=(const char *)param, *subtitle=(const char *)param+128;
    /* Title, subtitle and date on their own lines, beside the save's picture. */
    snprintf(out,size,"%.60s\n%.60s%s%s",title[0] ? title : dir,subtitle,subtitle[0] ? "\n" : "",when);
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_open(const unsigned char *p) {
    if (!p) return DIALOG_ARG_NULL;
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    memset(&save_dialog,0,sizeof(save_dialog));
    memcpy(&save_dialog.mode,p+52,4);
    memcpy(&save_dialog.user_data,p+112,8);
    const unsigned char *items; memcpy(&items,p+72,8);
    if (items) {
        const char *dirs; uint32_t count; const void *new_item;
        memcpy(&dirs,items+16,8); memcpy(&count,items+24,4); memcpy(&new_item,items+32,8);
        for (uint32_t i=0;dirs && i<count && save_dialog.dir_count<32;++i)
            if (dirs[i*32]) memcpy(save_dialog.dirs[save_dialog.dir_count++],dirs+i*32,31);
        save_dialog.has_new=new_item!=NULL;
    }
    if (save_dialog.mode!=SAVE_MODE_LIST) return dialog_open_mode(2,p,52,5);
    const char *labels[33], *icons[33]; int n=0;
    if (save_dialog.has_new) { snprintf(save_labels[n],sizeof(save_labels[n]),"New save"); labels[n]=save_labels[n]; icons[n]=NULL; ++n; }
    for (int i=0;i<save_dialog.dir_count;++i) {
        save_label(save_labels[n],sizeof(save_labels[n]),save_dialog.dirs[i]); labels[n]=save_labels[n];
        icons[n]=runtime_savedata_icon(USER_ID,save_dialog.dirs[i],save_icons[n],sizeof(save_icons[n])) ? NULL : save_icons[n];
        ++n;
    }
    uint32_t type; memcpy(&type,p+56,4);
    const char *title=type==1 ? "Save game" : type==2 ? "Load game" : "Delete save";
    if (!n || !gow3gpu_choice_begin(title,labels,icons,n,0)) {
        printf("Runtime: SaveDataDialog list (%s): %s\n",title,n ? "no window, cancelled" : "no saves, cancelled");
        save_dialog.result=SAVE_RESULT_CANCELED;
        dialogs[2].status=DIALOG_FINISHED; dialogs[2].button=0;
        return 0;
    }
    printf("Runtime: SaveDataDialog list (%s): %d entries shown\n",title,n);
    save_dialog.choosing=1;
    dialogs[2].status=DIALOG_RUNNING;
    return 0;
}
static ABI int32_t save_status(void) {
    if (save_dialog.choosing) {
        int choice=gow3gpu_choice_poll();
        if (choice!=-1) {
            save_dialog.choosing=0;
            dialogs[2].status=DIALOG_FINISHED;
            if (choice<0) { save_dialog.result=SAVE_RESULT_CANCELED; dialogs[2].button=0; }
            else {
                int dir=choice-save_dialog.has_new;
                /* The chosen save goes first; the new item has no name. */
                if (dir>=0) memcpy(save_dialog.dirs[0],save_dialog.dirs[dir],32);
                else save_dialog.dirs[0][0]=0;
                save_dialog.dir_count=dir>=0 || save_dialog.dir_count ? 1 : 0;
                dialogs[2].button=1;
            }
            printf("Runtime: SaveDataDialog list: %s%s\n",choice<0 ? "cancelled" : "chose ",
                   choice<0 ? "" : save_dialog.dirs[0][0] ? save_dialog.dirs[0] : "new save");
        }
    }
    return dialog_status(2);
}
static ABI int32_t save_term(void) { return dialog_term(2); }
static ABI int32_t save_close(void) { return dialog_close(2); }
static ABI int32_t save_progress(uint32_t target, uint32_t rate) { (void)rate; return dialog_progress(2,target); }
/* OrbisSaveDataDialogResult: mode, result, buttonId, dirName* @16, param* @24, userData @32. */
static ABI int32_t save_result(unsigned char *out) {
    int32_t r=dialog_result(2,out,4);
    if (r) return r;
    memcpy(out,&save_dialog.mode,4);
    memcpy(out+4,&save_dialog.result,4);
    char *dir; unsigned char *param;
    memcpy(&dir,out+16,8); memcpy(&param,out+24,8);
    const char *name=save_dialog.dir_count && !save_dialog.result ? save_dialog.dirs[0] : "";
    if (dir) { memset(dir,0,32); memcpy(dir,name,strnlen(name,31)); }
    if (param && name[0]) runtime_savedata_param(USER_ID,name,param);
    memcpy(out+32,&save_dialog.user_data,8);
    return 0;
}
static ABI int32_t save_ready(void) { return 1; }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("GOW3_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the user name: a controller alone can accept it (the port
     * has no on-screen keyboard). */
    const char *start=initial[0] ? initial : USER_NAME;
    if (!gow3gpu_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=gow3gpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies: accepted locally, recorded in the log ---- */
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { (void)ctx; (void)handle; (void)options; return 0; }
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    printf("Runtime: trophy %d unlocked\n",id);
    if (platinum) *platinum=-1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}
/* Nothing is unlocked on this console: an empty 128-trophy flag set. */
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    if (!flags || !count) return TROPHY_INVALID;
    if (ctx<=0 || handle<=0) return TROPHY_INVALID;
    memset(flags,0,16); *count=0;
    return 0;
}
static ABI int32_t trophy_release(int32_t id) { return id>0 ? 0 : TROPHY_INVALID; }

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=2; return 0; /* Full: nothing left to install */
}
/* Progress of installed chunks: done (1 of 1, never a zero total to divide by). */
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    out[0]=out[1]=1; return 0;
}
static ABI int32_t playgo_todo(int32_t handle,const void *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    return count ? 0 : PLAYGO_BAD_SIZE; /* everything is installed: nothing to schedule */
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0; return 0; /* already installed */
}
static uint64_t playgo_languages=~UINT64_C(0); /* every language chunk is installed */
static ABI int32_t playgo_get_languages(int32_t handle,uint64_t *mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!mask) return PLAYGO_BAD_POINTER;
    *mask=playgo_languages; return 0;
}
static ABI int32_t playgo_set_languages(int32_t handle,uint64_t mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_languages=mask; return 0;
}
static ABI int32_t playgo_prefetch(int32_t handle,const uint16_t *ids,uint32_t count,int8_t locus) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    return locus>=0 && locus<=3 && locus!=1 ? 0 : (int32_t)0x80B2000D; /* BAD_LOCUS */
}
static ABI int32_t playgo_get_todo(int32_t handle,void *list,uint32_t count,uint32_t *entries) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list || !entries) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *entries=0; return 0; /* nothing left to install */
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_terminate(void) { playgo_handle=0; return 0; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }
/* OpenPSID: a fixed 16-byte console identifier. */
static ABI int32_t open_psid(unsigned char *psid) {
    if (!psid) return 22;
    for (int i=0;i<16;++i) psid[i]=(unsigned char)(i+1);
    return 0;
}


/* Diagnostic wrappers preserve the offline contracts while exposing the path the game takes.
 * Never log request bodies, account data, or game packets. */
#define TRACE_STUB(api,impl) static ABI int32_t trace_##api(void) { \
    int32_t result=impl(); net_trace_line(stderr,#api,result); return result; }
TRACE_STUB(sceNpMatching2CreateContext,np_signed_out)
TRACE_STUB(sceNpMatching2ContextStart,np_signed_out)
TRACE_STUB(sceNpMatching2CreateJoinRoom,np_signed_out)
TRACE_STUB(sceNpMatching2JoinRoom,np_signed_out)
TRACE_STUB(sceNpMatching2SearchRoom,np_signed_out)
TRACE_STUB(sceNpMatching2GetWorldInfoList,np_signed_out)
TRACE_STUB(sceNpSetNpTitleId,ok_void)
TRACE_STUB(sceNpSetContentRestriction,ok_void)
TRACE_STUB(sceNpMatching2Initialize,ok_void)
TRACE_STUB(sceNpWebApiCreateContext,np_signed_out)
TRACE_STUB(sceNpWebApiSendRequest,np_signed_out)
TRACE_STUB(sceHttpSendRequest,http_fail)
TRACE_STUB(sceRudpInit,ok_void)
TRACE_STUB(sceRudpCreateContext,ok_void)
TRACE_STUB(sceRudpBind,ok_void)
TRACE_STUB(sceRudpInitiate,ok_void)
TRACE_STUB(sceRudpProcessEvents,ok_void)
TRACE_STUB(sceRudpNetFlush,ok_void)
TRACE_STUB(sceRudpNetReceived,ok_void)
TRACE_STUB(sceRudpPollWait,ok_void)
TRACE_STUB(sceRudpRead,ok_void)
TRACE_STUB(sceRudpWrite,ok_void)
#undef TRACE_STUB
static ABI int32_t trace_sceNpGetState(int32_t user,int32_t *state) {
    int32_t result=np_state(user,state);
    fprintf(stderr,"NETTRACE sceNpGetState result=%d state=%d\n",result,result==0 && state ? *state : -1);
    return result;
}
static ABI int32_t trace_sceNpGetOnlineId(int32_t user,NpOnlineId *id) {
    int32_t result=np_online_id(user,id); net_trace_line(stderr,"sceNpGetOnlineId",result); return result;
}
static ABI int32_t trace_sceNpGetNpId(int32_t user,NpId *id) {
    int32_t result=np_id(user,id); net_trace_line(stderr,"sceNpGetNpId",result); return result;
}
static ABI int32_t trace_sceNpCreateAsyncRequest(const NpAsyncParam *param) {
    int32_t result=np_manager_create(param); net_trace_line(stderr,"sceNpCreateAsyncRequest",result); return result;
}
static ABI int32_t trace_sceNpPollAsync(int32_t id,int32_t *out) {
    int32_t result=np_manager_poll(id,out); net_trace_line(stderr,"sceNpPollAsync",result); return result;
}
static ABI int32_t trace_sceNpDeleteRequest(int32_t id) {
    int32_t result=np_manager_delete(id); net_trace_line(stderr,"sceNpDeleteRequest",result); return result;
}
static ABI int32_t trace_sceNpAbortRequest(int32_t id) {
    int32_t result=np_manager_abort(id); net_trace_line(stderr,"sceNpAbortRequest",result); return result;
}
static ABI int32_t trace_sceNpCheckNpAvailability(int32_t id,const NpOnlineId *online) {
    int32_t result=np_check_availability(id,online); net_trace_line(stderr,"sceNpCheckNpAvailability",result); return result;
}
static ABI int32_t trace_sceNpCheckPlus(int32_t id,const NpCheckPlusParam *param,NpCheckPlusResult *out) {
    int32_t result=np_check_plus(id,param,out); net_trace_line(stderr,"sceNpCheckPlus",result); return result;
}
static ABI int32_t trace_sceNpAuthCreateAsyncRequest(const NpAsyncParam *param) {
    int32_t result=np_auth_create(param); net_trace_line(stderr,"sceNpAuthCreateAsyncRequest",result); return result;
}
static ABI int32_t trace_sceNpAuthGetAuthorizationCode(int32_t id,const NpAuthCodeParam *param,NpAuthorizationCode *code,int32_t *issuer) {
    int32_t result=np_auth_get_code(id,param,code,issuer); net_trace_line(stderr,"sceNpAuthGetAuthorizationCode",result); return result;
}
static ABI int32_t trace_sceNpAuthPollAsync(int32_t id,int32_t *out) {
    int32_t result=np_auth_poll(id,out); net_trace_line(stderr,"sceNpAuthPollAsync",result); return result;
}
static ABI int32_t trace_sceNpAuthDeleteRequest(int32_t id) {
    int32_t result=np_auth_delete(id); net_trace_line(stderr,"sceNpAuthDeleteRequest",result); return result;
}
static ABI int32_t trace_sceNpAuthAbortRequest(int32_t id) {
    int32_t result=np_auth_abort(id); net_trace_line(stderr,"sceNpAuthAbortRequest",result); return result;
}
static ABI int32_t trace_sceNpGetParentalControlInfo(int32_t id,const NpOnlineId *online,int8_t *age,NpParentalInfo *info) {
    int32_t result=np_parental_info(id,online,age,info); net_trace_line(stderr,"sceNpGetParentalControlInfo",result); return result;
}
static ABI int32_t trace_sceNpGetAccountCountry(const NpOnlineId *online,NpCountryCode *country) {
    int32_t result=np_account_country(online,country); net_trace_line(stderr,"sceNpGetAccountCountry",result); return result;
}
static ABI int32_t trace_sceNpGetAccountLanguage(int32_t id,const NpOnlineId *online,NpLanguageCode *language) {
    int32_t result=np_account_language(id,online,language); net_trace_line(stderr,"sceNpGetAccountLanguage",result); return result;
}
static ABI int32_t trace_sceNpRegisterStateCallback(void *callback,void *arg) {
    int32_t result=np_register(callback,arg); net_trace_line(stderr,"sceNpRegisterStateCallback",result); return result;
}
static ABI int32_t trace_sceNpRegisterPlusEventCallback(void *callback,void *arg) {
    int32_t result=np_register(callback,arg); net_trace_line(stderr,"sceNpRegisterPlusEventCallback",result); return result;
}
static ABI int32_t trace_sceNetCtlGetState(int32_t *state) {
    int32_t result=netctl_state(state);
    fprintf(stderr,"NETTRACE sceNetCtlGetState result=%d state=%d\n",result,state ? *state : -1);
    return result;
}
static ABI int32_t trace_sceNetCtlGetInfo(int code,void *info) {
    int32_t result=netctl_info(code,info);
    fprintf(stderr,"NETTRACE sceNetCtlGetInfo code=%d result=%d\n",code,result);
    return result;
}
static ABI int32_t trace_sceNetSocket(const char *name,int family,int type,int protocol) {
    int32_t result=net_socket(name,family,type,protocol); net_trace_line(stderr,"sceNetSocket",result); return result;
}
static ABI int32_t trace_sceNetConnect(int socket) {
    int32_t result=net_connect(socket); net_trace_line(stderr,"sceNetConnect",result); return result;
}
static ABI int32_t trace_sceNetSend(int socket,const void *data,uint64_t length) {
    int32_t result=net_send(socket,data,length); net_trace_line(stderr,"sceNetSend",result); return result;
}
static ABI int32_t trace_sceNetSendto(int socket,const void *data,uint64_t length) {
    int32_t result=net_send(socket,data,length); net_trace_line(stderr,"sceNetSendto",result); return result;
}
static ABI int32_t trace_sceNetRecv(void) {
    int32_t result=net_would_block(); net_trace_line(stderr,"sceNetRecv",result); return result;
}
static ABI int32_t trace_sceNetRecvfrom(void) {
    int32_t result=net_would_block(); net_trace_line(stderr,"sceNetRecvfrom",result); return result;
}
static const RuntimeExport trace_exports[]={
    {"sceNpGetState",trace_sceNpGetState}, {"sceNetCtlGetState",trace_sceNetCtlGetState},
    {"sceNetCtlGetInfo",trace_sceNetCtlGetInfo},
    {"sceNpGetOnlineId",trace_sceNpGetOnlineId}, {"sceNpGetNpId",trace_sceNpGetNpId},
    {"sceNpCreateAsyncRequest",trace_sceNpCreateAsyncRequest}, {"sceNpPollAsync",trace_sceNpPollAsync},
    {"sceNpDeleteRequest",trace_sceNpDeleteRequest}, {"sceNpAbortRequest",trace_sceNpAbortRequest},
    {"sceNpGetParentalControlInfo",trace_sceNpGetParentalControlInfo},
    {"sceNpGetAccountCountry",trace_sceNpGetAccountCountry}, {"sceNpGetAccountLanguage",trace_sceNpGetAccountLanguage},
    {"sceNpRegisterStateCallback",trace_sceNpRegisterStateCallback},
    {"sceNpRegisterPlusEventCallback",trace_sceNpRegisterPlusEventCallback},
    {"sceNpSetNpTitleId",trace_sceNpSetNpTitleId},
    {"sceNpSetContentRestriction",trace_sceNpSetContentRestriction},
    {"sceNpMatching2Initialize",trace_sceNpMatching2Initialize},
    {"sceNetSocket",trace_sceNetSocket}, {"sceNetConnect",trace_sceNetConnect},
    {"sceNetSend",trace_sceNetSend}, {"sceNetSendto",trace_sceNetSendto},
    {"sceNetRecv",trace_sceNetRecv}, {"sceNetRecvfrom",trace_sceNetRecvfrom},
    {"sceHttpSendRequest",trace_sceHttpSendRequest},
    {"sceNpCheckNpAvailability",trace_sceNpCheckNpAvailability},
    {"sceNpCheckPlus",trace_sceNpCheckPlus},
    {"sceNpAuthGetAuthorizationCode",trace_sceNpAuthGetAuthorizationCode},
    {"sceNpAuthCreateAsyncRequest",trace_sceNpAuthCreateAsyncRequest}, {"sceNpAuthPollAsync",trace_sceNpAuthPollAsync},
    {"sceNpAuthDeleteRequest",trace_sceNpAuthDeleteRequest}, {"sceNpAuthAbortRequest",trace_sceNpAuthAbortRequest},
    {"sceNpWebApiCreateContext",trace_sceNpWebApiCreateContext},
    {"sceNpWebApiSendRequest",trace_sceNpWebApiSendRequest},
    {"sceNpMatching2CreateContext",trace_sceNpMatching2CreateContext},
    {"sceNpMatching2ContextStart",trace_sceNpMatching2ContextStart},
    {"sceNpMatching2CreateJoinRoom",trace_sceNpMatching2CreateJoinRoom},
    {"sceNpMatching2JoinRoom",trace_sceNpMatching2JoinRoom},
    {"sceNpMatching2SearchRoom",trace_sceNpMatching2SearchRoom},
    {"sceNpMatching2GetWorldInfoList",trace_sceNpMatching2GetWorldInfoList},
    {"sceRudpInit",trace_sceRudpInit}, {"sceRudpCreateContext",trace_sceRudpCreateContext},
    {"sceRudpBind",trace_sceRudpBind},
    {"sceRudpInitiate",trace_sceRudpInitiate}, {"sceRudpRead",trace_sceRudpRead},
    {"sceRudpProcessEvents",trace_sceRudpProcessEvents},
    {"sceRudpNetFlush",trace_sceRudpNetFlush},
    {"sceRudpNetReceived",trace_sceRudpNetReceived},
    {"sceRudpPollWait",trace_sceRudpPollWait},
    {"sceRudpWrite",trace_sceRudpWrite},
};
static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceSystemServiceGetDisplaySafeAreaInfo",safe_area}, {"sceSystemServiceDisableMusicPlayer",disable_music_player},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_socket}, {"sceNetConnect",net_connect}, {"sceNetBind",net_ok},
    {"sceNetListen",net_ok}, {"sceNetAccept",net_would_block}, {"sceNetSend",net_send},
    {"sceNetSendto",net_send}, {"sceNetRecv",net_would_block}, {"sceNetRecvfrom",net_would_block},
    {"sceNetSetsockopt",net_ok}, {"sceNetGetsockopt",net_getsockopt},
    {"sceNetGetsockname",net_getsockname}, {"sceNetShutdown",net_ok},
    {"sceNetSocketClose",net_ok}, {"sceNetSocketAbort",net_ok},
    {"sceNetEpollControl",net_ok}, {"sceNetEpollWait",net_epoll_wait}, {"sceNetEpollAbort",net_ok},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_online_id}, {"sceNpGetNpId",np_id},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_manager_create}, {"sceNpDeleteRequest",np_manager_delete}, {"sceNpAbortRequest",np_manager_abort},
    {"sceNpPollAsync",np_manager_poll}, {"sceNpCheckNpAvailability",np_check_availability},
    {"sceNpGetParentalControlInfo",np_parental_info}, {"sceNpCheckPlus",np_check_plus},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_auth_create}, {"sceNpAuthDeleteRequest",np_auth_delete},
    {"sceNpAuthPollAsync",np_auth_poll}, {"sceNpAuthGetAuthorizationCode",np_auth_get_code},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogProgressBarSetValue",msg_progress},
    {"sceMsgDialogGetResult",msg_result}, {"sceMsgDialogGetStatus",msg_status},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress}, {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state}, {"sceNpTrophyAbortHandle",trophy_release},
    {"sceNpTrophyDestroyHandle",trophy_release}, {"sceNpTrophyDestroyContext",trophy_release},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetProgress",playgo_progress},
    {"scePlayGoSetToDoList",playgo_todo}, {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
    {"sceVoiceResetPort",ok_void}, {"sceVoiceSetVolume",ok_void},
    {"sceSystemServiceShowDisplaySafeAreaSettings",ok_void}, {"sceKernelGetOpenPsId",open_psid},
    {"sceNetCtlInit",ok_void}, {"sceNetCtlTerm",ok_void},
    {"sceNetHtonll",net_htonll}, {"sceNetNtohll",net_htonll}, {"sceNetGetSockInfo",net_sock_info},
    {"socket",posix_unreachable}, {"setsockopt",posix_unreachable}, {"bind",posix_unreachable},
    {"connect",posix_unreachable}, {"listen",posix_unreachable}, {"accept",posix_unreachable},
    {"send",posix_unreachable}, {"sendto",posix_unreachable}, {"recv",posix_unreachable},
    {"recvfrom",posix_unreachable}, {"getsockname",posix_unreachable}, {"getpeername",posix_unreachable},
    {"sceHttpsLoadCert",ok_void}, {"sceHttpSetAuthInfoCallback",ok_void}, {"sceHttpSetRecvTimeOut",ok_void},
    {"sceHttpsSetSslCallback",ok_void}, {"sceHttpGetAllResponseHeaders",http_fail},
    {"sceNpGetAccountId",np_account_id}, {"sceNpGetAccountCountry",np_account_country},
    {"sceNpGetAccountLanguage",np_account_language}, {"sceNpAuthAbortRequest",np_auth_abort},
    {"sceNpWebApiCreateHandle",http_object}, {"sceNpWebApiDeleteHandle",ok_void},
    {"sceNpWebApiAddHttpRequestHeader",np_signed_out},
    {"sceNpWebApiCreateServicePushEventFilter",np_signed_out}, {"sceNpWebApiDeleteServicePushEventFilter",ok_void},
    {"sceNpWebApiRegisterServicePushEventCallback",np_signed_out},
    {"sceNpWebApiUnregisterServicePushEventCallback",ok_void},
    {"sceNpWordFilterCreateTitleCtx",word_filter_ctx}, {"sceNpWordFilterDeleteTitleCtx",ok_void},
    {"sceNpWordFilterCreateAsyncRequest",word_filter_request}, {"sceNpWordFilterDeleteRequest",ok_void},
    {"sceNpWordFilterAbortRequest",ok_void}, {"sceNpWordFilterPollAsync",word_filter_poll},
    {"sceNpWordFilterCensorComment",word_filter_censor}, {"sceNpWordFilterSanitizeComment",word_filter_sanitize},
    {"sceNpBandwidthTestInitStart",ok_void}, {"sceNpBandwidthTestAbort",ok_void},
    {"sceNpBandwidthTestShutdown",ok_void}, {"sceNpBandwidthTestGetStatus",ok_void},
    /* Party and RUDP (peer-to-peer play) accept calls; no party or peer ever appears. */
    {"sceNpPartyInitialize",ok_void}, {"sceNpPartyTerminate",ok_void}, {"sceNpPartyCreate",ok_void},
    {"sceNpPartyRegisterHandler",ok_void}, {"sceNpPartyCheckCallback",ok_void},
    {"sceNpPartySendBinaryMessage",ok_void}, {"sceNpPartyShowInvitationList",ok_void},
    {"sceRudpInit",ok_void}, {"sceRudpEnd",ok_void}, {"sceRudpEnableInternalIOThread",ok_void},
    {"sceRudpCreateContext",ok_void}, {"sceRudpTerminate",ok_void}, {"sceRudpBind",ok_void},
    {"sceRudpInitiate",ok_void}, {"sceRudpGetContextStatus",ok_void}, {"sceRudpGetStatus",ok_void},
    {"sceRudpGetOption",ok_void}, {"sceRudpSetOption",ok_void}, {"sceRudpSetMaxSegmentSize",ok_void},
    {"sceRudpSetEventHandler",ok_void}, {"sceRudpProcessEvents",ok_void},
    {"sceRudpNetFlush",ok_void}, {"sceRudpNetReceived",ok_void},
    {"sceRudpPollCreate",ok_void}, {"sceRudpPollControl",ok_void}, {"sceRudpPollWait",ok_void},
    {"sceRudpPollDestroy",ok_void}, {"sceRudpGetNumberOfPacketsToRead",ok_void},
    {"sceRudpGetSizeReadable",ok_void}, {"sceRudpGetSizeWritable",ok_void},
    {"sceRudpRead",ok_void}, {"sceRudpWrite",ok_void},
    {"sceNpCommerceDialogClose",commerce_close},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogUpdateStatus",error_status}, {"sceErrorDialogClose",error_close},
    {"sceErrorDialogTerminate",error_term},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpen",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogClose",invitation_close},
    {"sceInvitationDialogTerminate",invitation_term},
    {"sceWebBrowserDialogInitialize",browser_init}, {"sceWebBrowserDialogOpen",browser_open},
    {"sceWebBrowserDialogUpdateStatus",browser_status}, {"sceWebBrowserDialogClose",browser_close},
    {"sceWebBrowserDialogTerminate",browser_term},
    {"scePlayGoGetEta",playgo_eta}, {"scePlayGoGetLanguageMask",playgo_get_languages},
    {"scePlayGoSetLanguageMask",playgo_set_languages}, {"scePlayGoPrefetch",playgo_prefetch},
    {"scePlayGoGetToDoList",playgo_get_todo},
};
uintptr_t runtime_services_resolve(const char *name) {
    const char *trace=getenv("KZSF_NET_TRACE");
    if (trace && !strcmp(trace,"1")) { /* opt-in: the campaign log stays clean */
        uintptr_t wrapper=RUNTIME_LOOKUP(trace_exports,name);
        if (wrapper) return wrapper;
    }
    return RUNTIME_LOOKUP(exports,name);
}
