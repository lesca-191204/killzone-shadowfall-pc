#undef NDEBUG
/* Avoid MinGW GCC's cross-ABI inliner ICE; retain actual guest ABI calls in this test. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-inline")
#endif
#include "../src/runtime_http.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;i++) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    _putenv("KZSF_PRIVATE_HTTP=");
    assert(!runtime_http_resolve("sceHttpSendRequest"));
    _putenv("KZSF_PRIVATE_HTTP=http://127.0.0.1:17800");
    typedef ABI int (*SendFn)(int,const void *,uint64_t);
    SendFn send_request=(SendFn)runtime_http_resolve("sceHttpSendRequest");
    assert(send_request);
    assert(!private_path("https://killzone4.online.scee.com.evil/test"));
    assert(!private_path("https://killzone4.online.scee.com@evil/test"));
    assert(!private_path("http://127.0.0.1:17800/test"));
    assert(!private_path("https://killzone4.online.scee.com/test\r\nX: bad"));
    assert(!strcmp(private_path("https://killzone4.online.scee.com/test?a=b"),"/test?a=b"));
    int ctx=private_init(1,1,4096);
    int tmpl=private_template(ctx,"KZSF-test",2,0);
    assert(private_header(tmpl,"X-Inherited","template",1)==0);
    int conn=private_connection(tmpl,"https://killzone4.online.scee.com",1);
    assert(ctx>0 && tmpl>0 && conn>0);
    assert(private_request(conn,0,"https://example.com/test",0)<0);
    assert(private_request(conn,0,"gameservice:unresolved?query-secret#fragment-secret",0)<0);
    assert(private_connection(tmpl,"https://name:userinfo-secret@other.invalid/path",0)<0);
    int req=private_request(conn,1,"https://killzone4.online.scee.com/test?hidden=value",4);
    assert(req>0);
    assert(private_header(req,"X-Test","ok\r\nInjected: bad",0)<0);
    assert(private_header(req,"X-Test","first",0)==0);
    assert(private_header(req,"X-Test","ok",0)==0);
    void *epoll=NULL;
    assert(private_create_epoll(ctx,&epoll)==0 && epoll);
    assert(private_set_epoll(req,epoll,(void *)(uintptr_t)42)==0);
    assert(send_request(req,"ping",4)==0);
    PrivateHttpEvent event={0};
    assert(private_wait(epoll,&event,1,0)==1 && event.id==req && event.user==(void *)(uintptr_t)42);
    assert(event.events==3 && private_wait(epoll,&event,1,0)==0);
    int status=-1,type=-1; uint64_t size=0;
    assert(private_status(req,&status)==0 && status==201);
    assert(private_length(req,&type,&size)==0 && type==0 && size==4);
    char data[8]={0};
    assert(private_read(req,data,2)==2 && private_read(req,data+2,6)==2 && !strcmp(data,"pong"));
    assert(private_read(req,data,1)==0);
    char *headers=NULL; uint64_t header_size=0;
    assert(private_headers(req,&headers,&header_size)==0 && headers && header_size && strstr(headers,"X-Test-Reply"));
    assert(private_delete_request(req)==0 && private_read(req,data,1)<0 && private_delete_request(req)<0);

    /* Each chunk arrives within the receive timeout, but the whole transfer must expire. */
    int slow=private_request(conn,0,"https://killzone4.online.scee.com/slow-chunks",0);
    assert(slow>req && private_set_epoll(slow,epoll,NULL)==0);
    ULONGLONG start=GetTickCount64();
    int slow_result=send_request(slow,NULL,0);
    ULONGLONG elapsed=GetTickCount64()-start;
    assert(slow_result==HTTP_NETWORK && elapsed>=4000 && elapsed<8000);
    assert(private_status(slow,&status)==HTTP_INVALID && private_read(slow,data,1)==HTTP_INVALID);
    assert(private_wait(epoll,&event,1,0)==0 && private_delete_request(slow)==0);
    /* Cancellation must release the lock and preserve the context for the next request. */
    req=private_request(conn,1,"https://killzone4.online.scee.com/test?hidden=value",4);
    assert(req>slow && private_header(req,"X-Test","ok",0)==0);
    assert(send_request(req,"ping",4)==0 && private_status(req,&status)==0 && status==201);
    assert(private_delete_request(req)==0);
    assert(private_destroy_epoll(ctx,epoll)==0 && private_destroy_epoll(ctx,epoll)<0);
    assert(private_delete_connection(conn)==0 && private_delete_template(tmpl)==0 && private_term(ctx)==0);
    puts("Private HTTP loopback test: ok");
    return 0;
}
