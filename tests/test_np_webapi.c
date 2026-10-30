#undef NDEBUG
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-inline")
#endif
#include "../src/runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uintptr_t runtime_np_webapi_resolve(const char *);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0; i<count; ++i)
        if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
static void gate(const char *value) {
#ifdef _WIN32
    _putenv_s("KZSF_PRIVATE_HTTP", value);
#else
    setenv("KZSF_PRIVATE_HTTP", value, 1);
#endif
}
static int callback_calls;
static ABI void unused_callback(void) { ++callback_calls; }
static void check_push_lifecycle(void) {
    typedef struct { char data_type[65]; } PushParameter;
    typedef ABI void (*Callback)(void);
    int (ABI *init)(int,uint64_t)=(void *)runtime_np_webapi_resolve("sceNpWebApiInitialize");
    int (ABI *context)(int,const void *)=(void *)runtime_np_webapi_resolve("sceNpWebApiCreateContext");
    int (ABI *delete_context)(int)=(void *)runtime_np_webapi_resolve("sceNpWebApiDeleteContext");
    int (ABI *terminate)(int)=(void *)runtime_np_webapi_resolve("sceNpWebApiTerminate");
    int (ABI *filter)(int,const PushParameter *,uint64_t)=
        (void *)runtime_np_webapi_resolve("sceNpWebApiCreatePushEventFilter");
    int (ABI *register_callback)(int,int,Callback,void *)=
        (void *)runtime_np_webapi_resolve("sceNpWebApiRegisterPushEventCallback");
    int (ABI *unregister_callback)(int,int)=
        (void *)runtime_np_webapi_resolve("sceNpWebApiUnregisterPushEventCallback");
    int (ABI *delete_filter)(int,int)=
        (void *)runtime_np_webapi_resolve("sceNpWebApiDeletePushEventFilter");
    assert(filter && register_callback && unregister_callback && delete_filter);
    struct { unsigned char before; PushParameter parameter; unsigned char after; } input;
    memset(&input,0xa5,sizeof input);
    memset(&input.parameter,0,sizeof input.parameter);
    strcpy(input.parameter.data_type,"np:sessionInvitation");
    unsigned char snapshot[sizeof input]; memcpy(snapshot,&input,sizeof input);
    char online[20]="Player1";
    int lib=init(1,65536),other=init(1,65536);
    int ctx=context(lib,online),other_ctx=context(other,online);
    assert(filter(lib,NULL,1)<0 && filter(lib,&input.parameter,0)<0);
    assert(filter(lib,&input.parameter,UINT64_MAX)<0 && filter(ctx,&input.parameter,1)<0);
    PushParameter invalid={0};
    assert(filter(lib,&invalid,1)<0);
    memset(&invalid,'X',sizeof invalid); assert(filter(lib,&invalid,1)<0);
    invalid.data_type[64]=0; invalid.data_type[1]='\n';
    assert(filter(lib,&invalid,1)<0);
    int first=filter(lib,&input.parameter,1),second=filter(lib,&input.parameter,1);
    assert(first>0 && second>first && first!=ctx);
    assert(register_callback(ctx,first,NULL,NULL)<0);
    assert(register_callback(lib,first,unused_callback,NULL)<0);
    assert(register_callback(other_ctx,first,unused_callback,NULL)<0);
    assert(register_callback(ctx,ctx,unused_callback,NULL)<0);
    int cb=register_callback(ctx,first,unused_callback,&callback_calls);
    assert(cb>second);
    assert(delete_filter(lib,first)<0 && delete_context(ctx)<0 && terminate(lib)<0);
    assert(delete_filter(other,second)<0 && delete_filter(lib,cb)<0);
    assert(unregister_callback(other_ctx,cb)<0 && unregister_callback(ctx,first)<0);
    gate("");
    assert(!runtime_np_webapi_resolve("sceNpWebApiCreatePushEventFilter"));
    assert(filter(lib,&input.parameter,1)<0 && register_callback(ctx,second,unused_callback,NULL)<0);
    assert(unregister_callback(ctx,cb)<0 && delete_filter(lib,second)<0);
    gate("http://127.0.0.1:17800");
    assert(unregister_callback(ctx,cb)==0 && unregister_callback(ctx,cb)<0);
    assert(delete_filter(lib,first)==0 && delete_filter(lib,first)<0);
    assert(register_callback(ctx,first,unused_callback,NULL)<0);
    assert(delete_filter(lib,second)==0);
    int capacity_filters[128],n=0;
    while (n<128) {
        int id=filter(lib,&input.parameter,1);
        if (id<0) break;
        capacity_filters[n++]=id;
    }
    assert(n>0 && n<128);
    assert(register_callback(ctx,capacity_filters[0],unused_callback,NULL)<0);
    assert(delete_filter(lib,capacity_filters[--n])==0);
    int next=register_callback(ctx,capacity_filters[0],unused_callback,NULL);
    assert(next>cb && unregister_callback(ctx,next)==0);
    for (int i=0;i<n;++i) assert(delete_filter(lib,capacity_filters[i])==0);
    assert(delete_context(ctx)==0 && delete_context(other_ctx)==0);
    assert(terminate(lib)==0 && terminate(other)==0);
    assert(!memcmp(&input,snapshot,sizeof input));
    assert(callback_calls==0); /* Local subscriptions never invent remote events. */
}
int main(void) {
    gate(""); assert(!runtime_np_webapi_resolve("sceNpWebApiInitialize"));
    gate("http://127.0.0.1:17800.evil"); assert(!runtime_np_webapi_resolve("sceNpWebApiInitialize"));
    gate("http://127.0.0.1:17800");
    int (ABI *init)(int,uint64_t)=(void *)runtime_np_webapi_resolve("sceNpWebApiInitialize");
    int (ABI *context)(int,const void *)=(void *)runtime_np_webapi_resolve("sceNpWebApiCreateContext");
    int (ABI *handle)(int)=(void *)runtime_np_webapi_resolve("sceNpWebApiCreateHandle");
    int (ABI *delete_context)(int)=(void *)runtime_np_webapi_resolve("sceNpWebApiDeleteContext");
    int (ABI *delete_handle)(int,int)=(void *)runtime_np_webapi_resolve("sceNpWebApiDeleteHandle");
    int (ABI *terminate)(int)=(void *)runtime_np_webapi_resolve("sceNpWebApiTerminate");
    int (ABI *request)(int,const char *,const char *,int,const void *,uint64_t *)=
        (void *)runtime_np_webapi_resolve("sceNpWebApiCreateRequest");
    assert(init && context && handle && delete_context && delete_handle && terminate && request);
    assert(!runtime_np_webapi_resolve("sceNpWebApiInventedFunction"));
    assert(!runtime_np_webapi_resolve(NULL));
    char online_id[20]="KZSFPrivate01";
    assert(init(-1,65536)<0 && init(1,0)<0);
    int lib=init(1,65536), other=init(1,65536);
    assert(lib>0 && other>0 && lib!=other);
    assert(context(-1,online_id)<0 && context(lib,NULL)<0 && context(lib,"")<0);
    int ctx=context(lib,online_id), h=handle(lib);
    assert(ctx>0 && h>0 && ctx!=h && ctx!=lib);
    assert(context(ctx,online_id)<0 && handle(ctx)<0);
    assert(delete_handle(other,h)<0 && delete_context(h)<0);
    assert(terminate(lib)<0); /* Live child objects cannot silently disappear. */
    uint64_t output=UINT64_C(0x1122334455667788);
    assert(request(ctx,"sessionInvitation","/v1/users/me/sessions?query=hidden",0,NULL,&output)<0);
    assert(output==UINT64_C(0x1122334455667788));
    assert(request(h,"group","/path",0,NULL,&output)<0);
    gate(""); assert(context(lib,online_id)<0 && handle(lib)<0);
    gate("http://127.0.0.1:17800");
    assert(delete_context(ctx)==0 && delete_context(ctx)<0);
    assert(delete_handle(lib,h)==0 && delete_handle(lib,h)<0);
    char full_id[20]="1234567890123456";
    int next=context(lib,full_id); assert(next>0 && next!=ctx);
    assert(delete_context(next)==0 && terminate(lib)==0 && terminate(lib)<0);
    assert(context(lib,online_id)<0);
    int handles[128], count=0;
    while(count<128) { int value=handle(other); if(value<0) break; handles[count++]=value; }
    assert(count>0 && count<128);
    for(int i=0;i<count;++i) assert(delete_handle(other,handles[i])==0);
    assert(terminate(other)==0);
    int fresh=init(1,65536); assert(fresh>0 && fresh!=lib && fresh!=other);
    assert(terminate(fresh)==0);
    check_push_lifecycle();
    puts("Private NP WebAPI object lifecycle: ok");
    return 0;
}
