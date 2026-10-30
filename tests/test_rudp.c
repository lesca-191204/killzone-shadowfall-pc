#undef NDEBUG
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-inline")
#endif
#include "../src/runtime_network.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

extern uintptr_t runtime_rudp_resolve(const char *);
static __thread int32_t test_errno;
int32_t *runtime_errno(void) { return &test_errno; }
uintptr_t runtime_file_resolve(const char *name) { (void)name;return 0; }
uintptr_t runtime_services_resolve(const char *name) { (void)name;return 0; }
#ifndef KZSF_RUDP_TEST_PORT
#define KZSF_RUDP_TEST_PORT 17801
#endif
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
typedef void (ABI *ContextCallback)(int,int,int,void *);
typedef int (ABI *GlobalCallback)(int,int,const void *,uint64_t,const void *,uint32_t,void *);
static int (ABI *init)(void *,uint32_t);
static int (ABI *end)(void);
static int (ABI *create)(ContextCallback,void *,int *);
static int (ABI *bind_context)(int,int,uint16_t,int);
static int (ABI *initiate)(int,const void *,uint32_t,uint16_t);
static int (ABI *process)(uint64_t);
static int (ABI *count_packets)(int);
static int (ABI *readable)(int);
static int (ABI *writable)(int);
static int (ABI *read_packet)(int,void *,uint64_t,int,void *);
static int (ABI *write_packet)(int,const void *,uint64_t,int);
static int (ABI *terminate_context)(int);
static int (ABI *set_handler)(GlobalCallback,void *);
static int (ABI *get_status)(void *,uint32_t);
static int (ABI *set_option)(int,int,const void *,uint32_t);
static int (ABI *get_option)(int,int,void *,uint32_t);
static int (ABI *poll_create)(int);
static int (ABI *poll_control)(int,int,int,int);
static int (ABI *poll_wait)(int,void *,int,uint64_t);
static int (ABI *poll_destroy)(int);
static int (ABI *net_received)(int,const void *,uint64_t,const void *,uint32_t);
static int guest_socket,udp_poll;
static int connected,closed,errors,released;
static ABI void callback(int context,int event,int result,void *arg) {
    fprintf(stderr,"test RUDP callback context=%d event=%d result=%x\n",context,event,(unsigned)result);
    assert(arg==(void *)(uintptr_t)42);
    /* A callback must be able to reenter an API without a global-lock deadlock. */
    assert(writable(context)>=0 || event!=2);
    if (event==2) { assert(!result); ++connected; }
    else if (event==1) { assert(!result); ++closed; }
    else if (event==3) { assert(result<0); ++errors; }
    else assert(0);
}
static ABI int global_callback(int event,int socket,const void *data,uint64_t length,
                               const void *address,uint32_t address_length,void *arg) {
    (void)data;(void)length;(void)address;(void)address_length;
    assert(event==2 && socket==guest_socket && arg==(void *)(uintptr_t)43);
    ++released; return 0;
}
static void pump_until(int *value,int target) {
    ULONGLONG deadline=GetTickCount64()+5000;
    while (*value<target && GetTickCount64()<deadline) { assert(process(0)==0); Sleep(1); }
    assert(*value==target);
}
static ABI void callback_with_early_data(int context,int event,int result,void *arg) {
    (void)arg;assert(event==2&&result==0);
    /* Simulate a concurrent NetFlush/pump before this callback returns. Native
     * UDP wake handling can drain every context sharing the socket. */
    for(int i=0;i<20;++i){assert(process(0)==0);Sleep(1);}
    unsigned char events[24],data[16];
    assert(count_packets(context)==0&&readable(context)==0);
    assert(read_packet(context,data,sizeof data,0,NULL)==(int)0x80770016);
    assert(private_poll_wait(udp_poll,events,1,0)==0);
    ++connected;
}
int main(void) {
    _putenv_s("KZSF_PRIVATE_HTTP",""); assert(!runtime_rudp_resolve("sceRudpInit"));
    _putenv_s("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800.evil"); assert(!runtime_rudp_resolve("sceRudpInit"));
    _putenv_s("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    _putenv_s("KZSF_PROBE_UDP","1");_putenv_s("KZSF_NET_TRACE","0");
#define RESOLVE(variable,name) do { variable=(void *)runtime_rudp_resolve(name); assert(variable); } while (0)
    RESOLVE(init,"sceRudpInit"); RESOLVE(end,"sceRudpEnd"); RESOLVE(create,"sceRudpCreateContext");
    RESOLVE(bind_context,"sceRudpBind"); RESOLVE(initiate,"sceRudpInitiate"); RESOLVE(process,"sceRudpProcessEvents");
    RESOLVE(count_packets,"sceRudpGetNumberOfPacketsToRead"); RESOLVE(readable,"sceRudpGetSizeReadable");
    RESOLVE(writable,"sceRudpGetSizeWritable"); RESOLVE(read_packet,"sceRudpRead"); RESOLVE(write_packet,"sceRudpWrite");
    RESOLVE(terminate_context,"sceRudpTerminate"); RESOLVE(set_handler,"sceRudpSetEventHandler");
    RESOLVE(get_status,"sceRudpGetStatus"); RESOLVE(set_option,"sceRudpSetOption"); RESOLVE(get_option,"sceRudpGetOption");
    RESOLVE(poll_create,"sceRudpPollCreate"); RESOLVE(poll_control,"sceRudpPollControl");
    RESOLVE(poll_wait,"sceRudpPollWait"); RESOLVE(poll_destroy,"sceRudpPollDestroy");
    RESOLVE(net_received,"sceRudpNetReceived");
    char pool[4096]; int context=-1,option=1346,read_option=0;
    assert(init(pool,sizeof(pool))==0 && init(pool,sizeof(pool))<0);
    /* The native external-IO receive path requires a real UDP readiness event
     * before calling NetReceived and then draining the RUDP message queues. */
    guest_socket=private_socket("rudp-wakeup-test",2,2,0);assert(guest_socket>0);
    udp_poll=private_poll_create("rudp-wakeup-test",0);assert(udp_poll>0);
    unsigned char udp_event[24]={1};memcpy(udp_event+16,&guest_socket,4);
    assert(private_poll_control(udp_poll,1,guest_socket,udp_event)==0);
    assert(set_handler(global_callback,(void *)(uintptr_t)43)==0);
    assert(create(callback,(void *)(uintptr_t)42,&context)==0 && context>=0);
    assert(set_option(context,1,&option,4)==0 && get_option(context,1,&read_option,4)==0 && read_option==option);
    assert(set_option(context,999,&option,4)<0);
    assert(bind_context(context,guest_socket,100,2)==0);
    unsigned char local[16]={0};uint32_t local_size=16;
    assert(udp_getsockname(guest_socket,local,&local_size)==0&&local_size==16);
    assert(local[4]==127&&local[7]==1&&(local[2]||local[3]));
    unsigned char address[16]={16,2,KZSF_RUDP_TEST_PORT>>8,KZSF_RUDP_TEST_PORT&255,127,0,0,1};
    address[4]=8; assert(initiate(context,address,16,20)<0); address[4]=127;
    assert(initiate(context,address,16,20)==0 && !connected);
    pump_until(&connected,1);
    assert(write_packet(context,"ping",4,0x40)==4);
    ULONGLONG deadline=GetTickCount64()+5000;
    while (count_packets(context)<2 && GetTickCount64()<deadline) { assert(process(0)==0); Sleep(1); }
    assert(count_packets(context)==2 && readable(context)==4);
    assert(private_poll_wait(udp_poll,udp_event,1,500000)==1);
    unsigned char wake[64],source[16];uint32_t source_size=16;
    int wake_size=private_recvfrom(guest_socket,wake,sizeof wake,0,source,&source_size);
    assert(wake_size==24&&source_size==16);
    unsigned char altered[24];memcpy(altered,wake,24);altered[8]^=1;
    assert(net_received(guest_socket,altered,24,source,16)<0);
    assert(net_received(guest_socket+1,wake,24,source,16)<0);
    assert(net_received(guest_socket,wake,23,source,16)<0);
    source[2]^=1;assert(net_received(guest_socket,wake,24,source,16)<0);source[2]^=1;
    source[4]=8;assert(net_received(guest_socket,wake,24,source,16)<0);source[4]=127;
    assert(net_received(guest_socket,wake,24,source,16)==0);
    assert(net_received(guest_socket,wake,24,source,16)<0); /* Reject replay. */
    unsigned char info[12]={12}; char data[8]={0};
    assert(read_packet(context,data,2,0,info)<0 && count_packets(context)==2);
    assert(read_packet(context,data,8,0,info)==4 && !memcmp(data,"pong",4));
    assert(info[0]==12 && readable(context)==3 && read_packet(context,data,8,0,info)==3 && !memcmp(data,"two",3));
    assert(read_packet(context,data,8,0,info)==(int)0x80770016);
    assert(process(0)==0&&private_poll_wait(udp_poll,udp_event,1,0)==0);
    unsigned char *large=malloc(65535); assert(large); memset(large,0x5a,65535);
    int accepted=0,result=0;
    while ((result=write_packet(context,large,65535,0x48))>=0 && accepted<100) ++accepted;
    assert(accepted>0 && accepted<100 && result==(int)0x80770016);
    free(large);
    assert(terminate_context(context)==0 && terminate_context(context)<0);
    assert(process(0)==0 && released==1);
    unsigned char status[248]; memset(status,0xcc,sizeof(status));
    assert(get_status(status,sizeof(status))==0 && *(uint32_t *)(status+100)==0);
    /* Second connection: malformed length is a bounded protocol error. */
    assert(create(callback,(void *)(uintptr_t)42,&context)==0 && bind_context(context,guest_socket,101,2)==0);
    assert(initiate(context,address,16,21)==0);
    pump_until(&connected,2); pump_until(&errors,1);
    assert(count_packets(context)==0 && terminate_context(context)==0 && process(0)==0);
    /* Third connection: a normal remote close raises event 1, not success-data. */
    assert(create(callback,(void *)(uintptr_t)42,&context)==0 && bind_context(context,guest_socket,102,2)==0);
    assert(initiate(context,address,16,20)==0);
    pump_until(&connected,3); pump_until(&closed,1);
    assert(terminate_context(context)==0 && process(0)==0);
    /* Poll output uses eight-byte entries, revents at +6. */
    assert(create(NULL,NULL,&context)==0 && bind_context(context,guest_socket,103,2)==0);
    int poll=poll_create(4); assert(poll>0 && poll_control(poll,1,context,11)==0);
    assert(initiate(context,address,16,21)==0);
    unsigned char events[32]={0}; int events_count=0;
    deadline=GetTickCount64()+5000;
    while (!events_count && GetTickCount64()<deadline) { events_count=poll_wait(poll,events,4,0); assert(events_count>=0); Sleep(1); }
    assert(events_count==1 && *(int *)events==context && (*(uint16_t *)(events+6)&2));
    assert(poll_control(poll,2,context,9)==0 && poll_destroy(poll)==0);
    assert(terminate_context(context)==0 && process(0)==0);
    /* Early DATA is retained but cannot wake/read until CONNECTED returns. */
    assert(create(callback_with_early_data,NULL,&context)==0&&bind_context(context,guest_socket,104,2)==0);
    assert(initiate(context,address,16,20)==0);pump_until(&connected,4);
    assert(count_packets(context)==1&&private_poll_wait(udp_poll,udp_event,1,500000)==1);
    source_size=16;wake_size=private_recvfrom(guest_socket,wake,sizeof wake,0,source,&source_size);
    assert(wake_size==24&&net_received(guest_socket,wake,24,source,16)==0);
    assert(read_packet(context,data,sizeof data,0,NULL)==5&&!memcmp(data,"early",5));
    assert(terminate_context(context)==0&&process(0)==0);
    assert(private_poll_destroy(udp_poll)==0&&private_close(guest_socket)==0);
    assert(end()==0 && end()<0);
    puts("Private RUDP framed TCP: ok");
    return 0;
}
