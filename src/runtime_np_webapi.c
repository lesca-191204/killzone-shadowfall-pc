/* Local WebAPI object lifecycle for the observed Killzone init sequence.
 * This does not sign into PSN or claim support for remote requests/push events. */
#include "runtime.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_INVALID_ID ((int32_t)0x80550015)
#define NP_REQUEST_MAX ((int32_t)0x80550013)
/* Private adapter errors; no claim that these are Sony WebAPI error numbers. */
#define PRIVATE_UNSUPPORTED (-1)
#define PRIVATE_BUSY (-2)
enum { WEB_OBJECT_CAP=32, LIBRARY=1, CONTEXT=2, HANDLE=3, PUSH_FILTER=4, PUSH_CALLBACK=5 };
typedef struct { char data_type[65]; } WebPushParameter;
typedef ABI void (*WebPushCallback)(void); /* Stored only; no synthetic event dispatch. */
_Static_assert(sizeof(WebPushParameter)==65,"WebAPI push-filter parameter ABI size");
typedef struct {
    int id, kind, parent;
    char online_id[17];
    char data_type[65];
    int filter;
    WebPushCallback callback;
    void *callback_arg;
} WebObject;
static WebObject objects[WEB_OBJECT_CAP];
static pthread_mutex_t web_lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=0x4e500000;

static int enabled(void) {
    const char *value=getenv("KZSF_PRIVATE_HTTP");
    return value && !strcmp(value,"http://127.0.0.1:17800");
}
/* Called only with web_lock held. IDs are never reused, including after terminate. */
static WebObject *find_object(int id, int kind) {
    for (size_t i=0;i<WEB_OBJECT_CAP;++i)
        if (objects[i].kind==kind && objects[i].id==id) return &objects[i];
    return NULL;
}
static WebObject *new_object(int kind, int parent) {
    if (next_id==INT_MAX) return NULL;
    for (size_t i=0;i<WEB_OBJECT_CAP;++i) if (!objects[i].kind) {
        WebObject *object=&objects[i];
        memset(object,0,sizeof(*object));
        object->id=++next_id; object->kind=kind; object->parent=parent;
        return object;
    }
    return NULL;
}
static ABI int web_initialize(int net_pool, uint64_t pool_size) {
    if (!enabled()) return NP_SIGNED_OUT;
    if (net_pool<0 || !pool_size || pool_size>16*1024*1024) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=new_object(LIBRARY,0);
    int result=object?object->id:NP_REQUEST_MAX;
    pthread_mutex_unlock(&web_lock);
    fprintf(stderr,"NPWEBAPI_PRIVATE initialize result=%d\n",result);
    return result;
}
/* This eboot's legacy ABI takes a pointer to NpOnlineId, not an integer userId.
 * Caller 0x49628b zeroes 20 bytes and copies the first 16 identity characters. */
static ABI int web_context(int library, const void *online_id) {
    if (!enabled()) return NP_SIGNED_OUT;
    if (!online_id) return NP_INVALID_ARGUMENT;
    const unsigned char *input=online_id;
    if (!input[0]) return NP_INVALID_ARGUMENT;
    char name[17]={0};
    for (size_t i=0;i<16 && input[i];++i) {
        if (input[i]<0x20 || input[i]>0x7e) return NP_INVALID_ARGUMENT;
        name[i]=(char)input[i];
    }
    pthread_mutex_lock(&web_lock);
    int result=NP_INVALID_ID;
    if (find_object(library,LIBRARY)) {
        WebObject *object=new_object(CONTEXT,library);
        if (object) { memcpy(object->online_id,name,sizeof(name)); result=object->id; }
        else result=NP_REQUEST_MAX;
    }
    pthread_mutex_unlock(&web_lock);
    fprintf(stderr,"NPWEBAPI_PRIVATE create_context result=%d\n",result);
    return result;
}
static ABI int web_handle(int library) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    int result=NP_INVALID_ID;
    if (find_object(library,LIBRARY)) {
        WebObject *object=new_object(HANDLE,library);
        result=object?object->id:NP_REQUEST_MAX;
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static ABI int web_delete_context(int context) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=find_object(context,CONTEXT);
    int result=object?0:NP_INVALID_ID;
    if (object) {
        for (size_t i=0;i<WEB_OBJECT_CAP;++i)
            if (objects[i].kind==PUSH_CALLBACK && objects[i].parent==context) result=PRIVATE_BUSY;
        if (!result) memset(object,0,sizeof(*object));
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
/* This local registry implements the observed basic subscription lifecycle only.
 * No network connection, callback invocation, or incoming PSN event is simulated. */
static int push_trace(const char *operation,int result) {
    const char *trace=getenv("KZSF_NET_TRACE");
    if (trace && !strcmp(trace,"1"))
        fprintf(stderr,"NPWEBAPI_PRIVATE %s result=%d\n",operation,result);
    return result;
}
static ABI int web_push_filter(int library,const WebPushParameter *parameter,uint64_t count) {
    if (!enabled()) return NP_SIGNED_OUT;
    /* Guest 0x496bc7 supplies exactly one fixed 65-byte data-type record. */
    if (!parameter || count!=1 || !parameter->data_type[0]) return NP_INVALID_ARGUMENT;
    const char *end=memchr(parameter->data_type,0,sizeof(parameter->data_type));
    if (!end) return NP_INVALID_ARGUMENT;
    size_t length=(size_t)(end-parameter->data_type);
    for (size_t i=0;i<length;++i)
        if ((unsigned char)parameter->data_type[i]<0x20 ||
            (unsigned char)parameter->data_type[i]>0x7e) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&web_lock);
    int result=NP_INVALID_ID;
    if (find_object(library,LIBRARY)) {
        WebObject *object=new_object(PUSH_FILTER,library);
        if (object) { memcpy(object->data_type,parameter->data_type,length+1); result=object->id; }
        else result=NP_REQUEST_MAX;
    }
    pthread_mutex_unlock(&web_lock);
    return push_trace("create_push_filter",result);
}
static ABI int web_register_push(int context,int filter,WebPushCallback callback,void *arg) {
    if (!enabled()) return NP_SIGNED_OUT;
    if (!callback) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&web_lock);
    WebObject *owner=find_object(context,CONTEXT),*subscription=find_object(filter,PUSH_FILTER);
    int result=NP_INVALID_ID;
    if (owner && subscription && owner->parent==subscription->parent) {
        WebObject *object=new_object(PUSH_CALLBACK,context);
        if (object) {
            object->filter=filter; object->callback=callback; object->callback_arg=arg;
            result=object->id;
        } else result=NP_REQUEST_MAX;
    }
    pthread_mutex_unlock(&web_lock);
    return push_trace("register_push_callback",result);
}
static ABI int web_unregister_push(int context,int callback) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=find_object(callback,PUSH_CALLBACK);
    int result=NP_INVALID_ID;
    if (find_object(context,CONTEXT) && object && object->parent==context) {
        memset(object,0,sizeof(*object)); result=0;
    }
    pthread_mutex_unlock(&web_lock);
    return push_trace("unregister_push_callback",result);
}
static ABI int web_delete_push_filter(int library,int filter) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=find_object(filter,PUSH_FILTER);
    int result=NP_INVALID_ID;
    if (find_object(library,LIBRARY) && object && object->parent==library) {
        result=0;
        for (size_t i=0;i<WEB_OBJECT_CAP;++i)
            if (objects[i].kind==PUSH_CALLBACK && objects[i].filter==filter) result=PRIVATE_BUSY;
        if (!result) memset(object,0,sizeof(*object));
    }
    pthread_mutex_unlock(&web_lock);
    return push_trace("delete_push_filter",result);
}
static ABI int web_delete_handle(int library, int handle) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=find_object(handle,HANDLE);
    int result=NP_INVALID_ID;
    if (find_object(library,LIBRARY) && object && object->parent==library) {
        memset(object,0,sizeof(*object)); result=0;
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static ABI int web_terminate(int library) {
    if (!enabled()) return NP_SIGNED_OUT;
    pthread_mutex_lock(&web_lock);
    WebObject *object=find_object(library,LIBRARY);
    int result=object?0:NP_INVALID_ID;
    if (object) {
        for (size_t i=0;i<WEB_OBJECT_CAP;++i)
            if (objects[i].kind && objects[i].parent==library) result=PRIVATE_BUSY;
        if (!result) memset(object,0,sizeof(*object));
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
/* Diagnostics omit query/fragment and control bytes. Never print a request body. */
static void trace_part(char *output, size_t capacity, const char *input) {
    size_t length=0;
    if (input) while(length+1<capacity) {
        unsigned char c=(unsigned char)input[length];
        if (!c || c=='?' || c=='#' || c<0x20 || c>0x7e) break;
        output[length++]=(char)c;
    }
    output[length]=0;
}
/* Observed ABI: context, API group, path, method, optional 32-byte params, u64*out.
 * No endpoint has been verified yet. Failure leaves the output ID untouched. */
static ABI int web_request(int context, const char *group, const char *path,
                           int method, const void *params, uint64_t *out) {
    (void)params;
    if (!enabled()) return NP_SIGNED_OUT;
    if (!group || !path || !out || method<0 || method>3) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&web_lock);
    int valid=find_object(context,CONTEXT)!=NULL;
    pthread_mutex_unlock(&web_lock);
    if (!valid) return NP_INVALID_ID;
    char group_text[65],path_text[129];
    trace_part(group_text,sizeof(group_text),group);
    trace_part(path_text,sizeof(path_text),path);
    fprintf(stderr,"NPWEBAPI_PRIVATE unsupported_request group=%s path=%s method=%d\n",
            group_text,path_text,method);
    return PRIVATE_UNSUPPORTED;
}

uintptr_t runtime_np_webapi_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"sceNpWebApiInitialize",web_initialize},
        {"sceNpWebApiCreateContext",web_context},
        {"sceNpWebApiCreateHandle",web_handle},
        {"sceNpWebApiDeleteContext",web_delete_context},
        {"sceNpWebApiDeleteHandle",web_delete_handle},
        {"sceNpWebApiTerminate",web_terminate},
        {"sceNpWebApiCreatePushEventFilter",web_push_filter},
        {"sceNpWebApiRegisterPushEventCallback",web_register_push},
        {"sceNpWebApiUnregisterPushEventCallback",web_unregister_push},
        {"sceNpWebApiDeletePushEventFilter",web_delete_push_filter},
        {"sceNpWebApiCreateRequest",web_request}
    };
    if (!name || !enabled()) return 0;
    return runtime_lookup(exports,sizeof(exports)/sizeof(exports[0]),name);
}
