/* Reuse the service test's runtime/GPU fixtures and verify its offline baseline. */
#define main services_baseline_test
#include "test_services.c"
#undef main

static void test_env(const char *name,const char *value) {
#ifdef _WIN32
    assert(_putenv_s(name,value ? value : "")==0);
#else
    assert((value ? setenv(name,value,1) : unsetenv(name))==0);
#endif
}

typedef ABI int32_t (*NpStateFn)(int32_t,int32_t *);
typedef ABI int32_t (*NpOnlineIdFn)(int32_t,NpOnlineId *);
typedef ABI int32_t (*NpIdFn)(int32_t,NpId *);

static void check_identity(int enabled,const char *expected_name) {
    NpStateFn get_state=(NpStateFn)runtime_services_resolve("sceNpGetState");
    NpOnlineIdFn get_online=(NpOnlineIdFn)runtime_services_resolve("sceNpGetOnlineId");
    NpIdFn get_id=(NpIdFn)runtime_services_resolve("sceNpGetNpId");
    assert(get_state && get_online && get_id);
    struct { unsigned char before; NpOnlineId value; unsigned char after; } online;
    struct { unsigned char before; NpId value; unsigned char after; } id;
    memset(&online,0xa5,sizeof online);
    memset(&id,0xa5,sizeof id);
    int32_t state=-123;
    assert(get_state(USER_ID,&state)==0 && state==(enabled ? 2 : 1));
    assert(get_state(USER_ID,NULL)==NP_INVALID_ARGUMENT);
    state=-123;
    assert(get_state(USER_ID+1,&state)==NP_USER_NOT_FOUND && state==-123);
    assert(get_online(USER_ID,NULL)==NP_INVALID_ARGUMENT);
    assert(get_id(USER_ID,NULL)==NP_INVALID_ARGUMENT);
    assert(get_online(USER_ID+1,&online.value)==NP_USER_NOT_FOUND);
    assert(get_id(USER_ID+1,&id.value)==NP_USER_NOT_FOUND);
    unsigned char untouched_online[sizeof online],untouched_id[sizeof id];
    memset(untouched_online,0xa5,sizeof untouched_online);
    memset(untouched_id,0xa5,sizeof untouched_id);
    assert(!memcmp(&online,untouched_online,sizeof online));
    assert(!memcmp(&id,untouched_id,sizeof id));
    assert(get_online(USER_ID,&online.value)==(enabled ? 0 : NP_SIGNED_OUT));
    assert(get_id(USER_ID,&id.value)==(enabled ? 0 : NP_SIGNED_OUT));
    if (enabled) {
        NpOnlineId expected_online={0};
        NpId expected_id={0};
        memcpy(expected_online.data,expected_name,strlen(expected_name));
        expected_id.handle=expected_online;
        assert(!memcmp(&online.value,&expected_online,sizeof expected_online));
        assert(!memcmp(&id.value,&expected_id,sizeof expected_id));
        assert(online.before==0xa5 && online.after==0xa5);
        assert(id.before==0xa5 && id.after==0xa5);
    } else {
        assert(!memcmp(&online,untouched_online,sizeof online));
        assert(!memcmp(&id,untouched_id,sizeof id));
    }
}

static void check_profile(void) {
    typedef ABI int32_t (*CreateFn)(const NpAsyncParam *);
    typedef ABI int32_t (*PollFn)(int32_t,int32_t *);
    typedef ABI int32_t (*RequestFn)(int32_t);
    typedef ABI int32_t (*ParentalFn)(int32_t,const NpOnlineId *,int8_t *,NpParentalInfo *);
    typedef ABI int32_t (*CountryFn)(const NpOnlineId *,NpCountryCode *);
    typedef ABI int32_t (*LanguageFn)(int32_t,const NpOnlineId *,NpLanguageCode *);
    CreateFn create=(CreateFn)runtime_services_resolve("sceNpCreateAsyncRequest");
    PollFn poll=(PollFn)runtime_services_resolve("sceNpPollAsync");
    PollFn auth_poll=(PollFn)runtime_services_resolve("sceNpAuthPollAsync");
    PollFn lookup_poll=(PollFn)runtime_services_resolve("sceNpLookupPollAsync");
    RequestFn release=(RequestFn)runtime_services_resolve("sceNpDeleteRequest");
    RequestFn abort_request=(RequestFn)runtime_services_resolve("sceNpAbortRequest");
    ParentalFn parental=(ParentalFn)runtime_services_resolve("sceNpGetParentalControlInfo");
    CountryFn country=(CountryFn)runtime_services_resolve("sceNpGetAccountCountry");
    LanguageFn language=(LanguageFn)runtime_services_resolve("sceNpGetAccountLanguage");
    assert(create && poll && auth_poll && lookup_poll && release && abort_request && parental && country && language);
    assert(lookup_poll==np_poll);
    NpAsyncParam param={24,0x3f,700,{0}},bad_param=param;
    bad_param.size=23;
    assert(create(NULL)==NP_INVALID_ARGUMENT && create(&bad_param)==NP_INVALID_SIZE);
    test_env("KZSF_PROBE_NP","1");
    NpOnlineId online,unknown;
    assert(np_online_id(USER_ID,&online)==0); unknown=online; unknown.data[0]='X';
    int32_t first=create(&param),second=create(&param),result=-123;
    assert(first>0 && second>first);
    assert(poll(first,&result)==NP_INVALID_ID && result==-123);
    assert(poll(first,NULL)==NP_INVALID_ARGUMENT && poll(0,&result)==NP_REQUEST_NOT_FOUND);
    struct { unsigned char before; int8_t value; unsigned char after; } age;
    struct { unsigned char before; NpParentalInfo value; unsigned char after; } info;
    struct { unsigned char before; NpCountryCode value; unsigned char after; } region;
    struct { unsigned char before; NpLanguageCode value; unsigned char after; } lang;
    memset(&age,0xa5,sizeof age); memset(&info,0xa5,sizeof info);
    memset(&region,0xa5,sizeof region); memset(&lang,0xa5,sizeof lang);
    assert(parental(first,NULL,&age.value,&info.value)==NP_INVALID_ARGUMENT);
    assert(parental(first,&online,NULL,&info.value)==NP_INVALID_ARGUMENT);
    assert(parental(first,&online,&age.value,NULL)==NP_INVALID_ARGUMENT);
    assert(parental(first,&unknown,&age.value,&info.value)==NP_USER_NOT_FOUND);
    assert(parental(0,&online,&age.value,&info.value)==NP_INVALID_ARGUMENT);
    assert(parental(second+1,&online,&age.value,&info.value)==NP_REQUEST_NOT_FOUND);
    assert(country(NULL,&region.value)==NP_INVALID_ARGUMENT && country(&online,NULL)==NP_INVALID_ARGUMENT);
    assert(country(&unknown,&region.value)==NP_USER_NOT_FOUND);
    assert(language(second,NULL,&lang.value)==NP_INVALID_ARGUMENT && language(second,&online,NULL)==NP_INVALID_ARGUMENT);
    assert(language(second,&unknown,&lang.value)==NP_USER_NOT_FOUND);
    unknown=online; unknown.term=1;
    assert(language(second,&unknown,&lang.value)==NP_USER_NOT_FOUND);
    for (size_t i=0;i<sizeof age;++i) assert(((unsigned char *)&age)[i]==0xa5);
    for (size_t i=0;i<sizeof info;++i) assert(((unsigned char *)&info)[i]==0xa5);
    for (size_t i=0;i<sizeof region;++i) assert(((unsigned char *)&region)[i]==0xa5);
    for (size_t i=0;i<sizeof lang;++i) assert(((unsigned char *)&lang)[i]==0xa5);
    assert(parental(first,&online,&age.value,&info.value)==0);
    assert(age.value==18 && age.before==0xa5 && age.after==0xa5);
    assert(!info.value.content && !info.value.chat && !info.value.user_content);
    assert(info.before==0xa5 && info.after==0xa5);
    assert(poll(first,&result)==0 && result==0);
    result=-123; assert(auth_poll(first,&result)==NP_AUTH_REQUEST_NOT_FOUND && result==-123);
    assert(lookup_poll(first,&result)==0 && result==NP_SIGNED_OUT);
    result=-123; assert(poll(second,&result)==NP_INVALID_ID && result==-123);
    assert(parental(first,&online,&age.value,&info.value)==NP_INVALID_ARGUMENT);
    assert(country(&online,&region.value)==0 && !memcmp(&region.value,"us\0",4));
    assert(region.before==0xa5 && region.after==0xa5);
    assert(language(second,&online,&lang.value)==0);
    unsigned char expected_language[16]={'e','n'};
    assert(!memcmp(&lang.value,expected_language,16) && lang.before==0xa5 && lang.after==0xa5);
    assert(abort_request(second)==0 && poll(second,&result)==0 && result==0);
    assert(release(first)==0 && release(first)==NP_REQUEST_NOT_FOUND);
    assert(poll(first,&result)==NP_REQUEST_NOT_FOUND && abort_request(first)==NP_REQUEST_NOT_FOUND);
    int32_t third=create(&param); assert(third>second);
    assert(parental(first,&online,&age.value,&info.value)==NP_REQUEST_NOT_FOUND);
    assert(abort_request(third)==0 && abort_request(third)==0);
    assert(poll(third,&result)==0 && result==NP_ABORTED);
    assert(language(third,&online,&lang.value)==NP_ABORTED);
    assert(release(second)==0 && release(third)==0);
    int32_t requests[32];
    for (size_t i=0;i<32;++i) { requests[i]=create(&param); assert(requests[i]>third); }
    assert(create(&param)==NP_REQUEST_MAX);
    for (size_t i=0;i<32;++i) assert(release(requests[i])==0);

    test_env("KZSF_PROBE_NP","0");
    int32_t disabled=create(&param); assert(disabled>third);
    memset(&age,0xa5,sizeof age); memset(&info,0xa5,sizeof info);
    memset(&region,0xa5,sizeof region); memset(&lang,0xa5,sizeof lang);
    assert(parental(disabled,&online,&age.value,&info.value)==NP_SIGNED_OUT);
    assert(country(&online,&region.value)==NP_SIGNED_OUT);
    assert(language(disabled,&online,&lang.value)==NP_SIGNED_OUT);
    assert(poll(disabled,&result)==0 && result==NP_SIGNED_OUT);
    assert(release(disabled)==0);
    for (size_t i=0;i<sizeof age;++i) assert(((unsigned char *)&age)[i]==0xa5);
    for (size_t i=0;i<sizeof info;++i) assert(((unsigned char *)&info)[i]==0xa5);
    for (size_t i=0;i<sizeof region;++i) assert(((unsigned char *)&region)[i]==0xa5);
    for (size_t i=0;i<sizeof lang;++i) assert(((unsigned char *)&lang)[i]==0xa5);
}

static void check_availability(void) {
    typedef ABI int32_t (*AvailabilityFn)(int32_t,const NpOnlineId *);
    typedef ABI int32_t (*CreateFn)(const NpAsyncParam *);
    typedef ABI int32_t (*PollFn)(int32_t,int32_t *);
    typedef ABI int32_t (*RequestFn)(int32_t);
    AvailabilityFn available=(AvailabilityFn)runtime_services_resolve("sceNpCheckNpAvailability");
    CreateFn create=(CreateFn)runtime_services_resolve("sceNpCreateAsyncRequest");
    PollFn poll=(PollFn)runtime_services_resolve("sceNpPollAsync");
    PollFn auth_poll=(PollFn)runtime_services_resolve("sceNpAuthPollAsync");
    PollFn lookup_poll=(PollFn)runtime_services_resolve("sceNpLookupPollAsync");
    RequestFn release=(RequestFn)runtime_services_resolve("sceNpDeleteRequest");
    RequestFn abort_request=(RequestFn)runtime_services_resolve("sceNpAbortRequest");
    assert(available && create && poll && release && abort_request && auth_poll && lookup_poll);
    NpAsyncParam param={24,0x3f,700,{0}};
    struct { unsigned char before; NpOnlineId value; unsigned char after; } online;
    memset(&online,0xa5,sizeof online);
    test_env("KZSF_PROBE_NP","1");
    assert(np_online_id(USER_ID,&online.value)==0);
    unsigned char original[sizeof online]; memcpy(original,&online,sizeof online);
    NpOnlineId unknown=online.value; unknown.data[0]='X';
    int32_t first=create(&param),second=create(&param),result=-123;
    assert(first>0 && second>first);
    assert(available(first,&online.value)==0);
    assert(poll(first,&result)==0 && result==0);
    assert(available(first,&online.value)==NP_INVALID_ARGUMENT);
    result=-123; assert(poll(second,&result)==NP_INVALID_ID && result==-123);
    assert(available(second,NULL)==NP_INVALID_ARGUMENT);
    assert(available(0,&online.value)==NP_INVALID_ARGUMENT);
    assert(available(second+1,&online.value)==NP_REQUEST_NOT_FOUND);
    assert(available(-1,&online.value)==NP_REQUEST_NOT_FOUND);
    assert(available(second,&unknown)==NP_USER_NOT_FOUND);
    unknown=online.value; unknown.term=1;
    assert(available(second,&unknown)==NP_USER_NOT_FOUND);
    result=-123; assert(poll(second,&result)==NP_INVALID_ID && result==-123);
    result=-123; assert(auth_poll(first,&result)==NP_AUTH_REQUEST_NOT_FOUND && result==-123);
    assert(lookup_poll(first,&result)==0 && result==NP_SIGNED_OUT);
    assert(release(first)==0);
    assert(available(first,&online.value)==NP_REQUEST_NOT_FOUND);
    int32_t third=create(&param); assert(third>second);
    assert(available(first,&online.value)==NP_REQUEST_NOT_FOUND);
    assert(abort_request(third)==0);
    assert(available(third,&online.value)==NP_ABORTED);
    assert(poll(third,&result)==0 && result==NP_ABORTED);
    assert(release(third)==0);

    const char *disabled_modes[]={NULL,"0","true"};
    for (size_t i=0;i<sizeof disabled_modes/sizeof disabled_modes[0];++i) {
        test_env("KZSF_PROBE_NP",disabled_modes[i]);
        assert(available(second,&online.value)==NP_SIGNED_OUT);
        assert(poll(second,&result)==0 && result==NP_SIGNED_OUT);
        test_env("KZSF_PROBE_NP","1");
        result=-123; assert(poll(second,&result)==NP_INVALID_ID && result==-123);
    }
    assert(available(second,&online.value)==0);
    assert(poll(second,&result)==0 && result==0);
    assert(release(second)==0 && release(second)==NP_REQUEST_NOT_FOUND);
    assert(!memcmp(&online,original,sizeof online));
}

static void check_private_eligibility(void) {
    /* Guest 0x8e3476 passes a 56-byte parameter and reads one authorized byte. */
    typedef struct {
        uint64_t size; int32_t user; unsigned char padding[4];
        uint64_t features; unsigned char reserved[32];
    } TestPlusParam;
    typedef struct { unsigned char authorized,reserved[32]; } TestPlusResult;
    typedef ABI int32_t (*CheckFn)(int32_t,const TestPlusParam *,TestPlusResult *);
    typedef ABI int32_t (*CreateFn)(const NpAsyncParam *);
    typedef ABI int32_t (*PollFn)(int32_t,int32_t *);
    typedef ABI int32_t (*RequestFn)(int32_t);
    CheckFn check=(CheckFn)runtime_services_resolve("sceNpCheckPlus");
    CreateFn create=(CreateFn)runtime_services_resolve("sceNpCreateAsyncRequest");
    PollFn poll=(PollFn)runtime_services_resolve("sceNpPollAsync");
    PollFn auth_poll=(PollFn)runtime_services_resolve("sceNpAuthPollAsync");
    RequestFn release=(RequestFn)runtime_services_resolve("sceNpDeleteRequest");
    RequestFn abort_request=(RequestFn)runtime_services_resolve("sceNpAbortRequest");
    assert(check && create && poll && release && abort_request && auth_poll);
    assert(sizeof(TestPlusParam)==56 && sizeof(TestPlusResult)==33);
    NpAsyncParam create_param={24,0x3f,700,{0}};
    TestPlusParam param={.size=56,.user=USER_ID,.features=1},bad;
    const TestPlusParam original=param;
    struct { unsigned char before; TestPlusResult value; unsigned char after; } output;
    memset(&output,0xa5,sizeof output);
    test_env("KZSF_PROBE_NP","1");
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    int32_t first=create(&create_param),second=create(&create_param),result=-123;
    assert(first>0 && second>first);
    assert(check(first,&param,&output.value)==0);
    assert(output.value.authorized==1 && output.before==0xa5 && output.after==0xa5);
    for (size_t i=0;i<sizeof output.value.reserved;++i) assert(output.value.reserved[i]==0xa5);
    assert(poll(first,&result)==0 && result==0);
    assert(poll(second,&result)==NP_INVALID_ID && result==0);
    assert(auth_poll(first,&result)==NP_AUTH_REQUEST_NOT_FOUND && result==0);
    memset(&output,0xa5,sizeof output);
    assert(check(first,&param,&output.value)==NP_INVALID_ARGUMENT);
    assert(check(second,NULL,&output.value)==NP_INVALID_ARGUMENT);
    assert(check(second,&param,NULL)==NP_INVALID_ARGUMENT);
    assert(check(0,&param,&output.value)==NP_INVALID_ARGUMENT);
    assert(check(-1,&param,&output.value)==NP_REQUEST_NOT_FOUND);
    bad=param; bad.size=55;
    assert(check(second,&bad,&output.value)==NP_INVALID_SIZE);
    bad=param; bad.user=-1;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    bad=param; bad.user=USER_ID+1;
    assert(check(second,&bad,&output.value)==NP_USER_NOT_FOUND);
    bad=param; bad.features=0;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    bad.features=2;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    bad.features=3;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    bad.features=4;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    bad=param; bad.reserved[31]=1;
    assert(check(second,&bad,&output.value)==NP_INVALID_ARGUMENT);
    test_env("KZSF_PRIVATE_PLAYER","invalid");
    assert(check(second,&param,&output.value)==NP_INVALID_ARGUMENT);
    test_env("KZSF_PRIVATE_PLAYER",NULL);
    const char *disabled_endpoints[]={NULL,"","http://localhost:17800","http://127.0.0.1:17801",
        "http://127.0.0.1:17800/","http://127.0.0.1:17800.evil.test"};
    for (size_t i=0;i<sizeof disabled_endpoints/sizeof disabled_endpoints[0];++i) {
        test_env("KZSF_PRIVATE_HTTP",disabled_endpoints[i]);
        assert(check(second,&param,&output.value)==NP_SIGNED_OUT);
    }
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    const char *disabled_np[]={NULL,"0","true"};
    for (size_t i=0;i<sizeof disabled_np/sizeof disabled_np[0];++i) {
        test_env("KZSF_PROBE_NP",disabled_np[i]);
        assert(check(second,&param,&output.value)==NP_SIGNED_OUT);
    }
    for (size_t i=0;i<sizeof output;++i) assert(((unsigned char *)&output)[i]==0xa5);
    test_env("KZSF_PROBE_NP","1");
    result=-123; assert(poll(second,&result)==NP_INVALID_ID && result==-123);
    assert(abort_request(second)==0);
    assert(check(second,&param,&output.value)==NP_ABORTED);
    assert(poll(second,&result)==0 && result==NP_ABORTED);
    assert(release(second)==0 && release(first)==0);
    int32_t next=create(&create_param); assert(next>second);
    assert(check(first,&param,&output.value)==NP_REQUEST_NOT_FOUND);
    assert(check(second,&param,&output.value)==NP_REQUEST_NOT_FOUND);
    assert(release(next)==0);
    const char *players[]={"Player1","Player2"};
    for (size_t i=0;i<2;++i) {
        test_env("KZSF_PRIVATE_PLAYER",players[i]);
        int32_t request=create(&create_param);
        assert(check(request,&param,&output.value)==0 && output.value.authorized==1);
        assert(poll(request,&result)==0 && result==0 && release(request)==0);
    }
    assert(!memcmp(&param,&original,sizeof param));
    test_env("KZSF_PRIVATE_PLAYER",NULL);
    test_env("KZSF_PRIVATE_HTTP",NULL);
}

static void check_private_auth(void) {
    /* Independent ABI fixtures allow this test to catch output-size/layout mistakes. */
    typedef struct { char id[129]; unsigned char padding[7]; } TestClientId;
    typedef struct { char code[129]; unsigned char padding[7]; } TestAuthCode;
    typedef struct {
        uint64_t size; const NpOnlineId *online; const TestClientId *client; const char *scope;
    } TestAuthParam;
    typedef ABI int32_t (*CreateFn)(const NpAsyncParam *);
    typedef ABI int32_t (*PollFn)(int32_t,int32_t *);
    typedef ABI int32_t (*RequestFn)(int32_t);
    typedef ABI int32_t (*CodeFn)(int32_t,const TestAuthParam *,TestAuthCode *,int32_t *);
    const int32_t invalid_argument=(int32_t)0x80550301,invalid_size=(int32_t)0x80550302;
    const int32_t aborted=(int32_t)0x80550304,request_max=(int32_t)0x80550305;
    const int32_t not_found=(int32_t)0x80550306,invalid_id=(int32_t)0x80550307;
    CreateFn create=(CreateFn)runtime_services_resolve("sceNpAuthCreateAsyncRequest");
    PollFn poll=(PollFn)runtime_services_resolve("sceNpAuthPollAsync");
    PollFn lookup_poll=(PollFn)runtime_services_resolve("sceNpLookupPollAsync");
    RequestFn release=(RequestFn)runtime_services_resolve("sceNpAuthDeleteRequest");
    RequestFn abort_request=(RequestFn)runtime_services_resolve("sceNpAuthAbortRequest");
    CodeFn get_code=(CodeFn)runtime_services_resolve("sceNpAuthGetAuthorizationCode");
    assert(create && poll && lookup_poll && release && abort_request && get_code);
    assert(sizeof(TestAuthParam)==32 && sizeof(TestClientId)==136 && sizeof(TestAuthCode)==136);
    NpAsyncParam create_param={24,0x3f,700,{0}},bad_create=create_param;
    TestClientId client={.id="local-test-client"};
    NpOnlineId online={.data=USER_NAME},unknown=online;
    TestAuthParam param={32,&online,&client,"local-test"},bad_param=param;
    struct { unsigned char before; TestAuthCode value; unsigned char after; } code;
    struct { int32_t before,value,after; } issuer;
    memset(&code,0xa5,sizeof code); memset(&issuer,0xa5,sizeof issuer);
    test_env("KZSF_PROBE_NP","1");
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    int32_t first=create(&create_param),second=create(&create_param),result=-123;
    assert(first>0 && second>first);
    assert(get_code(first,&param,&code.value,&issuer.value)==0);
    TestAuthCode expected={.code="KZSF_LOCAL_TEST"};
    assert(!memcmp(&code.value,&expected,sizeof expected));
    assert(code.before==0xa5 && code.after==0xa5);
    assert(issuer.value==0x100 && issuer.before==(int32_t)0xa5a5a5a5 && issuer.after==(int32_t)0xa5a5a5a5);
    assert(first>=0x10000001 && second<0x20000000);
    assert(poll(first,&result)==0 && result==0);
    assert(abort_request(first)==0 && poll(first,&result)==0 && result==0);
    assert(poll(second,&result)==invalid_id && result==0);
    assert(lookup_poll(first,&result)==0 && result==NP_SIGNED_OUT);
    memset(&code,0xa5,sizeof code); memset(&issuer,0xa5,sizeof issuer);
    assert(get_code(first,&param,&code.value,&issuer.value)==invalid_argument);
    bad_create.size=23;
    assert(create(NULL)==invalid_argument && create(&bad_create)==invalid_size);
    assert(get_code(second,NULL,&code.value,&issuer.value)==invalid_argument);
    assert(get_code(second,&param,NULL,&issuer.value)==invalid_argument);
    bad_param.size=31;
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==invalid_size);
    bad_param=param; bad_param.online=NULL;
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==invalid_argument);
    bad_param=param; bad_param.client=NULL;
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==invalid_argument);
    bad_param=param; bad_param.scope=NULL;
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==invalid_argument);
    bad_param=param; bad_param.online=&unknown; unknown.data[0]='X';
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==NP_USER_NOT_FOUND);
    unknown=online; unknown.term=1;
    assert(get_code(second,&bad_param,&code.value,&issuer.value)==NP_USER_NOT_FOUND);
    assert(get_code(0,&param,&code.value,&issuer.value)==not_found);
    assert(get_code(-1,&param,&code.value,&issuer.value)==not_found);
    assert(poll(second,NULL)==invalid_argument && poll(0,&result)==not_found);
    int32_t manager=np_manager_create(&create_param);
    assert(manager>=0x20000001 && manager!=second);
    assert(get_code(manager,&param,&code.value,&issuer.value)==not_found);
    assert(poll(manager,&result)==not_found && release(manager)==not_found && abort_request(manager)==not_found);
    assert(np_manager_poll(second,&result)==NP_REQUEST_NOT_FOUND);
    assert(np_manager_delete(second)==NP_REQUEST_NOT_FOUND && np_manager_delete(manager)==0);

    const char *disabled_endpoints[]={NULL,"","http://localhost:17800","http://127.0.0.1:17801",
        "http://127.0.0.1:17800/","http://127.0.0.1:17800.evil.test","https://127.0.0.1:17800"};
    for (size_t i=0;i<sizeof disabled_endpoints/sizeof disabled_endpoints[0];++i) {
        test_env("KZSF_PRIVATE_HTTP",disabled_endpoints[i]);
        assert(get_code(second,&param,&code.value,&issuer.value)==NP_SIGNED_OUT);
        assert(poll(second,&result)==0 && result==NP_SIGNED_OUT);
    }
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    const char *disabled_np[]={NULL,"0","true"};
    for (size_t i=0;i<sizeof disabled_np/sizeof disabled_np[0];++i) {
        test_env("KZSF_PROBE_NP",disabled_np[i]);
        assert(get_code(second,&param,&code.value,&issuer.value)==NP_SIGNED_OUT);
        assert(poll(second,&result)==0 && result==NP_SIGNED_OUT);
    }
    for (size_t i=0;i<sizeof code;++i) assert(((unsigned char *)&code)[i]==0xa5);
    for (size_t i=0;i<sizeof issuer;++i) assert(((unsigned char *)&issuer)[i]==0xa5);
    test_env("KZSF_PROBE_NP","1");
    result=-123; assert(poll(second,&result)==invalid_id && result==-123);
    assert(get_code(second,&param,&code.value,NULL)==0); /* Optional issuer pointer. */
    assert(!memcmp(&code.value,&expected,sizeof expected));
    assert(code.before==0xa5 && code.after==0xa5);
    assert(poll(second,&result)==0 && result==0);
    assert(release(first)==0 && release(first)==not_found);
    assert(get_code(first,&param,&code.value,&issuer.value)==not_found);
    int32_t third=create(&create_param); assert(third>second);
    assert(poll(first,&result)==not_found && abort_request(first)==not_found);
    assert(abort_request(third)==0 && abort_request(third)==0);
    memset(&code,0xa5,sizeof code);
    assert(get_code(third,&param,&code.value,&issuer.value)==aborted);
    assert(poll(third,&result)==0 && result==aborted);
    for (size_t i=0;i<sizeof code;++i) assert(((unsigned char *)&code)[i]==0xa5);
    assert(release(second)==0 && release(third)==0);
    int32_t requests[16];
    for (size_t i=0;i<16;++i) { requests[i]=create(&create_param); assert(requests[i]>third); }
    assert(create(&create_param)==request_max);
    for (size_t i=0;i<16;++i) assert(release(requests[i])==0);
    test_env("KZSF_PRIVATE_HTTP",NULL);
}

static void check_private_mtu(void) {
    struct { uint32_t before,mtu,after; } output={0xa5a5a5a5,0xa5a5a5a5,0xa5a5a5a5};
    test_env("KZSF_PRIVATE_HTTP",NULL);
    assert(netctl_info(3,&output.mtu)==NET_CTL_NOT_CONNECTED && output.mtu==0xa5a5a5a5);
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    assert(netctl_info(3,&output.mtu)==0 && output.mtu==1500);
    assert(output.before==0xa5a5a5a5 && output.after==0xa5a5a5a5);
    assert(netctl_info(3,NULL)==NET_CTL_INVALID_ADDR);
    assert(netctl_info(17,&output.mtu)==NET_CTL_NOT_CONNECTED);
    test_env("KZSF_PRIVATE_HTTP",NULL);
}

static void check_private_players(void) {
    typedef ABI int32_t (*UserNameFn)(int32_t,char *,uint64_t);
    UserNameFn get_name=(UserNameFn)runtime_services_resolve("sceUserServiceGetUserName");
    assert(get_name);
    NpAsyncParam create_param={24,0x3f,700,{0}};
    NpClientId auth_client={.id="local-test-client"};
    const char *players[]={NULL,"","Player1","Player2"};
    test_env("KZSF_PROBE_NP","1");
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    for (size_t i=0;i<sizeof players/sizeof players[0];++i) {
        test_env("KZSF_PRIVATE_PLAYER",players[i]);
        const char *name=players[i] && *players[i] ? players[i] : "Player";
        check_identity(1,name);
        struct { unsigned char before; char value[8]; unsigned char after; } user;
        memset(&user,0xa5,sizeof user);
        assert(get_name(USER_ID,user.value,strlen(name))==(int32_t)0x8096000a);
        for (size_t n=0;n<sizeof user;++n) assert(((unsigned char *)&user)[n]==0xa5);
        assert(get_name(USER_ID,user.value,strlen(name)+1)==0 && !strcmp(user.value,name));
        assert(user.before==0xa5 && user.after==0xa5);
        NpOnlineId online,other={.data="Player1"};
        assert(np_online_id(USER_ID,&online)==0);
        if (!strcmp(name,"Player1")) memcpy(other.data,"Player2",7);
        int32_t parental=np_manager_create(&create_param),language=np_manager_create(&create_param);
        int32_t availability=np_manager_create(&create_param),auth=np_auth_create(&create_param),result=-1;
        int8_t age=-1; NpParentalInfo restrictions={0xff,0xff,0xff};
        NpCountryCode country={0}; NpLanguageCode lang={0};
        assert(np_parental_info(parental,&other,&age,&restrictions)==NP_USER_NOT_FOUND && age==-1);
        assert(np_account_country(&other,&country)==NP_USER_NOT_FOUND);
        assert(np_account_language(language,&other,&lang)==NP_USER_NOT_FOUND);
        assert(np_check_availability(availability,&other)==NP_USER_NOT_FOUND);
        assert(np_parental_info(parental,&online,&age,&restrictions)==0 && age==18);
        assert(np_account_country(&online,&country)==0 && !memcmp(&country,"us\0",4));
        assert(np_account_language(language,&online,&lang)==0 && !strcmp(lang.code,"en"));
        assert(np_check_availability(availability,&online)==0);
        assert(np_manager_poll(availability,&result)==0 && result==0);
        NpAuthCodeParam auth_param={32,&other,&auth_client,"local-test"};
        struct { unsigned char before; NpAuthorizationCode value; unsigned char after; } code;
        struct { int32_t before,value,after; } issuer;
        memset(&code,0xa5,sizeof code); memset(&issuer,0xa5,sizeof issuer);
        assert(np_auth_get_code(auth,&auth_param,&code.value,&issuer.value)==NP_USER_NOT_FOUND);
        for (size_t n=0;n<sizeof code;++n) assert(((unsigned char *)&code)[n]==0xa5);
        for (size_t n=0;n<sizeof issuer;++n) assert(((unsigned char *)&issuer)[n]==0xa5);
        auth_param.online=&online;
        assert(np_auth_get_code(auth,&auth_param,&code.value,&issuer.value)==0);
        NpAuthorizationCode expected={.code="KZSF_LOCAL_TEST"};
        if (strcmp(name,"Player")) snprintf(expected.code,sizeof expected.code,"KZSF_LOCAL_TEST:%s",name);
        assert(!memcmp(&code.value,&expected,sizeof expected));
        assert(code.before==0xa5 && code.after==0xa5);
        assert(issuer.value==0x100 && issuer.before==(int32_t)0xa5a5a5a5 && issuer.after==(int32_t)0xa5a5a5a5);
        assert(np_auth_poll(auth,&result)==0 && result==0);
        assert(np_manager_delete(parental)==0 && np_manager_delete(language)==0);
        assert(np_manager_delete(availability)==0 && np_auth_delete(auth)==0);
    }
    const char *invalid[]={"Player","player1","Player3"," Player1","Player1 ","Player1:Player2","Player1\n","Player123456789012345678"};
    for (size_t i=0;i<sizeof invalid/sizeof invalid[0];++i) {
        test_env("KZSF_PRIVATE_PLAYER",invalid[i]);
        unsigned char output[160],original[160];
        memset(output,0xa5,sizeof output); memcpy(original,output,sizeof output);
        int32_t state=-123;
        assert(np_state(USER_ID,&state)==NP_INVALID_ARGUMENT && state==-123);
        assert(get_name(USER_ID,(char *)output,8)==USER_INVALID_ARGUMENT);
        assert(np_online_id(USER_ID,(NpOnlineId *)output)==NP_INVALID_ARGUMENT);
        assert(np_id(USER_ID,(NpId *)output)==NP_INVALID_ARGUMENT);
        NpOnlineId prior={.data="Player1"};
        int32_t request=np_manager_create(&create_param),auth=np_auth_create(&create_param),issuer=-123;
        int8_t age=-1;
        assert(np_parental_info(request,&prior,&age,(NpParentalInfo *)output)==NP_INVALID_ARGUMENT && age==-1);
        assert(np_account_country(&prior,(NpCountryCode *)output)==NP_INVALID_ARGUMENT);
        assert(np_account_language(request,&prior,(NpLanguageCode *)output)==NP_INVALID_ARGUMENT);
        assert(np_check_availability(request,&prior)==NP_INVALID_ARGUMENT);
        NpAuthCodeParam param={32,&prior,&auth_client,"local-test"};
        assert(np_auth_get_code(auth,&param,(NpAuthorizationCode *)output,&issuer)==NP_INVALID_ARGUMENT && issuer==-123);
        assert(!memcmp(output,original,sizeof output));
        assert(np_manager_delete(request)==0 && np_auth_delete(auth)==0);
    }
    /* An unselected private environment must retain the old local/offline identity. */
    const char *unselected_players[]={"Player1","Player2","invalid"};
    for (size_t i=0;i<sizeof unselected_players/sizeof unselected_players[0];++i) {
        test_env("KZSF_PRIVATE_PLAYER",unselected_players[i]);
        test_env("KZSF_PROBE_NP","0");
        test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
        check_identity(0,"Player");
        char user[8]; assert(get_name(USER_ID,user,sizeof user)==0 && !strcmp(user,"Player"));
        test_env("KZSF_PROBE_NP","1");
        test_env("KZSF_PRIVATE_HTTP","http://localhost:17800");
        check_identity(1,"Player");
        assert(get_name(USER_ID,user,sizeof user)==0 && !strcmp(user,"Player"));
    }
    test_env("KZSF_PRIVATE_PLAYER",NULL);
    test_env("KZSF_PRIVATE_HTTP",NULL);
}

static void check_private_nicknames(void) {
    const char *slots[]={"Player1","Player2"};
    const char *nicknames[]={NULL,"","Ace","Raven_7-Delta","A123456789012345"};
    NpAsyncParam create_param={24,0x3f,700,{0}};
    NpClientId auth_client={.id="local-test-client"};
    test_env("KZSF_PROBE_NP","1");
    test_env("KZSF_PRIVATE_HTTP","http://127.0.0.1:17800");
    for (size_t s=0;s<sizeof slots/sizeof slots[0];++s) {
        test_env("KZSF_PRIVATE_PLAYER",slots[s]);
        for (size_t n=0;n<sizeof nicknames/sizeof nicknames[0];++n) {
            test_env("KZSF_PRIVATE_NICKNAME",nicknames[n]);
            const char *name=nicknames[n] && *nicknames[n] ? nicknames[n] : slots[s];
            check_identity(1,name);
            struct { unsigned char before; char value[17]; unsigned char after; } user;
            memset(&user,0xa5,sizeof user);
            assert(user_name(USER_ID,user.value,strlen(name))==(int32_t)0x8096000a);
            for (size_t i=0;i<sizeof user;++i) assert(((unsigned char *)&user)[i]==0xa5);
            assert(user_name(USER_ID,user.value,sizeof user.value)==0 && !strcmp(user.value,name));
            assert(user.before==0xa5 && user.after==0xa5);
            NpOnlineId online={0},other={.data="SomeoneElse"};
            assert(np_online_id(USER_ID,&online)==0 && np_validate_online_id(&online)==0);
            assert(np_validate_online_id(&other)==NP_USER_NOT_FOUND);
            if (nicknames[n] && *nicknames[n]) {
                NpOnlineId old_slot={0}; memcpy(old_slot.data,slots[s],strlen(slots[s]));
                assert(np_validate_online_id(&old_slot)==NP_USER_NOT_FOUND);
            }
            int32_t request=np_manager_create(&create_param),auth=np_auth_create(&create_param),result=-1;
            assert(np_check_availability(request,&online)==0);
            assert(np_manager_poll(request,&result)==0 && result==0);
            NpAuthCodeParam param={32,&online,&auth_client,"local-test"};
            NpAuthorizationCode code={0};
            assert(np_auth_get_code(auth,&param,&code,NULL)==0);
            char expected[32]; snprintf(expected,sizeof expected,"KZSF_LOCAL_TEST:%s",slots[s]);
            assert(!strcmp(code.code,expected));
            assert(np_manager_delete(request)==0 && np_auth_delete(auth)==0);
        }
    }
    const char *invalid[]={"ab","A1234567890123456","Raven One","Raven:One","Raven\n","Raven/One","\xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba"};
    for (size_t n=0;n<sizeof invalid/sizeof invalid[0];++n) {
        test_env("KZSF_PRIVATE_NICKNAME",invalid[n]);
        unsigned char output[160],original[160];
        memset(output,0xa5,sizeof output); memcpy(original,output,sizeof output);
        int32_t state=-123,issuer=-123,auth=np_auth_create(&create_param);
        NpOnlineId slot={.data="Player2"};
        NpAuthCodeParam param={32,&slot,&auth_client,"local-test"};
        assert(np_state(USER_ID,&state)==NP_INVALID_ARGUMENT && state==-123);
        assert(user_name(USER_ID,(char *)output,17)==USER_INVALID_ARGUMENT);
        assert(np_online_id(USER_ID,(NpOnlineId *)output)==NP_INVALID_ARGUMENT);
        assert(np_id(USER_ID,(NpId *)output)==NP_INVALID_ARGUMENT);
        assert(np_auth_get_code(auth,&param,(NpAuthorizationCode *)output,&issuer)==NP_INVALID_ARGUMENT && issuer==-123);
        assert(!memcmp(output,original,sizeof output));
        assert(np_auth_delete(auth)==0);
    }
    test_env("KZSF_PRIVATE_NICKNAME","Raven");
    const char *missing_slots[]={NULL,"","Player3"};
    for (size_t s=0;s<sizeof missing_slots/sizeof missing_slots[0];++s) {
        test_env("KZSF_PRIVATE_PLAYER",missing_slots[s]);
        NpOnlineId online;
        assert(np_online_id(USER_ID,&online)==NP_INVALID_ARGUMENT);
    }
    test_env("KZSF_PRIVATE_PLAYER","Player1");
    test_env("KZSF_PRIVATE_NICKNAME","invalid name");
    test_env("KZSF_PROBE_NP","0");
    check_identity(0,"Player");
    char user[8]; assert(user_name(USER_ID,user,sizeof user)==0 && !strcmp(user,"Player"));
    test_env("KZSF_PROBE_NP","1");
    test_env("KZSF_PRIVATE_HTTP","http://localhost:17800");
    check_identity(1,"Player");
    assert(user_name(USER_ID,user,sizeof user)==0 && !strcmp(user,"Player"));
    test_env("KZSF_PRIVATE_NICKNAME",NULL);
    test_env("KZSF_PRIVATE_PLAYER",NULL);
    test_env("KZSF_PRIVATE_HTTP",NULL);
}

int main(void) {
    test_env("KZSF_PROBE_NP",NULL);
    test_env("KZSF_PRIVATE_HTTP",NULL);
    test_env("KZSF_PRIVATE_PLAYER",NULL);
    test_env("KZSF_PRIVATE_NICKNAME",NULL);
    assert(services_baseline_test()==0);
    check_private_mtu();
    for (int trace=0;trace<2;++trace) {
        test_env("KZSF_NET_TRACE",trace ? "1" : "0");
        test_env("KZSF_PROBE_NP",NULL); check_identity(0,"Player");
        test_env("KZSF_PROBE_NP","1"); check_identity(1,"Player");
        test_env("KZSF_PROBE_NP","0"); check_identity(0,"Player");
        test_env("KZSF_PROBE_NP","true"); check_identity(0,"Player");
        check_profile();
        check_availability();
        check_private_eligibility();
        check_private_auth();
        check_private_players();
        check_private_nicknames();
    }
    puts("NP probe test: ok");
    return 0;
}
