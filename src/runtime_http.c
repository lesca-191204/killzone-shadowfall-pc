/* Private-service HTTP adapter. The diagnostic endpoint is deliberately loopback-only. */
#include "runtime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <pthread.h>

#define HTTP_INVALID ((int32_t)0x80431077)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_LIMIT ((int32_t)0x80431022)
#define PRIVATE_BODY_LIMIT (4u*1024u*1024u)
#define PRIVATE_REQUEST_MS 5000u
enum { PRIVATE_CTX=1,PRIVATE_TEMPLATE,PRIVATE_CONNECTION,PRIVATE_REQUEST,PRIVATE_EPOLL };
typedef struct { char name[64],value[1024]; int mode; } PrivateHeader;
typedef struct {
    int id,kind,parent,method,complete,event,status;
    uintptr_t epoll;
    void *user;
    uint64_t content_length;
    HINTERNET session;
    char path[2048];
    PrivateHeader headers[16];
    unsigned header_count;
    char *response_headers;
    unsigned char *body;
    size_t size,offset;
} PrivateHttp;
typedef struct { uint32_t events,detail; int32_t id; uint32_t pad; void *user; } PrivateHttpEvent;
_Static_assert(sizeof(PrivateHttpEvent)==24,"HTTP event ABI");
static PrivateHttp private_objects[64];
static int private_next_id=0x30000001;
static pthread_mutex_t private_lock=PTHREAD_MUTEX_INITIALIZER;

static int private_enabled(void) {
    const char *endpoint=getenv("KZSF_PRIVATE_HTTP");
    return endpoint && !strcmp(endpoint,"http://127.0.0.1:17800");
}
static int private_rejected_url(const char *url) {
    const char *trace=getenv("KZSF_NET_TRACE");
    if (private_enabled() && trace && !strcmp(trace,"1")) {
        char route[193]; size_t count=0;
        if (!url) url="(null)";
        while (count<sizeof route-1 && url[count] && url[count]!='?' && url[count]!='#') {
            unsigned char ch=(unsigned char)url[count];
            route[count++]=ch<32 || ch==127 ? '_' : (char)ch;
        }
        route[count]=0;
        char *scheme=strstr(route,"://");
        if (scheme) {
            char *authority=scheme+3,*end=strchr(authority,'/'),*userinfo=NULL;
            if (!end && count==sizeof route-1) strcpy(route,"(oversize authority)");
            else {
                if (!end) end=route+count;
                for (char *p=authority;p<end;++p) if (*p=='@') userinfo=p;
                if (userinfo) memmove(authority,userinfo+1,strlen(userinfo+1)+1);
            }
        }
        fprintf(stderr,"PRIVATEHTTP rejected-url=%s\n",route);
    }
    return HTTP_NETWORK;
}
static const char *private_path(const char *url) {
    static const char origin[]="https://killzone4.online.scee.com";
    if (!url || strncmp(url,origin,sizeof origin-1) || strpbrk(url,"\r\n\\#")) return NULL;
    const char *path=url+sizeof origin-1;
    if (!*path) return "/";
    return *path=='/' && strlen(path)<sizeof(private_objects[0].path) ? path : NULL;
}
static PrivateHttp *private_find(int id,int kind) {
    for (unsigned i=0;i<64;i++) if (private_objects[i].id==id && (!kind || private_objects[i].kind==kind)) return &private_objects[i];
    return NULL;
}
static PrivateHttp *private_new(int kind,int parent) {
    if (private_next_id==INT32_MAX) return NULL;
    for (unsigned i=0;i<64;i++) if (!private_objects[i].id) {
        PrivateHttp *o=&private_objects[i]; memset(o,0,sizeof *o);
        o->id=private_next_id++; o->kind=kind; o->parent=parent; return o;
    }
    return NULL;
}
static int private_result(PrivateHttp *o) { int result=o ? o->id : HTTP_LIMIT; pthread_mutex_unlock(&private_lock); return result; }
static int private_unlock(int result) { pthread_mutex_unlock(&private_lock); return result; }
static ABI int private_init(int pool,int ssl,uint64_t size) {
    (void)pool; (void)ssl; if (!size) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    PrivateHttp *o=private_new(PRIVATE_CTX,0);
    if (o) {
        o->session=WinHttpOpen(L"KZSF-Private/1",WINHTTP_ACCESS_TYPE_NO_PROXY,NULL,NULL,WINHTTP_FLAG_ASYNC);
        if (!o->session) { o->id=0; return private_unlock(HTTP_NETWORK); }
        WinHttpSetTimeouts(o->session,2000,2000,2000,2000);
    }
    return private_result(o);
}
static ABI int private_template(int ctx,const char *agent,int version,int proxy) {
    (void)version; (void)proxy; if (!agent) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    if (!private_find(ctx,PRIVATE_CTX)) return private_unlock(HTTP_INVALID);
    return private_result(private_new(PRIVATE_TEMPLATE,ctx));
}
static ABI int private_connection(int tmpl,const char *url,int keepalive) {
    (void)keepalive;
    if (!private_path(url)) return private_rejected_url(url);
    pthread_mutex_lock(&private_lock);
    if (!private_find(tmpl,PRIVATE_TEMPLATE)) return private_unlock(HTTP_INVALID);
    return private_result(private_new(PRIVATE_CONNECTION,tmpl));
}
static ABI int private_request(int conn,int method,const char *url,uint64_t size) {
    const char *path=private_path(url);
    if (!path) return private_rejected_url(url);
    if ((method!=0 && method!=1) || size>PRIVATE_BODY_LIMIT) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    if (!private_find(conn,PRIVATE_CONNECTION)) return private_unlock(HTTP_INVALID);
    PrivateHttp *o=private_new(PRIVATE_REQUEST,conn);
    if (o) { strcpy(o->path,path); o->method=method; o->content_length=size; }
    return private_result(o);
}
static ABI int private_header(int id,const char *name,const char *value,int mode) {
    if (!name || !*name || !value || strlen(name)>=64 || strlen(value)>=1024 || (mode!=0 && mode!=1)) return HTTP_INVALID;
    if (strpbrk(name,"\r\n: \t") || strpbrk(value,"\r\n")) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    PrivateHttp *o=private_find(id,0);
    if (!o || o->kind<PRIVATE_TEMPLATE || o->kind>PRIVATE_REQUEST || o->complete) return private_unlock(HTTP_INVALID);
    unsigned slot=o->header_count;
    if (!mode) for (unsigned i=0;i<o->header_count;i++) if (!_stricmp(o->headers[i].name,name)) { slot=i; break; }
    if (slot==16) return private_unlock(HTTP_LIMIT);
    if (slot==o->header_count) o->header_count++;
    strcpy(o->headers[slot].name,name); strcpy(o->headers[slot].value,value); o->headers[slot].mode=mode;
    return private_unlock(0);
}
static int private_add_headers(HINTERNET request,PrivateHttp *o) {
    if (o->kind!=PRIVATE_TEMPLATE && !private_add_headers(request,private_find(o->parent,0))) return 0;
    for (unsigned i=0;i<o->header_count;i++) {
        char line[1100]; wchar_t wide[1100];
        snprintf(line,sizeof line,"%s: %s",o->headers[i].name,o->headers[i].value);
        if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line,-1,wide,1100)) return 0;
        DWORD flags=WINHTTP_ADDREQ_FLAG_ADD;
        if (!o->headers[i].mode) flags|=WINHTTP_ADDREQ_FLAG_REPLACE;
        if (!WinHttpAddRequestHeaders(request,wide,(DWORD)-1,flags)) return 0;
    }
    return 1;
}
/* Keep the guest API synchronous, but use cancellable WinHTTP operations internally.
 * Closing an in-progress synchronous WinHTTP handle from another thread is unsafe. */
typedef struct {
    HANDLE ready;
    volatile LONG references;
    DWORD notification,read;
    unsigned char *upload;
    unsigned char chunk[8192];
} PrivateTransfer;
static void private_transfer_release(PrivateTransfer *transfer) {
    if (transfer && InterlockedDecrement(&transfer->references)==0) {
        if (transfer->ready) CloseHandle(transfer->ready);
        free(transfer->upload); free(transfer);
    }
}
static void CALLBACK private_transfer_callback(HINTERNET handle,DWORD_PTR context,DWORD notification,void *data,DWORD length) {
    (void)handle; (void)data;
    PrivateTransfer *transfer=(PrivateTransfer *)context;
    if (!transfer) return;
    if (notification==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        private_transfer_release(transfer); return;
    }
    if (notification!=WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
        notification!=WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE &&
        notification!=WINHTTP_CALLBACK_STATUS_READ_COMPLETE &&
        notification!=WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return;
    transfer->read=notification==WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? length : 0;
    transfer->notification=notification;
    SetEvent(transfer->ready);
}
static int private_transfer_started(BOOL result) {
    return result || GetLastError()==ERROR_IO_PENDING;
}
static int private_transfer_wait(PrivateTransfer *transfer,ULONGLONG deadline,DWORD expected) {
    ULONGLONG now=GetTickCount64();
    return now<deadline && WaitForSingleObject(transfer->ready,(DWORD)(deadline-now))==WAIT_OBJECT_0 &&
        transfer->notification==expected && GetTickCount64()<deadline;
}
static ABI int private_send(int id,const void *data,uint64_t size) {
    if ((!data && size) || size>PRIVATE_BODY_LIMIT) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || o->complete || o->content_length!=size) return private_unlock(HTTP_INVALID);
    PrivateHttp *ctx=o;
    while (ctx->kind!=PRIVATE_CTX) ctx=private_find(ctx->parent,0);
    ULONGLONG deadline=GetTickCount64()+PRIVATE_REQUEST_MS;
    PrivateTransfer *transfer=NULL;
    HINTERNET connection=WinHttpConnect(ctx->session,L"127.0.0.1",17800,0),request=NULL;
    wchar_t path[2048]; int result=HTTP_NETWORK;
    if (!connection || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,o->path,-1,path,2048)) goto done;
    request=WinHttpOpenRequest(connection,o->method ? L"POST" : L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0);
    if (!request) goto done;
    DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof disable) || !private_add_headers(request,o)) goto done;
    transfer=calloc(1,sizeof *transfer);
    if (!transfer) goto done;
    transfer->references=1;
    transfer->ready=CreateEventW(NULL,FALSE,FALSE,NULL);
    if (!transfer->ready) goto done;
    if (size) {
        transfer->upload=malloc((size_t)size);
        if (!transfer->upload) goto done;
        memcpy(transfer->upload,data,(size_t)size);
    }
    DWORD_PTR context=(DWORD_PTR)transfer;
    if (!WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof context)) goto done;
    DWORD notifications=WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE|WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE|
        WINHTTP_CALLBACK_FLAG_READ_COMPLETE|WINHTTP_CALLBACK_FLAG_REQUEST_ERROR|WINHTTP_CALLBACK_FLAG_HANDLES;
    if (WinHttpSetStatusCallback(request,private_transfer_callback,notifications,0)==WINHTTP_INVALID_STATUS_CALLBACK) goto done;
    /* The handle owns a reference until its last HANDLE_CLOSING callback, including after timeout. */
    InterlockedIncrement(&transfer->references);
    if (GetTickCount64()>=deadline ||
        !private_transfer_started(WinHttpSendRequest(request,NULL,0,transfer->upload,(DWORD)size,(DWORD)size,context)) ||
        !private_transfer_wait(transfer,deadline,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) ||
        !private_transfer_started(WinHttpReceiveResponse(request,NULL)) ||
        !private_transfer_wait(transfer,deadline,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) goto done;
    DWORD status=0,bytes=sizeof status;
    if (!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&status,&bytes,NULL)) goto done;
    o->status=(int)status;
    bytes=0; WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,NULL,NULL,&bytes,NULL);
    if (!bytes || bytes>65536) goto done;
    wchar_t *wide=malloc(bytes);
    if (!wide) goto done;
    if (!WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,NULL,wide,&bytes,NULL)) { free(wide); goto done; }
    int length=WideCharToMultiByte(CP_UTF8,0,wide,-1,NULL,0,NULL,NULL);
    o->response_headers=length>0 ? malloc((size_t)length) : NULL;
    if (o->response_headers) WideCharToMultiByte(CP_UTF8,0,wide,-1,o->response_headers,length,NULL,NULL);
    free(wide);
    if (!o->response_headers) goto done;
    for (;;) {
        if (GetTickCount64()>=deadline ||
            !private_transfer_started(WinHttpReadData(request,transfer->chunk,sizeof transfer->chunk,NULL)) ||
            !private_transfer_wait(transfer,deadline,WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) goto done;
        DWORD read=transfer->read;
        if (!read) break;
        if (o->size+read>PRIVATE_BODY_LIMIT) { result=HTTP_LIMIT; goto done; }
        unsigned char *body=realloc(o->body,o->size+read);
        if (!body) goto done;
        o->body=body; memcpy(body+o->size,transfer->chunk,read); o->size+=read;
    }
    o->complete=1; o->event=1; result=0;
done:
    if (result) { free(o->body); o->body=NULL; o->size=0; free(o->response_headers); o->response_headers=NULL; }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    private_transfer_release(transfer);
    const char *trace=getenv("KZSF_NET_TRACE");
    if (trace && !strcmp(trace,"1")) fprintf(stderr,"PRIVATEHTTP method=%s path=%.*s result=%d status=%d bytes=%zu\n",o->method ? "POST" : "GET",(int)strcspn(o->path,"?"),o->path,result,o->status,o->size);
    return private_unlock(result);
}
static ABI int private_status(int id,int *status) {
    if (!status) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || !o->complete) return private_unlock(HTTP_INVALID);
    *status=o->status; return private_unlock(0);
}
static ABI int private_length(int id,int *type,uint64_t *size) {
    if (!type || !size) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || !o->complete) return private_unlock(HTTP_INVALID);
    *type=0; *size=o->size; return private_unlock(0);
}
static ABI int private_headers(int id,char **headers,uint64_t *size) {
    if (!headers || !size) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || !o->complete) return private_unlock(HTTP_INVALID);
    *headers=o->response_headers; *size=strlen(o->response_headers)+1; return private_unlock(0);
}
static ABI int private_read(int id,void *data,uint64_t size) {
    if (!data && size) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || !o->complete) return private_unlock(HTTP_INVALID);
    size_t count=o->size-o->offset; if (size<count) count=(size_t)size;
    if (count) memcpy(data,o->body+o->offset,count);
    o->offset+=count; return private_unlock((int)count);
}
static ABI int private_content_length(int id,uint64_t size) {
    if (size>PRIVATE_BODY_LIMIT) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || o->complete) return private_unlock(HTTP_INVALID);
    o->content_length=size; return private_unlock(0);
}
static int private_delete(int id,int kind) {
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,kind);
    if (!o) return private_unlock(HTTP_INVALID);
    for (unsigned i=0;i<64;i++) if (private_objects[i].id && private_objects[i].parent==id) return private_unlock(HTTP_INVALID);
    if (o->session) WinHttpCloseHandle(o->session);
    free(o->body); free(o->response_headers); memset(o,0,sizeof *o); return private_unlock(0);
}
static ABI int private_delete_request(int id) { return private_delete(id,PRIVATE_REQUEST); }
static ABI int private_delete_connection(int id) { return private_delete(id,PRIVATE_CONNECTION); }
static ABI int private_delete_template(int id) { return private_delete(id,PRIVATE_TEMPLATE); }
static ABI int private_term(int id) { return private_delete(id,PRIVATE_CTX); }
static ABI int private_create_epoll(int ctx,void **handle) {
    if (!handle) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    if (!private_find(ctx,PRIVATE_CTX)) return private_unlock(HTTP_INVALID);
    PrivateHttp *o=private_new(PRIVATE_EPOLL,ctx);
    if (!o) return private_unlock(HTTP_LIMIT);
    *handle=(void *)(uintptr_t)o->id; return private_unlock(0);
}
static ABI int private_set_epoll(int id,void *handle,void *user) {
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o || !private_find((int)(uintptr_t)handle,PRIVATE_EPOLL)) return private_unlock(HTTP_INVALID);
    o->epoll=(uintptr_t)handle; o->user=user; return private_unlock(0);
}
static ABI int private_unset_epoll(int id) {
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find(id,PRIVATE_REQUEST);
    if (!o) return private_unlock(HTTP_INVALID);
    o->epoll=0; o->user=NULL; return private_unlock(0);
}
static ABI int private_destroy_epoll(int ctx,void *handle) {
    pthread_mutex_lock(&private_lock); PrivateHttp *o=private_find((int)(uintptr_t)handle,PRIVATE_EPOLL);
    int valid=o && o->parent==ctx;
    pthread_mutex_unlock(&private_lock);
    return valid ? private_delete((int)(uintptr_t)handle,PRIVATE_EPOLL) : HTTP_INVALID;
}
static ABI int private_wait(void *handle,PrivateHttpEvent *events,int max,int timeout) {
    if (!events || max<1) return HTTP_INVALID;
    pthread_mutex_lock(&private_lock);
    if (!private_find((int)(uintptr_t)handle,PRIVATE_EPOLL)) return private_unlock(HTTP_INVALID);
    int count=0;
    for (unsigned i=0;i<64 && count<max;i++) {
        PrivateHttp *o=&private_objects[i];
        if (o->kind==PRIVATE_REQUEST && o->epoll==(uintptr_t)handle && o->event) {
            events[count++]=(PrivateHttpEvent){.events=3,.id=o->id,.user=o->user}; o->event=0;
        }
    }
    pthread_mutex_unlock(&private_lock);
    if (!count && timeout) Sleep(1);
    return count;
}
uintptr_t runtime_http_resolve(const char *name) {
    if (!private_enabled()) return 0;
    static const RuntimeExport exports[]={
        {"sceHttpInit",private_init},{"sceHttpTerm",private_term},
        {"sceHttpCreateTemplate",private_template},{"sceHttpDeleteTemplate",private_delete_template},
        {"sceHttpCreateConnectionWithURL",private_connection},{"sceHttpDeleteConnection",private_delete_connection},
        {"sceHttpCreateRequestWithURL",private_request},{"sceHttpDeleteRequest",private_delete_request},
        {"sceHttpAddRequestHeader",private_header},{"sceHttpSetRequestContentLength",private_content_length},
        {"sceHttpSendRequest",private_send},{"sceHttpGetStatusCode",private_status},
        {"sceHttpGetResponseContentLength",private_length},{"sceHttpGetAllResponseHeaders",private_headers},
        {"sceHttpReadData",private_read},{"sceHttpCreateEpoll",private_create_epoll},
        {"sceHttpSetEpoll",private_set_epoll},{"sceHttpUnsetEpoll",private_unset_epoll},
        {"sceHttpDestroyEpoll",private_destroy_epoll},{"sceHttpWaitRequest",private_wait}
    };
    return RUNTIME_LOOKUP(exports,name);
}
#else
uintptr_t runtime_http_resolve(const char *name) { (void)name; return 0; }
#endif
