/* Private, loopback-only RUDP API adapter. KZRP is our framing, not Sony RUDP.
 * Native Turbine messages are carried unchanged. Both channels use reliable TCP.
 * Callbacks are delivered by ProcessEvents outside the state lock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <pthread.h>

enum { CONTEXT_CAP=16, POLL_CAP=4, FRAME_HEADER=16, MAX_MESSAGE=65535,
       QUEUE_BYTES=1024*1024, QUEUE_PACKETS=256, CONNECT_MS=5000 };
#ifdef KZSF_RUDP_TEST_PORT
#define PRIVATE_PORT KZSF_RUDP_TEST_PORT
#else
#define PRIVATE_PORT 17801
#endif
#define RUDP_INVALID ((int32_t)0x80770002)
#define RUDP_UNSUPPORTED ((int32_t)0x80770004)
#define RUDP_TOO_LARGE ((int32_t)0x80770012)
#define RUDP_AGAIN ((int32_t)0x80770016)
#define RUDP_IO ((int32_t)0x80770019)
enum { UNUSED, CREATED, BOUND, CONNECTING, WAIT_ACK, CONNECTED, CLOSED };
typedef void (ABI *RudpCallback)(int,int,int,void *);
typedef int (ABI *RudpGlobal)(int,int,const void *,uint64_t,const void *,uint32_t,void *);
typedef struct Packet {
    struct Packet *next;
    uint32_t length, offset;
    unsigned char bytes[];
} Packet;
typedef struct {
    int phase, guest_socket;
    SOCKET socket;
    uint16_t local_port, remote_port;
    RudpCallback callback;
    void *arg;
    uint64_t deadline;
    uint32_t options[20], option_mask;
    Packet *tx_head,*tx_tail,*rx_head,*rx_tail;
    size_t tx_bytes,rx_bytes;
    unsigned rx_packets;
    unsigned char header[FRAME_HEADER];
    uint32_t header_read,body_read,body_length;
    Packet *incoming;
    int pending_event,pending_result,last_error;
    unsigned poll_events, callbacks_active;
    int terminating,trace_phase;
    uint64_t wake_sequence;
    uint16_t wake_port;
} RudpContext;
typedef struct { int used,capacity; uint16_t masks[CONTEXT_CAP]; } RudpPoll;
static RudpContext contexts[CONTEXT_CAP];
static RudpPoll polls[POLL_CAP];
static pthread_mutex_t rudp_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callbacks_done=PTHREAD_COND_INITIALIZER;
static __thread int callback_context=-1;
static int initialized;
static uint64_t session_nonce;
static uint64_t next_wake_sequence;
static RudpGlobal global_handler;
static void *global_arg;
static int released_sockets[CONTEXT_CAP],released_count;
static uint16_t maximum_segment=1400;
static volatile LONG progress_armed,progress_trace_count,process_trace_count,flush_trace_count;
static volatile LONG io_trace_count,count_trace_count,readable_trace_count,poll_trace_count;
static int32_t trace_io(const char *operation,int id,int32_t result,uint64_t first,uint64_t second) {
    if(InterlockedIncrement(&io_trace_count)<=128)
        fprintf(stderr,"RUDP_PRIVATE io=%s context=%d result=%d arg1=%llu arg2=%llu\n",operation,id,result,(unsigned long long)first,(unsigned long long)second);
    return result;
}
static void trace_progress(const char *event,int context,int value) {
    if(InterlockedIncrement(&progress_trace_count)<=128)
        fprintf(stderr,"RUDP_PRIVATE progress=%s context=%d value=%d thread=%lu\n",event,context,value,(unsigned long)GetCurrentThreadId());
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint16_t be16(const unsigned char *p) { return (uint16_t)((unsigned)p[0]<<8|p[1]); }
static uint64_t be64(const unsigned char *p) { return (uint64_t)be32(p)<<32|be32(p+4); }
static void put16(unsigned char *p,uint16_t v) { p[0]=(unsigned char)(v>>8);p[1]=(unsigned char)v; }
static void put32(unsigned char *p,uint32_t v) { p[0]=(unsigned char)(v>>24);p[1]=(unsigned char)(v>>16);p[2]=(unsigned char)(v>>8);p[3]=(unsigned char)v; }
static void put64(unsigned char *p,uint64_t v) { put32(p,(uint32_t)(v>>32));put32(p+4,(uint32_t)v); }
static int enabled(void) { const char *v=getenv("KZSF_PRIVATE_HTTP");return v&&!strcmp(v,"http://127.0.0.1:17800"); }
static RudpContext *find_context(int id) { return initialized&&id>=0&&id<CONTEXT_CAP&&contexts[id].phase!=UNUSED&&!contexts[id].terminating?&contexts[id]:NULL; }
static int would_block(void) { int e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS||e==WSAEALREADY; }
static void free_packets(Packet **head,Packet **tail) {
    while (*head) { Packet *p=*head;*head=p->next;free(p); } *tail=NULL;
}
static void close_transport(RudpContext *c,int event,int result) {
    if (result) fprintf(stderr,"RUDP private transport error phase=%d vport=%u host=%d\n",c->phase,c->remote_port,WSAGetLastError());
    if (c->socket!=INVALID_SOCKET) { closesocket(c->socket);c->socket=INVALID_SOCKET; }
    free_packets(&c->tx_head,&c->tx_tail);free_packets(&c->rx_head,&c->rx_tail);
    free(c->incoming);c->incoming=NULL;c->tx_bytes=c->rx_bytes=0;c->rx_packets=0;
    c->phase=CLOSED;c->last_error=result;c->poll_events|=8;
    if (!c->pending_event) { c->pending_event=event;c->pending_result=result; }
}
static int enqueue_frame(RudpContext *c,int kind,const void *body,uint32_t size,uint32_t flags) {
    size_t length=FRAME_HEADER+(size_t)size;
    if (length>QUEUE_BYTES-c->tx_bytes) return RUDP_AGAIN;
    Packet *p=malloc(sizeof(*p)+length);if (!p) return RUDP_AGAIN;
    p->next=NULL;p->length=(uint32_t)length;p->offset=0;
    memcpy(p->bytes,"KZRP",4);p->bytes[4]=1;p->bytes[5]=(unsigned char)kind;
    put16(p->bytes+6,c->remote_port);put32(p->bytes+8,size);put32(p->bytes+12,flags);
    if (size) memcpy(p->bytes+FRAME_HEADER,body,size);
    if (c->tx_tail) c->tx_tail->next=p;else c->tx_head=p;c->tx_tail=p;c->tx_bytes+=length;
    return 0;
}
static void connected_socket(RudpContext *c) {
    unsigned char hello[12]={0};uint64_t nonce=session_nonce;
    for (int i=7;i>=0;--i) { hello[i]=(unsigned char)nonce;nonce>>=8; }
    put16(hello+8,c->local_port);c->phase=WAIT_ACK;
    if (enqueue_frame(c,1,hello,sizeof hello,0)) close_transport(c,3,RUDP_IO);
    trace_progress("HELLO_QUEUED",(int)(c-contexts),c->phase);
}
static void pump_context(RudpContext *c) {
    if (c->phase<CONNECTING||c->phase>CLOSED||c->phase==CLOSED||c->terminating) return;
    if(c->trace_phase!=c->phase){c->trace_phase=c->phase;trace_progress("PUMP_PHASE",(int)(c-contexts),c->phase);}
    if ((c->phase==CONNECTING||c->phase==WAIT_ACK)&&GetTickCount64()>=c->deadline) {
        close_transport(c,3,RUDP_IO);return;
    }
    if (c->phase==CONNECTING) {
        fd_set writefds,errors;FD_ZERO(&writefds);FD_ZERO(&errors);FD_SET(c->socket,&writefds);FD_SET(c->socket,&errors);
        struct timeval zero={0,0};int ready=select(0,NULL,&writefds,&errors,&zero);
        if (ready<0) { close_transport(c,3,RUDP_IO);return; }
        if (!ready) return;
        trace_progress("TCP_READY",(int)(c-contexts),ready);
        int error=0,size=sizeof error;
        if (getsockopt(c->socket,SOL_SOCKET,SO_ERROR,(char *)&error,&size)||error||FD_ISSET(c->socket,&errors)) {
            fprintf(stderr,"RUDP connect completion ready=%d error=%d exceptional=%d\n",ready,error,FD_ISSET(c->socket,&errors));
            close_transport(c,3,RUDP_IO);return;
        }
        connected_socket(c);
    }
    /* Each pump has bounded work even if a peer continuously streams data. */
    for (unsigned step=0;step<64&&c->tx_head&&c->phase!=CLOSED;++step) {
        Packet *p=c->tx_head;uint32_t remaining=p->length-p->offset;
#ifdef KZSF_RUDP_TEST_CHUNK
        if (remaining>KZSF_RUDP_TEST_CHUNK) remaining=KZSF_RUDP_TEST_CHUNK;
#endif
        int sent=send(c->socket,(const char *)p->bytes+p->offset,(int)remaining,0);
        if(!p->offset&&p->bytes[5]==1)trace_progress("HELLO_SEND",(int)(c-contexts),sent);
        if (sent<0) { if (!would_block()) close_transport(c,3,RUDP_IO);break; }
        if (!sent) { close_transport(c,1,0);break; }
        p->offset+=(uint32_t)sent;c->tx_bytes-=(size_t)sent;
        if (p->offset==p->length) { c->tx_head=p->next;if (!c->tx_head)c->tx_tail=NULL;free(p); }
    }
    for (unsigned step=0;step<64&&c->phase!=CLOSED;++step) {
        if (c->header_read<FRAME_HEADER) {
            int n=recv(c->socket,(char *)c->header+c->header_read,FRAME_HEADER-(int)c->header_read,0);
            if (n<0) { if (!would_block())close_transport(c,3,RUDP_IO);break; }
            if (!n) { close_transport(c,1,0);break; }
            c->header_read+=(uint32_t)n;if (c->header_read<FRAME_HEADER)continue;
            c->body_length=be32(c->header+8);uint32_t flags=be32(c->header+12);
            int kind=c->header[5];
            if (memcmp(c->header,"KZRP",4)||c->header[4]!=1||be16(c->header+6)!=c->remote_port||
                c->body_length>MAX_MESSAGE||
                (kind==2&&(c->phase!=WAIT_ACK||c->body_length||flags))||
                (kind==3&&(c->phase!=CONNECTED||!c->body_length||(flags!=0x40&&flags!=0x48)))||
                (kind!=2&&kind!=3)) { close_transport(c,3,RUDP_IO);break; }
            if (kind==2) {
                c->phase=CONNECTED;c->pending_event=2;c->pending_result=0;c->poll_events|=2;
                trace_progress("ACK_RECEIVED",(int)(c-contexts),0);
                c->header_read=0;break; /* Deliver the ACK callback before reading DATA. */
            }
        }
        if (!c->incoming) {
            if (c->rx_packets>=QUEUE_PACKETS||c->body_length>QUEUE_BYTES-c->rx_bytes)break;
            c->incoming=malloc(sizeof(Packet)+c->body_length);
            if (!c->incoming) { close_transport(c,3,RUDP_IO);break; }
            c->incoming->next=NULL;c->incoming->length=c->body_length;c->incoming->offset=0;c->body_read=0;
        }
        int n=recv(c->socket,(char *)c->incoming->bytes+c->body_read,(int)(c->body_length-c->body_read),0);
        if (n<0) { if (!would_block())close_transport(c,3,RUDP_IO);break; }
        if (!n) { close_transport(c,1,0);break; }
        c->body_read+=(uint32_t)n;if (c->body_read<c->body_length)continue;
        Packet *p=c->incoming;c->incoming=NULL;
        if (c->rx_tail)c->rx_tail->next=p;else c->rx_head=p;c->rx_tail=p;
        c->rx_bytes+=p->length;++c->rx_packets;c->header_read=c->body_read=c->body_length=0;
        trace_progress("DATA_RECEIVED",(int)(c-contexts),(int)p->length);
    }
}
static ABI int32_t rudp_init(void *pool,uint32_t size) {
    if (!enabled()||!pool||!size)return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);
    if (initialized) { pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID; }
    WSADATA data;int result=WSAStartup(MAKEWORD(2,2),&data);
    if (result||BCryptGenRandom(NULL,(PUCHAR)&session_nonce,sizeof session_nonce,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) {
        if (!result)WSACleanup();
        pthread_mutex_unlock(&rudp_lock);return RUDP_IO;
    }
    if (!session_nonce)session_nonce=1;
    memset(contexts,0,sizeof contexts);memset(polls,0,sizeof polls);
    released_count=0;initialized=1;pthread_mutex_unlock(&rudp_lock);return 0;
}
static ABI int32_t rudp_create(RudpCallback callback,void *arg,int32_t *out) {
    if (!out)return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);
    if (initialized)for (int i=0;i<CONTEXT_CAP;++i)if (contexts[i].phase==UNUSED&&!contexts[i].callbacks_active) {
        RudpContext *c=&contexts[i];memset(c,0,sizeof *c);c->phase=CREATED;c->socket=INVALID_SOCKET;c->guest_socket=-1;
        c->callback=callback;c->arg=arg;c->options[1]=maximum_segment;c->options[2]=c->options[3]=QUEUE_BYTES;
        c->option_mask=(1u<<1)|(1u<<2)|(1u<<3);*out=i;pthread_mutex_unlock(&rudp_lock);return 0;
    }
    pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;
}
static ABI int32_t rudp_bind(int id,int socket,uint16_t port,int mode) {
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=RUDP_INVALID;
    if (c&&c->phase==CREATED&&socket>=0&&port&&(mode==1||mode==2)) {
        if(runtime_network_prepare_udp(socket)<0)result=RUDP_IO;
        else { c->guest_socket=socket;c->local_port=port;c->phase=BOUND;result=0; }
    }
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_initiate(int id,const void *address,uint32_t size,uint16_t port) {
    const unsigned char *a=address;
    if (!a||size!=16||a[1]!=2||memcmp(a+4,"\x7f\0\0\1",4)||be16(a+2)!=PRIVATE_PORT||(port!=20&&port!=21))return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=RUDP_INVALID;
    if (c&&c->phase==BOUND) {
        c->socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);u_long nonblock=1;int one=1;
        if (c->socket==INVALID_SOCKET||ioctlsocket(c->socket,FIONBIO,&nonblock)||
            setsockopt(c->socket,IPPROTO_TCP,TCP_NODELAY,(char *)&one,sizeof one)) { close_transport(c,3,RUDP_IO);result=RUDP_IO; }
        else {
            struct sockaddr_in endpoint={0};endpoint.sin_family=AF_INET;endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);endpoint.sin_port=htons(PRIVATE_PORT);
            c->remote_port=port;c->deadline=GetTickCount64()+CONNECT_MS;c->phase=CONNECTING;
            int status=connect(c->socket,(struct sockaddr *)&endpoint,sizeof endpoint);
            fprintf(stderr,"RUDP_PRIVATE connect context=%d channel=%u result=%d host_error=%d\n",id,port,status,status?WSAGetLastError():0);
            if (!status)connected_socket(c);else if (!would_block())close_transport(c,3,RUDP_IO);
            result=c->phase==CLOSED?RUDP_IO:0;
            if(!result)InterlockedExchange(&progress_armed,1);
        }
    }
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_process(uint64_t timeout_us) {
    (void)timeout_us; /* Nonblocking pump; the caller owns scheduling in external IO mode. */
    int trace=InterlockedCompareExchange(&progress_armed,0,0)&&InterlockedIncrement(&process_trace_count)<=8;
    if(trace)trace_progress("PROCESS_ENTER",-1,0);
    pthread_mutex_lock(&rudp_lock);int ready=initialized;
    if (ready)for (int i=0;i<CONTEXT_CAP;++i)pump_context(&contexts[i]);
    pthread_mutex_unlock(&rudp_lock);if(trace)trace_progress("PROCESS_PUMPED",-1,ready);
    if (!ready)return RUDP_INVALID;
    for (int i=0;i<CONTEXT_CAP;++i) {
        pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(i);
        RudpCallback callback=NULL;void *arg=NULL;int event=0,result=0;
        if (c&&c->pending_event) {
            event=c->pending_event;result=c->pending_result;c->pending_event=0;callback=c->callback;arg=c->arg;
            if (callback)++c->callbacks_active;
        }
        pthread_mutex_unlock(&rudp_lock);
        if (callback) {
            trace_progress("CALLBACK_ENTER",i,event);
            int previous=callback_context;callback_context=i;callback(i,event,result,arg);callback_context=previous;
            trace_progress("CALLBACK_RETURN",i,event);
            pthread_mutex_lock(&rudp_lock);--contexts[i].callbacks_active;pthread_cond_broadcast(&callbacks_done);pthread_mutex_unlock(&rudp_lock);
        }
    }
    for (;;) {
        pthread_mutex_lock(&rudp_lock);
        if (!released_count) { pthread_mutex_unlock(&rudp_lock);break; }
        int socket=released_sockets[--released_count];RudpGlobal handler=global_handler;void *arg=global_arg;
        pthread_mutex_unlock(&rudp_lock);if(handler)handler(2,socket,NULL,0,NULL,0,arg);
    }
    /* External IO drains messages only after UDP readiness -> NetReceived.
     * Signal after CONNECTED callbacks have returned, so the native context is
     * ready to write its reply. This lock serializes token consumption/close.
     * The network helper never calls RUDP or guest code while holding its lock. */
    pthread_mutex_lock(&rudp_lock);
    for(int i=0;i<CONTEXT_CAP;++i) {
        RudpContext *c=find_context(i);
        if(!c||c->phase!=CONNECTED||!c->callback||c->pending_event||c->callbacks_active||!c->rx_packets||c->wake_sequence)continue;
        unsigned char wake[24]={'K','Z','R','W',1,(unsigned char)i,0,0};
        if(!++next_wake_sequence)++next_wake_sequence;
        put64(wake+8,session_nonce);put64(wake+16,next_wake_sequence);
        int result=runtime_network_wake_udp(c->guest_socket,wake,sizeof wake,&c->wake_port);
        if(result==(int)sizeof wake)c->wake_sequence=next_wake_sequence;
        trace_progress("UDP_WAKE",i,result);
    }
    pthread_mutex_unlock(&rudp_lock);
    return 0;
}
static ABI int32_t rudp_net_received(int socket,const void *data,uint64_t size,const void *address,uint32_t length) {
    const unsigned char *wake=data,*from=address;
    if(!wake||size!=24||!from||length!=16||from[1]!=2||memcmp(from+4,"\x7f\0\0\1",4)||
       memcmp(wake,"KZRW",4)||wake[4]!=1||wake[6]||wake[7])return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(wake[5]);int result=RUDP_INVALID;
    if(c&&c->phase==CONNECTED&&c->guest_socket==socket&&c->wake_sequence&&be16(from+2)==c->wake_port&&
       be64(wake+8)==session_nonce&&be64(wake+16)==c->wake_sequence) {
        c->wake_sequence=0;result=0;
    }
    pthread_mutex_unlock(&rudp_lock);return trace_io("NetReceived",wake[5],result,(uint32_t)socket,size);
}
/* A UDP wake drains every context sharing that guest socket. Keep data hidden
 * until each context's CONNECTED callback has returned, including when a
 * different context triggers the wake or another thread runs NetFlush. */
static int read_ready(const RudpContext *c) { return !c->pending_event&&!c->callbacks_active; }
static ABI int32_t rudp_count(int id) { pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=c?(read_ready(c)?(int)c->rx_packets:0):RUDP_INVALID;pthread_mutex_unlock(&rudp_lock);return result; }
static ABI int32_t rudp_readable(int id) { pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=c?(read_ready(c)&&c->rx_head?(int)c->rx_head->length:0):RUDP_INVALID;pthread_mutex_unlock(&rudp_lock);return result; }
static ABI int32_t rudp_writable(int id) { pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=c&&c->phase==CONNECTED?(int)(QUEUE_BYTES-c->tx_bytes):RUDP_INVALID;pthread_mutex_unlock(&rudp_lock);return result; }
static ABI int32_t rudp_read(int id,void *buffer,uint64_t size,int flags,void *info) {
    if (!buffer||!size||flags||(info&&*(uint8_t *)info!=12))return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=RUDP_INVALID;
    if (c) {
        Packet *p=c->rx_head;result=RUDP_AGAIN;
        if(p&&read_ready(c)) {
            if(size<p->length)result=RUDP_TOO_LARGE;
            else {
                memcpy(buffer,p->bytes,p->length);result=(int)p->length;
                if(info){memset(info,0,12);*(uint8_t *)info=12;}
                c->rx_head=p->next;if(!c->rx_head)c->rx_tail=NULL;c->rx_bytes-=p->length;--c->rx_packets;free(p);
            }
        }
    }
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_write(int id,const void *buffer,uint64_t size,int flags) {
    if(!buffer||!size||(flags!=0x40&&flags!=0x48))return RUDP_INVALID;
    if(size>MAX_MESSAGE)return RUDP_TOO_LARGE;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=RUDP_INVALID;
    if(c&&c->phase==CONNECTED) { result=enqueue_frame(c,3,buffer,(uint32_t)size,(uint32_t)flags);if(!result)result=(int)size; }
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_terminate(int id) {
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);
    if(!c){pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;}
    int socket=c->guest_socket,shared=0;
    for(int i=0;i<CONTEXT_CAP;++i)if(i!=id&&contexts[i].phase!=UNUSED&&contexts[i].guest_socket==socket)shared=1;
    if(socket>=0&&!shared&&released_count==CONTEXT_CAP){pthread_mutex_unlock(&rudp_lock);return RUDP_AGAIN;}
    c->terminating=1;
    while(c->callbacks_active&&callback_context!=id)pthread_cond_wait(&callbacks_done,&rudp_lock);
    close_transport(c,0,0);c->pending_event=0;c->phase=UNUSED;
    for(int i=0;i<POLL_CAP;++i)polls[i].masks[id]=0;
    shared=0;for(int i=0;i<CONTEXT_CAP;++i)if(contexts[i].phase!=UNUSED&&contexts[i].guest_socket==socket)shared=1;
    if(socket>=0&&!shared&&released_count<CONTEXT_CAP)released_sockets[released_count++]=socket;
    pthread_mutex_unlock(&rudp_lock);return 0;
}
static ABI int32_t rudp_end(void) {
    pthread_mutex_lock(&rudp_lock);
    if(!initialized){pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;}
    for(int i=0;i<CONTEXT_CAP;++i)if(contexts[i].phase!=UNUSED||contexts[i].callbacks_active){pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;}
    initialized=0;global_handler=NULL;global_arg=NULL;released_count=0;WSACleanup();pthread_mutex_unlock(&rudp_lock);return 0;
}
static ABI int32_t rudp_handler(RudpGlobal handler,void *arg) {
    pthread_mutex_lock(&rudp_lock);int result=initialized?0:RUDP_INVALID;
    if(!result){global_handler=handler;global_arg=arg;}pthread_mutex_unlock(&rudp_lock);return result;
}
static int known_option(int option) { return option==1||option==2||option==3||option==5||option==6||option==11||option==18||option==19; }
static ABI int32_t rudp_set_option(int id,int option,const void *value,uint32_t size) {
    if(!known_option(option))return RUDP_UNSUPPORTED;
    if(!value||size!=4)return RUDP_INVALID;
    uint32_t setting;memcpy(&setting,value,4);
    if(setting>INT32_MAX||((option==5||option==6)&&setting>1))return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=c?0:RUDP_INVALID;
    if(c){c->options[option]=setting;c->option_mask|=1u<<option;}pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_get_option(int id,int option,void *value,uint32_t size) {
    if(!value||size!=4)return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);RudpContext *c=find_context(id);int result=RUDP_INVALID;
    if(c){result=RUDP_UNSUPPORTED;if(option==14){memcpy(value,&c->last_error,4);result=0;}
        else if(known_option(option)&&(c->option_mask&(1u<<option))){memcpy(value,&c->options[option],4);result=0;}}
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_max_segment(uint16_t size) { if(size<64)return RUDP_INVALID;pthread_mutex_lock(&rudp_lock);maximum_segment=size;pthread_mutex_unlock(&rudp_lock);return 0; }
static ABI int32_t rudp_status(void *buffer,uint32_t size) {
    if(!buffer||size!=248)return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);
    if(!initialized){pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;}
    /* Only +100 (active contexts during teardown) is verified in this title. */
    uint32_t active=0;for(int i=0;i<CONTEXT_CAP;++i)if(contexts[i].phase!=UNUSED)++active;
    memset(buffer,0,size);memcpy((unsigned char *)buffer+100,&active,4);pthread_mutex_unlock(&rudp_lock);return 0;
}
static ABI int32_t rudp_poll_create(int capacity) {
    if(capacity<=0||capacity>CONTEXT_CAP)return RUDP_INVALID;
    pthread_mutex_lock(&rudp_lock);
    if(initialized)for(int i=0;i<POLL_CAP;++i)if(!polls[i].used){memset(&polls[i],0,sizeof polls[i]);polls[i].used=1;polls[i].capacity=capacity;pthread_mutex_unlock(&rudp_lock);return i+1;}
    pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;
}
static ABI int32_t rudp_poll_control(int poll,int operation,int id,int mask) {
    pthread_mutex_lock(&rudp_lock);int result=RUDP_INVALID;
    if(poll>0&&poll<=POLL_CAP&&polls[poll-1].used&&find_context(id)&&!(mask&~11)&&(operation>=1&&operation<=3)) {
        RudpPoll *p=&polls[poll-1];int members=0;for(int i=0;i<CONTEXT_CAP;++i)if(p->masks[i])++members;
        if(operation==3){p->masks[id]=0;result=0;}
        else if(mask&&((operation==1&&!p->masks[id]&&members<p->capacity)||(operation==2&&p->masks[id]))) { p->masks[id]=(uint16_t)mask;result=0; }
    }
    pthread_mutex_unlock(&rudp_lock);return result;
}
static ABI int32_t rudp_poll_wait(int poll,void *events,int capacity,uint64_t timeout_us) {
    if(!events||capacity<=0||capacity>CONTEXT_CAP)return RUDP_INVALID;
    uint64_t wait_ms=timeout_us/1000;if(wait_ms>50)wait_ms=50;uint64_t deadline=GetTickCount64()+wait_ms;
    do {
        if(rudp_process(0)<0)return RUDP_INVALID;
        pthread_mutex_lock(&rudp_lock);
        if(poll<=0||poll>POLL_CAP||!polls[poll-1].used){pthread_mutex_unlock(&rudp_lock);return RUDP_INVALID;}
        int count=0;RudpPoll *p=&polls[poll-1];
        for(int i=0;i<CONTEXT_CAP&&count<capacity;++i)if(p->masks[i]&&contexts[i].phase!=UNUSED) {
            RudpContext *c=&contexts[i];uint16_t ready=(uint16_t)((c->poll_events|(c->rx_packets?1:0))&p->masks[i]);
            if(ready){unsigned char *entry=(unsigned char *)events+8*count++;memcpy(entry,&i,4);memcpy(entry+4,&p->masks[i],2);memcpy(entry+6,&ready,2);c->poll_events&=~(unsigned)(ready&2);}
        }
        pthread_mutex_unlock(&rudp_lock);if(count)return count;if(GetTickCount64()>=deadline)break;Sleep(1);
    }while(1);return 0;
}
static ABI int32_t rudp_poll_destroy(int poll) { pthread_mutex_lock(&rudp_lock);int result=RUDP_INVALID;if(poll>0&&poll<=POLL_CAP&&polls[poll-1].used){memset(&polls[poll-1],0,sizeof polls[0]);result=0;}pthread_mutex_unlock(&rudp_lock);return result; }
static ABI int32_t rudp_flush(void) {
    /* Turbine calls NetFlush while holding its own locks. Never invoke a guest
     * callback here; dispatch waits for the next ProcessEvents call. */
    int trace=InterlockedCompareExchange(&progress_armed,0,0)&&InterlockedIncrement(&flush_trace_count)<=8;
    if(trace)trace_progress("FLUSH_ENTER",-1,0);
    pthread_mutex_lock(&rudp_lock);int result=initialized?0:RUDP_INVALID;
    if(initialized)for(int i=0;i<CONTEXT_CAP;++i)pump_context(&contexts[i]);
    pthread_mutex_unlock(&rudp_lock);if(trace)trace_progress("FLUSH_RETURN",-1,result);return result;
}
static ABI int32_t rudp_unsupported(void) { return RUDP_UNSUPPORTED; }
/* Startup diagnostics: scalar arguments only, bounded across the process.
 * Keep polling and game packets out of this trace. */
static volatile LONG startup_trace_count;
static int32_t trace_startup(const char *operation,int id,int32_t result,uint32_t first,uint32_t second) {
    if(InterlockedIncrement(&startup_trace_count)<=128)
        fprintf(stderr,"RUDP_PRIVATE api=%s context=%d result=%d arg1=%u arg2=%u\n",operation,id,result,first,second);
    return result;
}
static ABI int32_t traced_init(void *pool,uint32_t size) {
    return trace_startup("Init",-1,rudp_init(pool,size),size,pool!=NULL);
}
static ABI int32_t traced_create(RudpCallback callback,void *arg,int32_t *out) {
    int32_t result=rudp_create(callback,arg,out);
    return trace_startup("CreateContext",result==0?*out:-1,result,callback!=NULL,0);
}
static ABI int32_t traced_bind(int id,int socket,uint16_t port,int mode) {
    return trace_startup("Bind",id,rudp_bind(id,socket,port,mode),port,(uint32_t)mode);
}
static ABI int32_t traced_initiate(int id,const void *address,uint32_t size,uint16_t port) {
    const unsigned char *a=address;
    return trace_startup("Initiate",id,rudp_initiate(id,address,size,port),port,a&&size==16?be16(a+2):0);
}
static ABI int32_t traced_handler(RudpGlobal handler,void *arg) {
    return trace_startup("SetEventHandler",-1,rudp_handler(handler,arg),handler!=NULL,0);
}
static ABI int32_t traced_max_segment(uint16_t size) {
    return trace_startup("SetMaxSegmentSize",-1,rudp_max_segment(size),size,0);
}
static ABI int32_t traced_set_option(int id,int option,const void *value,uint32_t size) {
    uint32_t setting=0;if(value&&size==4)memcpy(&setting,value,4);
    return trace_startup("SetOption",id,rudp_set_option(id,option,value,size),(uint32_t)option,setting);
}
static ABI int32_t traced_count(int id) {
    int32_t result=rudp_count(id);
    if(result||InterlockedIncrement(&count_trace_count)<=8)trace_io("Count",id,result,0,0);
    return result;
}
static ABI int32_t traced_readable(int id) {
    int32_t result=rudp_readable(id);
    if(result||InterlockedIncrement(&readable_trace_count)<=8)trace_io("Readable",id,result,0,0);
    return result;
}
static ABI int32_t traced_read(int id,void *buffer,uint64_t size,int flags,void *info) {
    uint8_t info_size=info?*(const uint8_t *)info:0;
    return trace_io("Read",id,rudp_read(id,buffer,size,flags,info),size,(uint64_t)(uint32_t)flags<<32|info_size);
}
static ABI int32_t traced_write(int id,const void *buffer,uint64_t size,int flags) {
    return trace_io("Write",id,rudp_write(id,buffer,size,flags),size,(uint32_t)flags);
}
static ABI int32_t traced_poll_create(int capacity) {
    return trace_startup("PollCreate",-1,rudp_poll_create(capacity),(uint32_t)capacity,0);
}
static ABI int32_t traced_poll_control(int poll,int operation,int id,int mask) {
    return trace_io("PollControl",id,rudp_poll_control(poll,operation,id,mask),(uint64_t)(uint32_t)poll<<32|(uint32_t)operation,(uint32_t)mask);
}
static ABI int32_t traced_poll_wait(int poll,void *events,int capacity,uint64_t timeout_us) {
    int32_t result=rudp_poll_wait(poll,events,capacity,timeout_us);
    if(result||InterlockedIncrement(&poll_trace_count)<=8)trace_io("PollWait",poll,result,(uint32_t)capacity,timeout_us);
    return result;
}
uintptr_t runtime_rudp_resolve(const char *name) {
    if(!enabled())return 0;
    static const RuntimeExport exports[]={
        {"sceRudpInit",traced_init},{"sceRudpEnd",rudp_end},{"sceRudpCreateContext",traced_create},
        {"sceRudpBind",traced_bind},{"sceRudpInitiate",traced_initiate},{"sceRudpTerminate",rudp_terminate},
        {"sceRudpSetEventHandler",traced_handler},{"sceRudpSetMaxSegmentSize",traced_max_segment},
        {"sceRudpSetOption",traced_set_option},{"sceRudpGetOption",rudp_get_option},
        {"sceRudpProcessEvents",rudp_process},{"sceRudpNetFlush",rudp_flush},
        {"sceRudpGetNumberOfPacketsToRead",traced_count},{"sceRudpGetSizeReadable",traced_readable},
        {"sceRudpGetSizeWritable",rudp_writable},{"sceRudpRead",traced_read},{"sceRudpWrite",traced_write},
        {"sceRudpGetStatus",rudp_status},{"sceRudpPollCreate",traced_poll_create},
        {"sceRudpPollControl",traced_poll_control},{"sceRudpPollWait",traced_poll_wait},{"sceRudpPollDestroy",rudp_poll_destroy},
        {"sceRudpGetContextStatus",rudp_unsupported},{"sceRudpEnableInternalIOThread",rudp_unsupported},
        {"sceRudpNetReceived",rudp_net_received}
    };
    return RUNTIME_LOOKUP(exports,name);
}
#else
uintptr_t runtime_rudp_resolve(const char *name) { (void)name;return 0; }
#endif
