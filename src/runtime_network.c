/* Opt-in IPv4 UDP for the game's mixed POSIX/SceNet discovery path. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET HostSocket;
typedef int HostLength;
#define HOST_INVALID INVALID_SOCKET
#define host_close closesocket
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
typedef int HostSocket;
typedef socklen_t HostLength;
#define HOST_INVALID (-1)
#define host_close close
#endif

enum { UDP_FD_BASE=0x10000, UDP_CAPACITY=64, EPOLL_CAPACITY=8, EPOLL_BASE=0x20000000 };
typedef struct { int fd; HostSocket socket; } UdpSocket;
typedef struct { int fd; unsigned char event[24]; } UdpPollMember;
typedef struct { int id,aborted; uint64_t generation; UdpPollMember members[UDP_CAPACITY]; } UdpPoll;
static UdpSocket udp_sockets[UDP_CAPACITY];
static UdpPoll udp_polls[EPOLL_CAPACITY];
static pthread_mutex_t udp_lock=PTHREAD_MUTEX_INITIALIZER;
static int next_udp_fd=UDP_FD_BASE;
static int next_poll_id=EPOLL_BASE;
#ifdef _WIN32
static pthread_once_t winsock_once=PTHREAD_ONCE_INIT;
static int winsock_error;
static void udp_start(void) { WSADATA data; winsock_error=WSAStartup(MAKEWORD(2,2),&data); }
#endif

static int udp_error(void) {
#ifdef _WIN32
    int error=WSAGetLastError();
    return error>=10000 && error<10100 ? error-10000 : 5;
#else
    return runtime_guest_errno(errno);
#endif
}
static int64_t udp_fail(int error) { *runtime_errno()=error; return -1; }
static int64_t udp_trace(const char *name,int fd,int64_t result) {
    const char *enabled=getenv("KZSF_NET_TRACE");
    if (enabled && !strcmp(enabled,"1")) /* opt-in: the campaign log stays clean */
        fprintf(stderr,"NETTRACE %s fd=%d result=%lld errno=%d\n",name,fd,(long long)result,result<0 ? *runtime_errno() : 0);
    return result;
}
/* The lock covers each nonblocking host call so close cannot recycle a live handle. */
static UdpSocket *udp_get(int fd) {
    pthread_mutex_lock(&udp_lock);
    for (size_t i=0;i<UDP_CAPACITY;++i) if (udp_sockets[i].fd==fd && fd>=UDP_FD_BASE) return &udp_sockets[i];
    pthread_mutex_unlock(&udp_lock); udp_fail(9); return NULL;
}
static int64_t udp_done(const char *name,int fd,int64_t result) {
    if (result<0) udp_fail(udp_error());
    pthread_mutex_unlock(&udp_lock);
    return udp_trace(name,fd,result);
}
static int udp_address(const void *guest,uint32_t size,struct sockaddr_in *host) {
    if (!guest) return (int)udp_fail(14);
    if (size<16) return (int)udp_fail(22);
    const unsigned char *bytes=guest;
    if (bytes[1]!=2) return (int)udp_fail(47);
    /* This title leaves sin_len uninitialized on sendto; addrlen is authoritative. */
    memset(host,0,sizeof(*host)); host->sin_family=AF_INET;
    memcpy(&host->sin_port,bytes+2,2); memcpy(&host->sin_addr,bytes+4,4);
    return 0;
}
static void udp_guest_address(void *guest,uint32_t *size,const struct sockaddr_in *host) {
    unsigned char bytes[16]={16,2};
    memcpy(bytes+2,&host->sin_port,2); memcpy(bytes+4,&host->sin_addr,4);
    memcpy(guest,bytes,*size<16 ? *size : 16); *size=16;
}
static ABI int32_t udp_socket(int family,int type,int protocol) {
    if (family!=2) return (int32_t)udp_trace("socket",-1,udp_fail(47));
    if (type!=2) return (int32_t)udp_trace("socket",-1,udp_fail(43));
    if (protocol!=0 && protocol!=17) return (int32_t)udp_trace("socket",-1,udp_fail(43));
#ifdef _WIN32
    pthread_once(&winsock_once,udp_start);
    if (winsock_error) return (int32_t)udp_trace("socket",-1,udp_fail(5));
#endif
    HostSocket host=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (host==HOST_INVALID) return (int32_t)udp_trace("socket",-1,udp_fail(udp_error()));
#ifdef _WIN32
    u_long nonblock=1; int result=ioctlsocket(host,FIONBIO,&nonblock);
#else
    int result=fcntl(host,F_SETFL,O_NONBLOCK);
#endif
    if (result<0) { int error=udp_error(); host_close(host); return (int32_t)udp_trace("socket",-1,udp_fail(error)); }
    pthread_mutex_lock(&udp_lock);
    for (size_t i=0;i<UDP_CAPACITY && next_udp_fd<EPOLL_BASE;++i) if (!udp_sockets[i].fd) {
        int fd=next_udp_fd++; udp_sockets[i]=(UdpSocket){fd,host};
        pthread_mutex_unlock(&udp_lock); return (int32_t)udp_trace("socket",fd,fd);
    }
    pthread_mutex_unlock(&udp_lock); host_close(host); return (int32_t)udp_trace("socket",-1,udp_fail(24));
}
static ABI int32_t udp_bind(int fd,const void *address,uint32_t size) {
    struct sockaddr_in host;
    if (udp_address(address,size,&host)<0) return (int32_t)udp_trace("bind",fd,-1);
    UdpSocket *s=udp_get(fd); if (!s) return (int32_t)udp_trace("bind",fd,-1);
    return (int32_t)udp_done("bind",fd,bind(s->socket,(struct sockaddr *)&host,sizeof(host)));
}
static ABI int32_t udp_getsockname(int fd,void *address,uint32_t *size) {
    if (!address || !size) return (int32_t)udp_trace("getsockname",fd,udp_fail(14));
    struct sockaddr_in host; HostLength length=sizeof(host);
    UdpSocket *s=udp_get(fd); if (!s) return (int32_t)udp_trace("getsockname",fd,-1);
    int result=getsockname(s->socket,(struct sockaddr *)&host,&length);
    if (!result) udp_guest_address(address,size,&host);
    return (int32_t)udp_done("getsockname",fd,result);
}
static ABI int64_t udp_sendto(int fd,const void *data,uint64_t size,int flags,const void *address,uint32_t length) {
    if (!data && size) return udp_trace("sendto",fd,udp_fail(14));
    if (size>65507) return udp_trace("sendto",fd,udp_fail(40));
    if (flags!=0 && flags!=0x80) return udp_trace("sendto",fd,udp_fail(45));
    struct sockaddr_in host;
    if (udp_address(address,length,&host)<0) return udp_trace("sendto",fd,-1);
    UdpSocket *s=udp_get(fd); if (!s) return udp_trace("sendto",fd,-1);
    return udp_done("sendto",fd,sendto(s->socket,data ? (const char *)data : "",(int)size,0,(struct sockaddr *)&host,sizeof(host)));
}
static ABI int64_t udp_recvfrom(int fd,void *data,uint64_t size,int flags,void *address,uint32_t *length) {
    if ((!data && size) || (address && !length)) return udp_trace("recvfrom",fd,udp_fail(14));
    if (size>INT_MAX) return udp_trace("recvfrom",fd,udp_fail(22));
    if (flags & ~(0x80|2)) return udp_trace("recvfrom",fd,udp_fail(45));
    UdpSocket *s=udp_get(fd); if (!s) return udp_trace("recvfrom",fd,-1);
    struct sockaddr_in host; HostLength host_length=sizeof(host); char empty;
    int64_t result=recvfrom(s->socket,data ? (char *)data : &empty,(int)size,(flags&2) ? MSG_PEEK : 0,(struct sockaddr *)&host,&host_length);
#ifdef _WIN32
    /* Winsock copies a truncated UDP datagram then reports WSAEMSGSIZE; POSIX returns bytes copied. */
    if (result<0 && WSAGetLastError()==WSAEMSGSIZE) result=(int64_t)size;
#endif
    if (result>=0 && address) udp_guest_address(address,length,&host);
    return udp_done("recvfrom",fd,result);
}
static ABI int32_t udp_setsockopt(int fd,int level,int option,const void *value,uint32_t size) {
    if (!value) return (int32_t)udp_trace("setsockopt",fd,udp_fail(14));
    if (size!=4) return (int32_t)udp_trace("setsockopt",fd,udp_fail(22));
    if (level!=0xffff || (option!=0x1200 && option!=0x20)) return (int32_t)udp_trace("setsockopt",fd,udp_fail(42));
    int setting; memcpy(&setting,value,4);
    if (option==0x1200 && setting!=1) return (int32_t)udp_trace("setsockopt",fd,udp_fail(45));
    UdpSocket *s=udp_get(fd); if (!s) return (int32_t)udp_trace("setsockopt",fd,-1);
    int result=option==0x1200 ? 0 : setsockopt(s->socket,SOL_SOCKET,SO_BROADCAST,(const char *)&setting,sizeof(setting));
    return (int32_t)udp_done("setsockopt",fd,result);
}
static ABI int32_t udp_sce_setsockopt(int fd,int level,int option,const void *value,uint32_t size) {
    if (fd<UDP_FD_BASE) {
        typedef int32_t (ABI *Legacy)(int,int,int,const void *,uint32_t);
        Legacy legacy=(Legacy)runtime_services_resolve("2mKX2Spso7I#libSceNet");
        return legacy ? legacy(fd,level,option,value,size) : (int32_t)0x80410109;
    }
    int result=udp_setsockopt(fd,level,option,value,size);
    return result<0 ? (int32_t)(0x80410100u | (uint32_t)*runtime_errno()) : result;
}
static ABI int32_t udp_sockinfo(int fd,void *info,int count,int flags) {
    if (fd<UDP_FD_BASE) {
        typedef int32_t (ABI *Legacy)(int,void *,int,int);
        Legacy legacy=(Legacy)runtime_services_resolve("hLuXdjHnhiI#libSceNet");
        return legacy ? legacy(fd,info,count,flags) : (int32_t)0x80410109;
    }
    if (!info || count!=1 || flags) { udp_fail(!info ? 14 : 22); return (int32_t)(0x80410100u | (uint32_t)*runtime_errno()); }
    UdpSocket *s=udp_get(fd); if (!s) return (int32_t)0x80410109;
#ifdef _WIN32
    u_long pending=0; int result=ioctlsocket(s->socket,FIONREAD,&pending);
#else
    int pending=0; int result=ioctl(s->socket,FIONREAD,&pending);
#endif
    if (!result) {
        /* shortcut: only recv_queue_length (+0x2c) is verified in this game's ABI; expand after a new caller requires more fields. */
        uint32_t bytes=(uint32_t)pending; memcpy((unsigned char *)info+0x2c,&bytes,4);
    }
    int error=result<0 ? udp_error() : 0;
    pthread_mutex_unlock(&udp_lock);
    /* Polls with an empty queue occur every 10ms; log data or errors only. */
    if (pending || error) udp_trace("sceNetGetSockInfo",fd,error ? udp_fail(error) : (int64_t)pending);
    return error ? (int32_t)(0x80410100u | (uint32_t)error) : 1;
}
static ABI int32_t udp_close(int fd) {
    if (fd<UDP_FD_BASE) {
        typedef int64_t (ABI *Legacy)(int);
        Legacy legacy=(Legacy)runtime_file_resolve("bY-PO6JhzhQ#libkernel");
        return legacy ? (int32_t)legacy(fd) : (int32_t)udp_fail(9);
    }
    UdpSocket *s=udp_get(fd); if (!s) return (int32_t)udp_trace("close",fd,-1);
    int result=host_close(s->socket);
    if (!result) {
        s->fd=0;
        for(size_t i=0;i<EPOLL_CAPACITY;++i)for(size_t j=0;j<UDP_CAPACITY;++j)
            if(udp_polls[i].members[j].fd==fd)memset(&udp_polls[i].members[j],0,sizeof(UdpPollMember));
    }
    return (int32_t)udp_done("close",fd,result);
}
/* Native SceNet adapters are a narrower opt-in than the existing POSIX
 * discovery path. Their real datagram destinations remain on loopback. */
static int private_enabled(void) {
    const char *http=getenv("KZSF_PRIVATE_HTTP"),*udp=getenv("KZSF_PROBE_UDP");
    return http&&!strcmp(http,"http://127.0.0.1:17800")&&udp&&!strcmp(udp,"1");
}
/* Deliver a local readiness notification to the same bound guest UDP socket.
 * The RUDP adapter owns the notification format; no game payload is injected. */
int32_t runtime_network_prepare_udp(int fd) {
    if(!private_enabled())return (int32_t)udp_fail(13);
    UdpSocket *s=udp_get(fd);if(!s)return -1;
    struct sockaddr_in address={0};HostLength length=sizeof address;
    int result=getsockname(s->socket,(struct sockaddr *)&address,&length);
    /* Winsock reports EINVAL for a newly created, unbound UDP socket. POSIX
     * reports 0.0.0.0:0. Bind only that state; preserve existing loopback binds. */
    if(result<0&&udp_error()!=22)return (int32_t)udp_done("rudp_prepare",fd,result);
    if(result==0&&address.sin_port) {
        int valid=length==sizeof address&&address.sin_family==AF_INET&&address.sin_addr.s_addr==htonl(INADDR_LOOPBACK);
        pthread_mutex_unlock(&udp_lock);return valid?0:(int32_t)udp_fail(13);
    }
    memset(&address,0,sizeof address);address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    return (int32_t)udp_done("rudp_prepare",fd,bind(s->socket,(struct sockaddr *)&address,sizeof address));
}
int32_t runtime_network_wake_udp(int fd,const void *data,uint32_t size,uint16_t *port) {
    if(!private_enabled()||!data||!size||size>64||!port)return (int32_t)udp_fail(22);
    UdpSocket *s=udp_get(fd);if(!s)return -1;
    struct sockaddr_in destination;HostLength length=sizeof destination;
    int result=getsockname(s->socket,(struct sockaddr *)&destination,&length);
    if(result<0)return (int32_t)udp_done("rudp_wake",fd,result);
    if(length!=sizeof destination||destination.sin_family!=AF_INET||
       destination.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)||!destination.sin_port) {
        pthread_mutex_unlock(&udp_lock);return (int32_t)udp_fail(22);
    }
    result=sendto(s->socket,data,(int)size,0,(struct sockaddr *)&destination,sizeof destination);
    if(result==(int)size)*port=ntohs(destination.sin_port);
    return (int32_t)udp_done("rudp_wake",fd,result);
}
static int32_t sce_result(int64_t result) {
    return result<0?(int32_t)(0x80410100u|(uint32_t)*runtime_errno()):(int32_t)result;
}
static int32_t sce_fail(int error) { return sce_result(udp_fail(error)); }
static ABI int32_t *private_errno(void) { return runtime_errno(); }
static ABI int32_t private_socket(const char *name,int family,int type,int protocol) {
    (void)name;if(!private_enabled())return sce_fail(13);
    return sce_result(udp_socket(family,type,protocol));
}
static ABI int32_t private_bind(int fd,const void *address,uint32_t size) {
    if(!private_enabled())return sce_fail(13);
    struct sockaddr_in host;if(udp_address(address,size,&host)<0)return sce_result(-1);
    if(host.sin_addr.s_addr==htonl(INADDR_ANY))host.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(host.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))return sce_fail(13);
    UdpSocket *s=udp_get(fd);if(!s)return sce_result(-1);
    return sce_result(udp_done("sceNetBind",fd,bind(s->socket,(struct sockaddr *)&host,sizeof host)));
}
static ABI int32_t private_name(int fd,void *address,uint32_t *size) {
    if(!private_enabled())return sce_fail(13);
    return sce_result(udp_getsockname(fd,address,size));
}
static ABI int32_t private_sendto(int fd,const void *data,uint64_t size,int flags,const void *address,uint32_t length) {
    if(!private_enabled())return sce_fail(13);
    struct sockaddr_in host;if(udp_address(address,length,&host)<0)return sce_result(-1);
    if(host.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))return sce_fail(13);
    return sce_result(udp_sendto(fd,data,size,flags,address,length));
}
static ABI int32_t private_recvfrom(int fd,void *data,uint64_t size,int flags,void *address,uint32_t *length) {
    if(!private_enabled())return sce_fail(13);
    return sce_result(udp_recvfrom(fd,data,size,flags,address,length));
}
static ABI int32_t private_close(int fd) {
    if(!private_enabled())return sce_fail(13);
    if(fd<UDP_FD_BASE)return sce_fail(9);
    return sce_result(udp_close(fd));
}
static ABI int32_t private_shutdown(int fd,int how) {
    if(!private_enabled())return sce_fail(13);
    if(how<0||how>2)return sce_fail(22);
    UdpSocket *s=udp_get(fd);if(!s)return sce_result(-1);
    return sce_result(udp_done("sceNetShutdown",fd,shutdown(s->socket,how)));
}
static ABI int32_t private_option(int fd,int level,int option,const void *value,uint32_t size) {
    if(!private_enabled())return sce_fail(13);
    if(!value)return sce_fail(14);
    if(size!=4)return sce_fail(22);
    int setting;memcpy(&setting,value,4);
    if(level==0xffff&&(option==0x1200||option==0x20))return sce_result(udp_setsockopt(fd,level,option,value,size));
    int host_level,host_option;
    if(level==0xffff&&(option==0x1001||option==0x1002)) {
        if(setting<=0)return sce_fail(22);
        host_level=SOL_SOCKET;host_option=option==0x1001?SO_SNDBUF:SO_RCVBUF;
    }else if(level==0&&option==0x43) {
        if(setting!=0&&setting!=1)return sce_fail(22);
        host_level=IPPROTO_IP;
#ifdef _WIN32
        host_option=IP_DONTFRAGMENT;
#else
        host_option=IP_MTU_DISCOVER;setting=setting?IP_PMTUDISC_DO:IP_PMTUDISC_DONT;
#endif
    }else return sce_fail(42);
    UdpSocket *s=udp_get(fd);if(!s)return sce_result(-1);
    return sce_result(udp_done("sceNetSetsockopt",fd,setsockopt(s->socket,host_level,host_option,(const char *)&setting,sizeof setting)));
}
static ABI int32_t private_get_option(int fd,int level,int option,void *value,uint32_t *size) {
    if(!private_enabled())return sce_fail(13);
    if(!value||!size)return sce_fail(14);
    if(*size<4)return sce_fail(22);
    if(level!=0xffff||(option!=0x1001&&option!=0x1002))return sce_fail(42);
    UdpSocket *s=udp_get(fd);if(!s)return sce_result(-1);
    int setting=0;HostLength length=sizeof setting;
    int result=getsockopt(s->socket,SOL_SOCKET,option==0x1001?SO_SNDBUF:SO_RCVBUF,(char *)&setting,&length);
    if(!result){memcpy(value,&setting,4);*size=4;}
    return sce_result(udp_done("sceNetGetsockopt",fd,result));
}
/* Epoll retains guest event data, never host handles. The same lock protects
 * a zero-timeout select and close; sleep happens unlocked in bounded steps. */
static UdpPoll *poll_locked(int id) {
    for(size_t i=0;i<EPOLL_CAPACITY;++i)if(udp_polls[i].id==id&&id>=EPOLL_BASE)return &udp_polls[i];
    return NULL;
}
static UdpSocket *socket_locked(int fd) {
    for(size_t i=0;i<UDP_CAPACITY;++i)if(udp_sockets[i].fd==fd&&fd>=UDP_FD_BASE)return &udp_sockets[i];
    return NULL;
}
static ABI int32_t private_poll_create(const char *name,int flags) {
    (void)name;if(!private_enabled())return sce_fail(13);
    if(flags)return sce_fail(22);
    pthread_mutex_lock(&udp_lock);
    for(size_t i=0;i<EPOLL_CAPACITY&&next_poll_id<INT_MAX;++i)if(!udp_polls[i].id) {
        UdpPoll *p=&udp_polls[i];memset(p,0,sizeof *p);p->id=next_poll_id++;
        int id=p->id;pthread_mutex_unlock(&udp_lock);return id;
    }
    pthread_mutex_unlock(&udp_lock);return sce_fail(24);
}
static ABI int32_t private_poll_control(int id,int operation,int fd,const void *event) {
    if(!private_enabled())return sce_fail(13);
    if(operation!=1&&operation!=3)return sce_fail(22);
    uint32_t mask=0;if(operation==1){if(!event)return sce_fail(14);memcpy(&mask,event,4);if(!mask||(mask&~0x19u))return sce_fail(22);}
    pthread_mutex_lock(&udp_lock);UdpPoll *p=poll_locked(id);int error=0;
    UdpPollMember *member=NULL,*free_member=NULL;
    if(!p||!socket_locked(fd))error=9;
    else {
        for(size_t i=0;i<UDP_CAPACITY;++i){if(p->members[i].fd==fd)member=&p->members[i];if(!p->members[i].fd)free_member=&p->members[i];}
        if(operation==3){if(!member)error=2;else memset(member,0,sizeof *member);}
        else if(member)error=17;
        else if(!free_member)error=28;
        else {free_member->fd=fd;memcpy(free_member->event,event,24);}
    }
    pthread_mutex_unlock(&udp_lock);return error?sce_fail(error):0;
}
static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
#endif
}
static void poll_sleep(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay={0,(long)milliseconds*1000000};nanosleep(&delay,NULL);
#endif
}
static ABI int32_t private_poll_wait(int id,void *events,int capacity,int32_t timeout_us) {
    if(!private_enabled())return sce_fail(13);
    if(!events)return sce_fail(14);
    if(capacity<=0||capacity>UDP_CAPACITY||timeout_us< -1)return sce_fail(22);
    uint64_t deadline=monotonic_ms()+(timeout_us>=0?((uint64_t)timeout_us+999)/1000:0),generation=0;
    int first=1;
    for(;;) {
        pthread_mutex_lock(&udp_lock);UdpPoll *p=poll_locked(id);
        if(!p){pthread_mutex_unlock(&udp_lock);return sce_fail(9);}
        if(first){generation=p->generation;first=0;}
        if(p->aborted||p->generation!=generation){p->aborted=0;pthread_mutex_unlock(&udp_lock);return sce_fail(4);}
        fd_set reads,errors;FD_ZERO(&reads);FD_ZERO(&errors);int members=0,maxfd=0;
        for(size_t i=0;i<UDP_CAPACITY;++i)if(p->members[i].fd) {
            UdpSocket *s=socket_locked(p->members[i].fd);if(!s)continue;
#ifndef _WIN32
            if(s->socket>=FD_SETSIZE){pthread_mutex_unlock(&udp_lock);return sce_fail(24);}
            if(s->socket>maxfd)maxfd=s->socket;
#endif
            uint32_t mask;memcpy(&mask,p->members[i].event,4);
            if(mask&1)FD_SET(s->socket,&reads);
            FD_SET(s->socket,&errors);++members;
        }
        struct timeval zero={0,0};int selected=members?select(maxfd+1,&reads,NULL,&errors,&zero):0;
        if(selected<0){int error=udp_error();pthread_mutex_unlock(&udp_lock);return sce_fail(error);}
        int count=0;
        if(selected)for(size_t i=0;i<UDP_CAPACITY&&count<capacity;++i)if(p->members[i].fd) {
            UdpSocket *s=socket_locked(p->members[i].fd);if(!s)continue;
            uint32_t ready=(FD_ISSET(s->socket,&reads)?1u:0)|(FD_ISSET(s->socket,&errors)?8u:0);
            if(ready){unsigned char *entry=(unsigned char *)events+24*count++;memcpy(entry,p->members[i].event,24);memcpy(entry,&ready,4);}
        }
        pthread_mutex_unlock(&udp_lock);if(count)return count;
        uint64_t now=monotonic_ms();if(timeout_us>=0&&now>=deadline)return 0;
        unsigned sleep_ms=timeout_us<0||deadline-now>5?5:(unsigned)(deadline-now);
        poll_sleep(sleep_ms);
        if(!private_enabled())return sce_fail(13);
    }
}
static ABI int32_t private_poll_abort(int id,int flags) {
    if(!private_enabled())return sce_fail(13);
    if(flags)return sce_fail(22);
    pthread_mutex_lock(&udp_lock);UdpPoll *p=poll_locked(id);
    if(p){p->aborted=1;++p->generation;}pthread_mutex_unlock(&udp_lock);return p?0:sce_fail(9);
}
static ABI int32_t private_poll_destroy(int id) {
    if(!private_enabled())return sce_fail(13);
    pthread_mutex_lock(&udp_lock);UdpPoll *p=poll_locked(id);
    if(p)memset(p,0,sizeof *p);pthread_mutex_unlock(&udp_lock);return p?0:sce_fail(9);
}
uintptr_t runtime_network_resolve(const char *name) {
    const char *enabled=getenv("KZSF_PROBE_UDP");
    if (!enabled || strcmp(enabled,"1")) return 0;
    if(private_enabled()) {
        static const RuntimeExport private_exports[]={
            {"sceNetSocket",private_socket},{"sceNetBind",private_bind},{"sceNetGetsockname",private_name},
            {"sceNetSendto",private_sendto},{"sceNetRecvfrom",private_recvfrom},
            {"sceNetSocketClose",private_close},{"sceNetShutdown",private_shutdown},{"sceNetErrnoLoc",private_errno},
            {"sceNetSetsockopt",private_option},{"sceNetGetsockopt",private_get_option},
            {"sceNetEpollCreate",private_poll_create},{"sceNetEpollControl",private_poll_control},
            {"sceNetEpollWait",private_poll_wait},{"sceNetEpollAbort",private_poll_abort},{"sceNetEpollDestroy",private_poll_destroy}
        };
        uintptr_t result=RUNTIME_LOOKUP(private_exports,name);if(result)return result;
    }
    static const RuntimeExport exports[]={
        {"socket",udp_socket},{"bind",udp_bind},{"getsockname",udp_getsockname},
        {"sendto",udp_sendto},{"recvfrom",udp_recvfrom},{"setsockopt",udp_setsockopt},
        {"sceNetSetsockopt",udp_sce_setsockopt},{"sceNetGetSockInfo",udp_sockinfo},{"close",udp_close}
    };
    return RUNTIME_LOOKUP(exports,name);
}
