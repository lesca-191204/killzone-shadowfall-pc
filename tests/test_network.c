/* Two real loopback UDP endpoints, guest sockaddr layout and mixed API calls. */
#undef NDEBUG
#include "../src/runtime_network.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static __thread int32_t test_errno;
int32_t *runtime_errno(void) { return &test_errno; }
#ifndef _WIN32
int32_t runtime_guest_errno(int error) {
    switch (error) {
        case EAGAIN: return 35;
        case EADDRINUSE: return 48;
        case EADDRNOTAVAIL: return 49;
        case ENETUNREACH: return 51;
        case ECONNREFUSED: return 61;
        default: return error;
    }
}
#endif
uintptr_t runtime_lookup(const RuntimeExport *t,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(t[i].name,name)) return (uintptr_t)t[i].function;
    return 0;
}
static int fallback_close_count;
static ABI int64_t file_close_test(int fd) { assert(fd==7); ++fallback_close_count; return 0; }
static ABI int32_t sce_option_test(int fd,int level,int option,const void *data,uint32_t size) {
    (void)level; (void)option; (void)data; (void)size; assert(fd==17); return 123;
}
uintptr_t runtime_file_resolve(const char *name) { return !strcmp(name,"bY-PO6JhzhQ#libkernel") ? (uintptr_t)file_close_test : 0; }
uintptr_t runtime_services_resolve(const char *name) { return !strcmp(name,"2mKX2Spso7I#libSceNet") ? (uintptr_t)sce_option_test : 0; }
static void env(const char *name,const char *value) {
#ifdef _WIN32
    assert(_putenv_s(name,value)==0);
#else
    assert(setenv(name,value,1)==0);
#endif
}
static void test_sleep(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay={milliseconds/1000,(long)(milliseconds%1000)*1000000}; nanosleep(&delay,NULL);
#endif
}
typedef int32_t (ABI *SceWait)(int,void *,int,int32_t);
typedef struct { SceWait wait; int poll,result; } WaitTest;
static void *waiting_thread(void *arg) {
    WaitTest *test=arg; unsigned char events[24];
    test->result=test->wait(test->poll,events,1,1000000); return NULL;
}
static void test_private_sce(void) {
    typedef int32_t (ABI *SceSocket)(const char *,int,int,int);
    typedef int32_t (ABI *SceBind)(int,const void *,uint32_t);
    typedef int32_t (ABI *SceName)(int,void *,uint32_t *);
    typedef int32_t (ABI *SceSend)(int,const void *,uint64_t,int,const void *,uint32_t);
    typedef int32_t (ABI *SceRecv)(int,void *,uint64_t,int,void *,uint32_t *);
    typedef int32_t (ABI *SceOption)(int,int,int,const void *,uint32_t);
    typedef int32_t (ABI *SceGetOption)(int,int,int,void *,uint32_t *);
    typedef int32_t (ABI *SceClose)(int);
    typedef int32_t (ABI *ScePollCreate)(const char *,int);
    typedef int32_t (ABI *ScePollControl)(int,int,int,const void *);
    typedef int32_t (ABI *ScePollAbort)(int,int);
    typedef int32_t *(ABI *SceErrno)(void);
    env("KZSF_PRIVATE_HTTP",""); assert(!runtime_network_resolve("sceNetSocket"));
    env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800/"); assert(!runtime_network_resolve("sceNetSocket"));
    env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    SceSocket create=(SceSocket)runtime_network_resolve("sceNetSocket"); assert(create);
    SceBind bind_sce=(SceBind)runtime_network_resolve("sceNetBind");
    SceName name=(SceName)runtime_network_resolve("sceNetGetsockname");
    SceSend send_sce=(SceSend)runtime_network_resolve("sceNetSendto");
    SceRecv recv_sce=(SceRecv)runtime_network_resolve("sceNetRecvfrom");
    SceOption option=(SceOption)runtime_network_resolve("sceNetSetsockopt");
    SceGetOption get_option=(SceGetOption)runtime_network_resolve("sceNetGetsockopt");
    SceClose close_sce=(SceClose)runtime_network_resolve("sceNetSocketClose");
    ScePollCreate poll_create=(ScePollCreate)runtime_network_resolve("sceNetEpollCreate");
    ScePollControl control=(ScePollControl)runtime_network_resolve("sceNetEpollControl");
    SceWait wait=(SceWait)runtime_network_resolve("sceNetEpollWait");
    SceClose destroy=(SceClose)runtime_network_resolve("sceNetEpollDestroy");
    ScePollAbort abort_poll=(ScePollAbort)runtime_network_resolve("sceNetEpollAbort");
    SceErrno error=(SceErrno)runtime_network_resolve("sceNetErrnoLoc");
    assert(bind_sce&&name&&send_sce&&recv_sce&&option&&get_option&&close_sce);
    assert(poll_create&&control&&wait&&destroy&&abort_poll&&error&&error()==runtime_errno());
    assert((uint32_t)create("unsupported",2,6,0)==0x8041012bu);
    int a=create("ping-a",2,2,0),b=create("ping-b",2,2,17); assert(a>0&&b>a);
    int one=1,setting=65536,actual=0; uint32_t size=4;
    assert(option(a,0xffff,0x1001,&setting,4)==0);
    assert(option(a,0xffff,0x1002,&setting,4)==0);
    assert(get_option(a,0xffff,0x1002,&actual,&size)==0&&size==4&&actual>=setting);
    assert(option(a,0xffff,0x1200,&one,4)==0&&option(a,0,0x43,&one,4)==0);
    assert((uint32_t)option(a,0xffff,0xdead,&one,4)==0x8041012au);
    unsigned char address[16]={16,2,0,0,127,0,0,1},source[16],buffer[32];
    size=16; assert(bind_sce(b,address,16)==0&&name(b,address,&size)==0&&size==16);
    assert(runtime_network_prepare_udp(b)==0); /* Preserve an existing bind. */
    unsigned char preserved[16];uint32_t preserved_size=16;
    assert(name(b,preserved,&preserved_size)==0&&!memcmp(preserved,address,16));
    assert((uint32_t)recv_sce(b,buffer,sizeof buffer,0,NULL,NULL)==0x80410123u&&*error()==35);
    int poll=poll_create("ping-test",0); assert(poll>0);
    unsigned char event[24]={0},received[25]; uint32_t mask=0x19;
    memcpy(event,&mask,4); memcpy(event+16,&b,4); event[8]=0x7b;
    assert(control(poll,1,b,event)==0);
    assert((uint32_t)control(poll,1,b,event)==0x80410111u);
    memset(received,0xa5,sizeof received); assert(wait(poll,received,1,0)==0&&received[0]==0xa5);
    const char request[24]="RTT 7";
    assert(send_sce(a,request,sizeof request,0,address,16)==24);
    assert(wait(poll,received,1,500000)==1);
    uint32_t ready;int ready_fd; memcpy(&ready,received,4);memcpy(&ready_fd,received+16,4);
    assert(ready==1&&ready_fd==b&&received[8]==0x7b&&received[24]==0xa5);
    size=16;assert(recv_sce(b,buffer,sizeof buffer,0,source,&size)==24&&!memcmp(buffer,request,24));
    assert(source[4]==127&&source[7]==1&&wait(poll,received,1,0)==0);
    uint16_t wake_port=0;
    assert(runtime_network_wake_udp(b,request,sizeof request,&wake_port)==24);
    assert(wake_port==((uint16_t)address[2]<<8|address[3]));
    assert(wait(poll,received,1,500000)==1);
    uint32_t wake_source_size=16;unsigned char wake_source[16];
    assert(recv_sce(b,buffer,sizeof buffer,0,wake_source,&wake_source_size)==24&&!memcmp(buffer,request,24));
    assert(!memcmp(wake_source,address,16)&&wait(poll,received,1,0)==0);
    assert(runtime_network_wake_udp(b,request,65,&wake_port)<0);
    assert(runtime_network_wake_udp(b,NULL,24,&wake_port)<0);
    env("KZSF_PRIVATE_HTTP","");assert(runtime_network_wake_udp(b,request,24,&wake_port)<0);
    env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    assert(send_sce(b,buffer,24,0,source,16)==24);
    for(int i=0;i<100;++i){int n=recv_sce(a,buffer,sizeof buffer,0,NULL,NULL);if(n==24)break;assert(n<0&&i<99);test_sleep(1);}
    assert(!memcmp(buffer,request,24));
    unsigned char remote[16]={16,2,0x45,0x8a,192,0,2,1};
    assert((uint32_t)send_sce(a,request,24,0,remote,16)==0x8041010du&&*error()==13);
    assert((uint32_t)send_sce(a,request,24,0,address,1)==0x80410116u);
    assert((uint32_t)wait(poll,NULL,1,0)==0x8041010eu);
    assert((uint32_t)wait(poll,received,0,0)==0x80410116u);
    assert((uint32_t)abort_poll(poll,1)==0x80410116u);
    WaitTest pending={wait,poll,0}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,waiting_thread,&pending));test_sleep(20);
    assert(abort_poll(poll,0)==0);assert(!pthread_join(thread,NULL));assert((uint32_t)pending.result==0x80410104u);
    assert(wait(poll,received,1,0)==0);
    assert(control(poll,3,b,NULL)==0&&control(poll,1,b,event)==0);
    assert(close_sce(b)==0&&wait(poll,received,1,0)==0);
    assert((uint32_t)close_sce(b)==0x80410109u&&(uint32_t)control(poll,1,b,event)==0x80410109u);
    int replacement=create("replacement",2,2,0);assert(replacement>b);assert(close_sce(replacement)==0);
    pending.result=0;assert(!pthread_create(&thread,NULL,waiting_thread,&pending));test_sleep(20);
    assert(destroy(poll)==0);assert(!pthread_join(thread,NULL));assert((uint32_t)pending.result==0x80410109u);
    assert((uint32_t)destroy(poll)==0x80410109u&&(uint32_t)abort_poll(poll,0)==0x80410109u);
    int polls[32],count=0;while(count<32){int id=poll_create("capacity",0);if(id<0)break;polls[count++]=id;}
    assert(count>0&&count<32&&*error()==24);for(int i=0;i<count;++i)assert(destroy(polls[i])==0);
    env("KZSF_PRIVATE_HTTP","");assert((uint32_t)create("disabled",2,2,0)==0x8041010du);
    env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");assert(close_sce(a)==0);
    puts("private SceNet UDP/epoll: passed");
}
int main(void) {
    typedef int32_t (ABI *Socket)(int,int,int);
    typedef int32_t (ABI *Bind)(int,const void *,uint32_t);
    typedef int32_t (ABI *Name)(int,void *,uint32_t *);
    typedef int64_t (ABI *Send)(int,const void *,uint64_t,int,const void *,uint32_t);
    typedef int64_t (ABI *Recv)(int,void *,uint64_t,int,void *,uint32_t *);
    typedef int32_t (ABI *Option)(int,int,int,const void *,uint32_t);
    typedef int32_t (ABI *Info)(int,void *,int,int);
    typedef int32_t (ABI *Close)(int);
    env("KZSF_PROBE_UDP","0"); assert(!runtime_network_resolve("socket"));
    env("KZSF_PROBE_UDP","1"); env("KZSF_NET_TRACE","0");
    Socket create=(Socket)runtime_network_resolve("socket"); assert(create);
    Bind bind_udp=(Bind)runtime_network_resolve("bind");
    Name local=(Name)runtime_network_resolve("getsockname");
    Send send_udp=(Send)runtime_network_resolve("sendto");
    Recv recv_udp=(Recv)runtime_network_resolve("recvfrom");
    Option option=(Option)runtime_network_resolve("setsockopt");
    Option sce_option=(Option)runtime_network_resolve("sceNetSetsockopt");
    Info info=(Info)runtime_network_resolve("sceNetGetSockInfo");
    Close close_udp=(Close)runtime_network_resolve("close");
    assert(bind_udp && local && send_udp && recv_udp && option && sce_option && info && close_udp);
    assert(create(28,2,0)==-1 && test_errno==47);
    assert(create(2,1,0)==-1 && test_errno==43);
    int a=create(2,2,0),b=create(2,2,17); assert(a>=0x10000 && b>a);
    unsigned char address[16]={16,2,0,0,127,0,0,1},peer[16],buffer[32],metadata[160];
    uint32_t size=sizeof(address);
    assert(bind_udp(b,address,sizeof(address))==0);
    assert(local(b,address,&size)==0 && size==16 && (address[2] || address[3]));
    assert(recv_udp(b,buffer,sizeof(buffer),0,NULL,NULL)==-1 && test_errno==35);
    int one=1,zero=0;
    assert(sce_option(a,0xffff,0x1200,&one,4)==0);
    assert(option(a,0xffff,0x20,&one,4)==0);
    assert(option(a,0xffff,0x1234,&one,4)==-1 && test_errno==42);
    assert(option(a,0xffff,0x1200,&zero,4)==-1 && test_errno==45);
    assert(sce_option(17,0xffff,0x1200,&one,4)==123);
    assert(send_udp(a,"hello",5,0,address,16)==5);
    uint32_t pending=0;
    for (int i=0;i<100 && !pending;++i) {
        memset(metadata,0xa5,sizeof(metadata)); assert(info(b,metadata,1,0)==1);
        memcpy(&pending,metadata+44,4);
#ifdef _WIN32
        if (!pending) Sleep(1);
#else
        if (!pending) { struct timespec delay={0,1000000}; nanosleep(&delay,NULL); }
#endif
    }
    if (pending!=5) fprintf(stderr,"Loopback packet not queued: port=%u pending=%u errno=%d\n",(address[2]<<8)|address[3],pending,test_errno);
    assert(pending==5 && metadata[48]==0xa5);
    size=sizeof(peer); assert(recv_udp(b,buffer,sizeof(buffer),0,peer,&size)==5);
    assert(!memcmp(buffer,"hello",5) && size==16 && peer[0]==16 && peer[1]==2);
    assert(send_udp(b,"back",4,0,peer,16)==4);
    int64_t received=-1;
    for (int i=0;i<100 && received<0;++i) {
        received=recv_udp(a,buffer,sizeof(buffer),0,NULL,NULL);
#ifdef _WIN32
        if (received<0) Sleep(1);
#else
        if (received<0) { struct timespec delay={0,1000000}; nanosleep(&delay,NULL); }
#endif
    }
    assert(received==4 && !memcmp(buffer,"back",4));
    assert(send_udp(a,"truncate",8,0,address,16)==8);
    received=-1;
    for (int i=0;i<100 && received<0;++i) {
        received=recv_udp(b,buffer,3,0,NULL,NULL);
#ifdef _WIN32
        if (received<0) Sleep(1);
#else
        if (received<0) { struct timespec delay={0,1000000}; nanosleep(&delay,NULL); }
#endif
    }
    assert(received==3 && !memcmp(buffer,"tru",3));
    assert(send_udp(a,"x",1,1,address,16)==-1 && test_errno==45);
    assert(send_udp(a,"x",1,0,address,1)==-1 && test_errno==22);
    assert(send_udp(a,NULL,1,0,address,16)==-1 && test_errno==14);
    assert(close_udp(a)==0 && close_udp(a)==-1 && test_errno==9);
    assert(send_udp(a,"x",1,0,address,16)==-1 && test_errno==9);
    assert(close_udp(b)==0 && close_udp(7)==0 && fallback_close_count==1);
    puts("network loopback: passed");
    test_private_sce();
    return 0;
}
