/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "cpu_check.h"
#include "isa_rewrite.h"
#include "tsc_patch.h"
#include "profiler.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "actor_hook.h"
#include "mouse_hook.h"
#include "damage_hook.h"
#include "postfx_hook.h"
#include "scene_res_hook.h"
#include "ultrawide_hook.h"
#include "prompt_hook.h"
#include "free_camera_hook.h"
#include "gpu/gow3gpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static unsigned cpu_missing_sets; /* CPU_SET_* the host lacks; the guest code needs them rewritten */
static IsaTable *isa_table; /* rewritten sites (--isa-rewrite); the fault handlers and the report read it */
static void isa_report_hook(void) { isa_rewrite_report(isa_table, stdout); }
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
/* Pages for the hooks' stubs and live cells, reserved right after the image (its rel32 jumps and
 * operands must reach them). Separate low mappings could land over 2 GB away once threads had
 * taken their stacks from the low region, and every hook then stayed off. */
enum { HOOK_PAGES = 32 };
static unsigned char *hook_pages;
static unsigned hook_pages_used;
static void *hook_page(void) {
    if (hook_pages && hook_pages_used < HOOK_PAGES) return hook_pages + (size_t)hook_pages_used++ * page_size;
    return allocate(page_size);
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
/* Cheats (src/cheat_hook.h): the menu's switches (one byte each, gow3_cheat_flag) and guest
 * memory reads and writes that fail instead of faulting. */
static volatile uint8_t *cheat_flags[GOW3_CHEAT_COUNT];
static uint64_t echo_site=UINT64_MAX, damage_site=UINT64_MAX, effects_site=UINT64_MAX;
static int cheat_on(unsigned id) { return cheat_flags[id] && *cheat_flags[id]; }
static int cheat_read(uintptr_t address, void *out, size_t size) {
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)address,out,size,&got) && got==size;
}
static int cheat_write(uintptr_t address, const void *in, size_t size) {
    SIZE_T done=0;
    return WriteProcessMemory(GetCurrentProcess(),(void *)address,in,size,&done) && done==size;
}
/* The Tactical Echo int3: keeps an active scan going, then replays "mov r14,rdi". */
static int echo_trap(CONTEXT *c, const void *address) {
    if (echo_site==UINT64_MAX || address!=image+echo_site) return 0;
    if (cheat_on(GOW3_CHEAT_ECHO)) gow3_echo_continue(c->Rdi,(uintptr_t)image,cheat_read,cheat_write);
    c->R14=c->Rdi;
    c->Rip=(uintptr_t)image+echo_site+GOW3_ECHO_LENGTH;
    return 1;
}
/* The god mode int3s: return at once (damage: for the player only), else replay "push rbp". */
static int damage_trap(CONTEXT *c, const void *address) {
    const int damage=damage_site!=UINT64_MAX && address==image+damage_site;
    const uint64_t at=damage ? damage_site : effects_site;
    if (!damage && (effects_site==UINT64_MAX || address!=image+effects_site)) return 0;
    uintptr_t entity=0, resume=0;
    if (cheat_on(GOW3_CHEAT_GOD) && cheat_read(c->Rsp,&resume,8) &&
        (!damage || (cheat_read(c->Rdi+0x38,&entity,8) && gow3_player_entity(entity,(uintptr_t)image,cheat_read,0)))) {
        c->Rip=resume;
        c->Rsp+=8;
        return 1;
    }
    c->Rsp-=8;
    *(DWORD64 *)c->Rsp=c->Rbp;
    c->Rip=(uintptr_t)image+at+1;
    return 1;
}
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    int site=isa_table ? isa_rewrite_site_of_stub(isa_table,address) : -1;
    if (site>=0 || (image && address-(uintptr_t)image<0x20000000)) {
        int n=snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image);
        if (site>=0 && n>=0 && (size_t)n<size)
            snprintf(out+n,size-n," While emulating site 0x%" PRIx64 " (%s)",isa_table->sites[site].offset,isa_kind_name(isa_table->sites[site].kind));
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    /* Killzone's fatal error handler (image+0x3277d0, 82 callers) formats its message into
     * [rbp-0x1000] (4 KiB) and ends in ud2 at +0x3278a0: print the game's own reason. */
    if (record->ExceptionCode==EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        c->Rip==(uintptr_t)image+0x3278a0 && !memcmp(image+0x32789b,"\xe8\x10\xf9\xff\xff\x0f\x0b",7)) {
        char message[4097];
        SIZE_T got=0;
        if (ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rbp-0x1000),message,4096,&got) && got) {
            message[got<4096 ? got : 4096]=0;
            fprintf(stderr,"Game fatal error: %s\n",message);
        }
    }
    /* A data lump the game could not load (image+0x61b280: "cache:lumps/<name>.core", ud2 at
     * +0x61b534 when the reader refuses it; the EU disc CUSA00002, issues #47/#53): its path is
     * a Decima String (length at +8, characters at +0x10) in one of the frame's locals. */
    if (record->ExceptionCode==EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        c->Rip==(uintptr_t)image+0x61b534 && !memcmp(image+0x61b534,"\x0f\x0b",2)) {
        int named=0;
        for (int slot=0x100; slot>=0xd8 && !named; slot-=8) {
            uint64_t string=0;
            int32_t length=0;
            char path[260];
            SIZE_T got=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rbp-slot),&string,8,&got) || got!=8 ||
                string<0x10000 || !ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(string+8),&length,4,&got) ||
                length<=6 || length>=(int32_t)sizeof(path) ||
                !ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(string+0x10),path,(SIZE_T)length,&got) ||
                got!=(SIZE_T)length) continue;
            path[length]=0;
            if (!strncmp(path,"cache:",6)) {
                fprintf(stderr,"Game fatal error: could not load the data file %s\n",path);
                named=1;
            }
        }
        if (!named) fprintf(stderr,"Game fatal error: could not load a data file (cache:lumps/*.core)\n");
    }
    if (gpu_enabled) gow3gpu_dump_guest_writes(info);
    /* Unwound through .pdata (drivers and the port's C++ have no frame pointer chain); guest
     * frames continue on rbp. Same layout as GOW3_PROFILE stacks: symbolize_profile.py names it. */
    fprintf(stderr,"  unwind:\n");
    CONTEXT u=*c;
    for (int depth=0; depth<40 && u.Rip; ++depth) {
        describe_address(where,sizeof(where),(uintptr_t)u.Rip);
        fprintf(stderr,"        %s\n",where);
        DWORD64 base=0;
        HMODULE module=NULL;
        PRUNTIME_FUNCTION f=RtlLookupFunctionEntry(u.Rip,&base,NULL);
        if (f) {
            void *handler_data; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,u.Rip,f,&u,&handler_data,&establisher,NULL);
        } else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)u.Rip,&module) && module) {
            SIZE_T got=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(void *)u.Rsp,&u.Rip,8,&got) || got!=8) break;
            u.Rsp+=8;
        } else if (depth==0 && record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->ExceptionInformation[0]==8) {
            /* A call through a bad pointer: the caller's return address is at rsp. */
            SIZE_T got=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(void *)u.Rsp,&u.Rip,8,&got) || got!=8) break;
            u.Rsp+=8;
        } else {
            uintptr_t frame[2];
            SIZE_T got=0;
            if (u.Rbp<u.Rsp || !ReadProcessMemory(GetCurrentProcess(),(void *)u.Rbp,frame,sizeof(frame),&got) || got!=sizeof(frame)) break;
            u.Rsp=u.Rbp+16; u.Rbp=frame[0]; u.Rip=frame[1];
        }
    }
    /* Leaf functions (CRT memcpy/strlen) have no frame: their caller's return address is at rsp. */
    uintptr_t top[4];
    SIZE_T top_got=0;
    if (ReadProcessMemory(GetCurrentProcess(),(void *)c->Rsp,top,sizeof(top),&top_got) && top_got==sizeof(top))
        for (int i=0;i<4;++i) { describe_address(where,sizeof(where),top[i]); fprintf(stderr,"  [rsp+%d] %s\n",i*8,where); }
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    if (isa_table) isa_rewrite_report(isa_table,stderr);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* CONTEXT <-> IsaRegs (gpr in x86 numbering). */
static void isa_context_regs(CONTEXT *c, IsaRegs *regs, int to_context) {
    DWORD64 *g[16]={&c->Rax,&c->Rcx,&c->Rdx,&c->Rbx,&c->Rsp,&c->Rbp,&c->Rsi,&c->Rdi,
                    &c->R8,&c->R9,&c->R10,&c->R11,&c->R12,&c->R13,&c->R14,&c->R15};
    for (int i=0;i<16;++i) { if (to_context) *g[i]=regs->gpr[i]; else regs->gpr[i]=*g[i]; }
    if (to_context) c->EFlags=(DWORD)regs->rflags; else regs->rflags=c->EFlags;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_BREAKPOINT && (echo_trap(info->ContextRecord,record->ExceptionAddress) ||
                                       damage_trap(info->ContextRecord,record->ExceptionAddress)))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (code==EXCEPTION_BREAKPOINT && isa_table) {
        /* An int3 ISA site: emulated here; any other breakpoint goes on to the report below. */
        IsaRegs regs;
        uintptr_t rip;
        isa_context_regs(info->ContextRecord,&regs,0);
        if (isa_rewrite_trap(isa_table,(uintptr_t)record->ExceptionAddress,&regs,&rip)) {
            isa_context_regs(info->ContextRecord,&regs,1);
            info->ContextRecord->Rip=(DWORD64)rip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && gow3gpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in gow3_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)gow3_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        /* The C runtime counts as ours: the runtime calls it with guest pointers. */
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) &&
                  (module==self || module==GetModuleHandleA("ucrtbase.dll"))) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* GOW3_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
/* ucontext gregs of IsaRegs.gpr (x86 numbering). */
static const int isa_gregs[16]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
static void fault(int sig, siginfo_t *info, void *context) {
    if (sig == SIGTRAP && isa_table) {
        /* An int3 ISA site (RIP is past the CC byte); any other trap ends in the report below. */
        greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
        IsaRegs regs;
        uintptr_t rip;
        for (int i = 0; i < 16; ++i) regs.gpr[i] = (uint64_t)g[isa_gregs[i]];
        regs.rflags = (uint64_t)g[REG_EFL];
        if (isa_rewrite_trap(isa_table, (uintptr_t)g[REG_RIP] - 1, &regs, &rip)) {
            for (int i = 0; i < 16; ++i) g[isa_gregs[i]] = (greg_t)regs.gpr[i];
            g[REG_EFL] = (greg_t)regs.rflags;
            g[REG_RIP] = (greg_t)rip;
            return;
        }
    }
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && gow3gpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    int site = isa_table ? isa_rewrite_site_of_stub(isa_table, rip) : -1;
    char emulating[64] = "";
    if (site >= 0)
        snprintf(emulating, sizeof(emulating), " While emulating site 0x%" PRIx64 " (%s)",
                 isa_table->sites[site].offset, isa_kind_name(isa_table->sites[site].kind));
    if (site >= 0 || rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p%s\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr, emulating);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) gow3gpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    if (isa_table) { isa_rewrite_report(isa_table, stderr); fflush(stderr); }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
extern ABI float gow3_orbs_gain(void *player, float requested);
extern void gow3_orbs_status(int supported);
extern uintptr_t gow3_cheat_flag(unsigned id);
extern uintptr_t gow3_actor_slots_address(void);
extern void gow3_actor_status(int supported);
extern void gow3_cheat_status(unsigned id);
extern uintptr_t gow3_damage_multiplier(int taken);
extern uintptr_t gow3_orb_pickup_multiplier(unsigned kind);
extern void gow3_stat_multipliers_status(int damage, int orb_pickup);
extern ABI void gow3_mouse_look_adjust(float *x, float *y);
extern ABI void gow3_mouse_look_limits(float *limits);
extern int gow3_mouse_direct_wanted(void);
extern void gow3_mouse_direct_status(int ready);

#ifdef _WIN32
/* The DebugSettings switches (src/cheat_hook.h): every 100 ms the menu's choices are written into
 * the game's object (it exists once the game has started); a switch turned off is cleared once,
 * so nothing else the game keeps there is touched. */
static uint64_t debug_settings_slot;
static unsigned debug_cheats_ready;
static DWORD WINAPI debug_cheats_thread(void *unused) {
    (void)unused;
    unsigned written=0;
    for (;;) {
        Sleep(100);
        uintptr_t object=0, vtable=0;
        if (!cheat_read((uintptr_t)image+debug_settings_slot,&object,8) || !object ||
            !cheat_read(object,&vtable,8) || vtable-(uintptr_t)image>0x20000000) continue;
        for (unsigned i=0;i<GOW3_DEBUG_CHEAT_COUNT;++i) {
            if (!(debug_cheats_ready & (1u<<i))) continue;
            const Gow3DebugCheat *cheat=&gow3_debug_cheats[i];
            const int on=cheat_on(gow3_debug_cheat_ids[i]);
            if (!on && !(written & (1u<<i))) continue;
            const uint32_t value=on ? 1 : 0;
            uint32_t now=0;
            if (cheat_read(object+cheat->field,&now,cheat->size) && now!=value)
                cheat_write(object+cheat->field,&value,cheat->size);
            written=on ? written|(1u<<i) : written&~(1u<<i);
        }
    }
    return 0;
}
#endif

/* The hooks below find their sites by signature (src/signature.h), so they work in any region
 * and version of the game whose code matches; a site that is not found leaves its feature off. */
static void install_cheats(Segment *segments, uint64_t count, uint64_t size) {
#ifdef _WIN32
    for (unsigned id = 0; id < GOW3_CHEAT_COUNT; ++id) cheat_flags[id] = (volatile uint8_t *)gow3_cheat_flag(id);
    uint64_t slot = UINT64_MAX;
    for (unsigned i = 0; i < GOW3_DEBUG_CHEAT_COUNT; ++i) {
        const Gow3DebugCheat *cheat = &gow3_debug_cheats[i];
        const uint64_t at = gow3_signature_find(image, size, &cheat->site);
        const uint64_t found = at == UINT64_MAX || !mapped(segments, count, at, 14) ? UINT64_MAX
                               : gow3_debug_settings_slot(image, at);
        if (found == UINT64_MAX || (slot != UINT64_MAX && found != slot) || found + 8 > size ||
            !mapped(segments, count, found, 8)) {
            printf("Cheats: %s unavailable (code not found in this eboot.bin)\n", cheat->name);
            continue;
        }
        slot = found;
        debug_cheats_ready |= 1u << i;
        gow3_cheat_status(gow3_debug_cheat_ids[i]);
    }
    if (debug_cheats_ready) {
        debug_settings_slot = slot;
        CloseHandle(CreateThread(NULL, 0, debug_cheats_thread, NULL, 0, NULL));
        printf("Cheats: game debug settings at image+0x%" PRIx64 " (god mode, ammo, reload, sprint, chapters; live)\n",
               slot);
    }
    const uint64_t hit = gow3_signature_find(image, size, &gow3_god_hit_site);
    if (hit == UINT64_MAX || !mapped(segments, count, hit, 1)) {
        printf("Cheats: god mode still shows hits (damage code not found in this eboot.bin)\n");
    } else {
        damage_site = hit;
        image[hit] = 0xcc;
        printf("Cheats: god mode skips the player's hits at image+0x%" PRIx64 "\n", hit);
    }
    const uint64_t effects = gow3_signature_find(image, size, &gow3_god_effects_site);
    if (effects == UINT64_MAX || !mapped(segments, count, effects, 1)) {
        printf("Cheats: god mode still shows hit effects (code not found in this eboot.bin)\n");
    } else {
        effects_site = effects;
        image[effects] = 0xcc;
        printf("Cheats: god mode skips the hit effects at image+0x%" PRIx64 "\n", effects);
    }
    const uint64_t at = gow3_signature_find(image, size, &gow3_echo_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_ECHO_LENGTH)) {
        printf("Cheats: continuous Tactical Echo unavailable (code not found in this eboot.bin)\n");
        return;
    }
    echo_site = at;
    image[at] = 0xcc;
    gow3_cheat_status(GOW3_CHEAT_ECHO);
    printf("Cheats: continuous Tactical Echo ready (live; use the Echo once after loading)\n");
#else
    (void)segments; (void)count; (void)size;
    printf("Cheats: unavailable on this platform\n");
#endif
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = gow3_signature_find(image, size, &gow3_actor_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_ACTOR_HOOK_LENGTH) ||
        !gow3_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = hook_page();
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3_actor_build_stub(stub, gow3_actor_slots_address(), (uintptr_t)(image + at + GOW3_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, GOW3_ACTOR_HOOK_LENGTH - 5);
    gow3_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

static void install_orb_multiplier(Segment *segments, uint64_t count, uint64_t size) {
    uint64_t calls[3];
    const uint64_t setter_at = gow3_orb_find(image, size, calls);
    if (setter_at == UINT64_MAX) {
        printf("Red orbs: multiplier unavailable (code not found in this eboot.bin)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = hook_page();
    uintptr_t helper = (uintptr_t)gow3_orbs_gain, setter = (uintptr_t)(image + setter_at);
    unsigned char code[26];
    gow3_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + calls[i] + 1, &displacements[i], 4);
    gow3_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Direct mouse look (src/mouse_hook.h): wraps the game's look input routine so the mouse turns
 * the camera linearly instead of through an emulated, saturating right stick. */
static void install_mouse_look(Segment *segments, uint64_t count, uint64_t size) {
    if (!gow3_mouse_direct_wanted()) return;
    const uint64_t at = gow3_signature_find(image, size, &gow3_mouse_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_MOUSE_HOOK_LENGTH) ||
        !gow3_mouse_signature_match(image + at, size - at)) {
        printf("Mouse look: direct mode unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = hook_page();
    gow3_mouse_build_stub(stub, (uintptr_t)gow3_mouse_look_adjust, (uintptr_t)(image + at + GOW3_MOUSE_HOOK_LENGTH));
    /* Optional: without it the mouse still turns, through the game's turn acceleration. */
    const uint64_t limits = gow3_signature_find(image, size, &gow3_mouse_limits_site);
    const int limits_found = limits != UINT64_MAX && mapped(segments, count, limits, GOW3_MOUSE_LIMITS_LENGTH) &&
                             gow3_mouse_limits_signature_match(image + limits, size - limits);
    if (limits_found)
        gow3_mouse_limits_build_stub(stub + 128, (uintptr_t)gow3_mouse_look_limits,
                                     (uintptr_t)(image + limits + GOW3_MOUSE_LIMITS_LENGTH));
    protect(stub, page_size, 5);
    if (!install_jump(at, GOW3_MOUSE_HOOK_LENGTH, stub)) {
        printf("Mouse look: direct mode unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3_mouse_direct_status(1);
    printf("Mouse look: direct mode ready (look input routine at image+0x%" PRIx64 ")\n", at);
    if (limits_found && install_jump(limits, GOW3_MOUSE_LIMITS_LENGTH, stub + 128))
        printf("Mouse look: no turn acceleration while the mouse moves (limits at image+0x%" PRIx64 ")\n", limits);
    else
        printf("Mouse look: turn acceleration kept (limits code not found)\n");
}

/* Live FOV (src/mouse_hook.h gow3_fov_site): the FOV update's pi/180 operand reads a writable
 * float near the image; the in-game menu and the launcher's FOV setting rewrite it. */
static void install_fov(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = gow3_signature_find(image, size, &gow3_fov_site);
    if (at == UINT64_MAX || !mapped(segments, count, at + GOW3_FOV_DISP_OFFSET, 4)) {
        printf("FOV: unavailable (camera code not found in this eboot.bin)\n");
        return;
    }
    float *cell = hook_page();
    *cell = 0.017453292f;
    const int64_t delta = (int64_t)(uintptr_t)cell - (int64_t)(uintptr_t)(image + at + GOW3_FOV_DISP_OFFSET + 4);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("FOV: unavailable (setting outside relative range)\n");
        return;
    }
    const int32_t relative = (int32_t)delta;
    memcpy(image + at + GOW3_FOV_DISP_OFFSET, &relative, 4);
    gow3gpu_fov_register(cell);
    printf("FOV: live setting ready (camera at image+0x%" PRIx64 ")\n", at);
}

/* Level of detail distance (issue #61). Each frame 0xdca7f0 copies the ViewportSetup resource
 * into the renderer ([0x3c3ffe0]): FOVMultiplier to +0x1f7c (the FOV hook above), LODFadingEnabled
 * to +0x1f84, LODDistanceMultiplier to +0x1f88 and LowLODDistanceMultiplier to +0x1f8c. 0xdc67e0
 * loads the two multipliers into its frame (0xdc697d..0xdc6999) and scales the LOD distances
 * with them. A stub replays those four instructions with both values times a float the menu
 * rewrites (live): above 1 meshes keep their detailed LODs further away (less pop-in), below 1
 * they switch earlier (faster). */
enum { GOW3_LOD_SITE = 0xdc697d, GOW3_LOD_LENGTH = 28 };
static const unsigned char gow3_lod_code[GOW3_LOD_LENGTH] = {
    0xc4, 0xc1, 0x7a, 0x10, 0x87, 0x88, 0x1f, 0x00, 0x00, /* vmovss xmm0,[r15+0x1f88] */
    0xc5, 0xfa, 0x11, 0x45, 0xd0,                         /* vmovss [rbp-0x30],xmm0 */
    0xc4, 0xc1, 0x7a, 0x10, 0x87, 0x8c, 0x1f, 0x00, 0x00, /* vmovss xmm0,[r15+0x1f8c] */
    0xc5, 0xfa, 0x11, 0x45, 0xd4};                        /* vmovss [rbp-0x2c],xmm0 */
static void install_lod_distance(Segment *segments, uint64_t count, uint64_t size) {
    if (GOW3_LOD_SITE + GOW3_LOD_LENGTH > size || !mapped(segments, count, GOW3_LOD_SITE, GOW3_LOD_LENGTH) ||
        memcmp(image + GOW3_LOD_SITE, gow3_lod_code, GOW3_LOD_LENGTH)) {
        printf("LOD distance: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    float *cell = hook_page();
    *cell = 1.0f;
    unsigned char *stub = hook_page();
    unsigned n = 0;
    const struct { unsigned from, length; } parts[2] = {{0, 9}, {14, 9}};
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(stub + n, gow3_lod_code + parts[i].from, parts[i].length); /* load the multiplier */
        n += parts[i].length;
        static const unsigned char mul[] = {0xc5, 0xfa, 0x59, 0x05};     /* vmulss xmm0,xmm0,[rip+cell] */
        memcpy(stub + n, mul, sizeof(mul));
        const int64_t delta = (int64_t)(uintptr_t)cell - (int64_t)(uintptr_t)(stub + n + 8);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("LOD distance: unavailable (setting outside relative range)\n");
            return;
        }
        const int32_t relative = (int32_t)delta;
        memcpy(stub + n + 4, &relative, 4);
        n += 8;
        memcpy(stub + n, gow3_lod_code + parts[i].from + 9, 5);           /* store it in the frame */
        n += 5;
    }
    static const unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0};         /* jmp [rip+0] */
    memcpy(stub + n, jump, sizeof(jump));
    const uintptr_t back = (uintptr_t)(image + GOW3_LOD_SITE + GOW3_LOD_LENGTH);
    memcpy(stub + n + sizeof(jump), &back, 8);
    protect(stub, page_size, 5);
    if (!install_jump(GOW3_LOD_SITE, GOW3_LOD_LENGTH, stub)) {
        printf("LOD distance: unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3gpu_lod_register(cell);
    printf("LOD distance: live setting ready (image+0x%x)\n", GOW3_LOD_SITE);
}

/* Post-processing switches (src/postfx_hook.h): motion blur, depth of field, bloom and film
 * grain turned off in the settings clear their bit of the game's feature mask (restart); camera
 * shake is a stub reading an int the menu rewrites (live). */
static void install_post_effects(Segment *segments, uint64_t count, uint64_t size) {
    const unsigned off = gow3gpu_post_effects_off();
    for (unsigned i = 0; i < GOW3_POSTFX_COUNT; ++i) {
        const Gow3PostFx *fx = &gow3_postfx[i];
        if (!(off & fx->bit)) continue;
        const uint64_t at = gow3_signature_find(image, size, &fx->site);
        const uint64_t mask = at == UINT64_MAX ? UINT64_MAX : gow3_postfx_mask(image, at);
        uint32_t flags = 0;
        if (mask == UINT64_MAX || mask + 4 > size || !mapped(segments, count, mask, 4)) {
            printf("Effects: %s switch unavailable (code not found in this eboot.bin)\n", fx->name);
            continue;
        }
        memcpy(&flags, image + mask, 4);
        if (!gow3_postfx_mask_known(flags)) {
            printf("Effects: %s switch unavailable (unknown feature mask %08x)\n", fx->name, flags);
            continue;
        }
        const uint32_t after = flags & ~fx->bit;
        memcpy(image + mask, &after, 4);
        printf("Effects: %s off (feature mask %08x -> %08x)\n", fx->name, flags, after);
    }

    const uint64_t at = gow3_signature_find(image, size, &gow3_shake_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_SHAKE_HOOK_LENGTH) ||
        memcmp(image + at, gow3_shake_original, GOW3_SHAKE_HOOK_LENGTH)) {
        printf("Camera shake: switch unavailable (camera code not found in this eboot.bin)\n");
        return;
    }
    int *cell = hook_page();
    *cell = 1;
    unsigned char *stub = hook_page();
    if (!gow3_shake_build_stub(stub, (uintptr_t)stub, (uintptr_t)cell,
                               (uintptr_t)(image + at + GOW3_SHAKE_HOOK_LENGTH))) {
        printf("Camera shake: switch unavailable (stub outside relative range)\n");
        return;
    }
    protect(stub, page_size, 5);
    if (!install_jump(at, GOW3_SHAKE_HOOK_LENGTH, stub)) {
        printf("Camera shake: switch unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3gpu_camera_shake_register(cell);
    printf("Camera shake: live setting ready (camera at image+0x%" PRIx64 ")\n", at);
}

/* Upscaler quality modes (src/scene_res_hook.h): GOW3_RENDER_RES=WxH below 1080p renders the 3D
 * scene at that size while the display, UI and movies stay 1920x1080; the GPU side upscales the
 * scene to the output before the UI is composed. The size is a cell the in-game menu rewrites:
 * the next frame makes the scene targets again at the new size. Every site is checked before
 * any is written. */
static void install_scene_resolution(Segment *segments, uint64_t count, uint64_t size) {
    const char *res = getenv("GOW3_RENDER_RES");
    unsigned width = 0, height = 0;
    if (!res || !*res || sscanf(res, "%ux%u", &width, &height) != 2) return;
    /* Below 1080p: the quality modes; wider than 16:9 (ultrawide, src/ultrawide_hook.h): up to 8K. */
    if (width < 320 || height < 180 || width > 7680 || height > 4320 || (width == 1920 && height == 1080)) {
        printf("Scene resolution: %s ignored (outside 320x180..7680x4320)\n", res);
        return;
    }
    uint64_t at[GOW3_SCENE_SIZE_SITES];
    for (unsigned i = 0; i < GOW3_SCENE_SIZE_SITES; ++i) {
        at[i] = gow3_signature_find(image, size, &gow3_scene_size_sites[i].site);
        if (at[i] == UINT64_MAX || !mapped(segments, count, at[i], 12)) {
            printf("Scene resolution: unavailable (%s size code not found in this eboot.bin)\n",
                   gow3_scene_size_sites[i].name);
            return;
        }
    }
    const uint64_t rect = gow3_signature_find(image, size, &gow3_scene_rect_site);
    if (rect == UINT64_MAX || !mapped(segments, count, rect, GOW3_SCENE_RECT_HOOK_LENGTH)) {
        printf("Scene resolution: unavailable (view copy code not found in this eboot.bin)\n");
        return;
    }
    uint64_t recreate[2];
    for (unsigned i = 0; i < 2; ++i) {
        recreate[i] = gow3_signature_find(image, size, &gow3_scene_recreate_sites[i]);
        if (recreate[i] == UINT64_MAX || !mapped(segments, count, recreate[i], GOW3_SCENE_RECREATE_HOOK_LENGTH)) {
            printf("Scene resolution: unavailable (target re-creation code not found in this eboot.bin)\n");
            return;
        }
    }
    Gow3SceneSize *cell = hook_page();
    *cell = (Gow3SceneSize){width, height, width, height};
    unsigned char *stub = hook_page();
    const unsigned rect_length = gow3_scene_rect_build_stub(stub, (uintptr_t)stub, (uintptr_t)(image + rect), cell);
    unsigned char *recreate_stub[2] = {stub + 256, stub + 384};
    if (!rect_length ||
        !gow3_scene_recreate_build_stub(recreate_stub[0], (uintptr_t)recreate_stub[0],
                                        (uintptr_t)(image + recreate[0]), cell) ||
        !gow3_scene_recreate_build_stub(recreate_stub[1], (uintptr_t)recreate_stub[1],
                                        (uintptr_t)(image + recreate[1]), cell)) {
        printf("Scene resolution: unavailable (stub outside relative range)\n");
        return;
    }
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < GOW3_SCENE_SIZE_SITES; ++i) {
        unsigned char probe_copy[12];
        memcpy(probe_copy, image + at[i], 12);
        if (!gow3_scene_size_write(probe_copy, (uintptr_t)(image + at[i]), gow3_scene_size_sites[i].width_first, cell)) {
            printf("Scene resolution: unavailable (size cell outside relative range)\n");
            return;
        }
    }
    if (!install_jump(rect, GOW3_SCENE_RECT_HOOK_LENGTH, stub)) {
        printf("Scene resolution: unavailable (hook outside relative-jump range)\n");
        return;
    }
    for (unsigned i = 0; i < GOW3_SCENE_SIZE_SITES; ++i)
        gow3_scene_size_write(image + at[i], (uintptr_t)(image + at[i]), gow3_scene_size_sites[i].width_first, cell);
    /* Without these the size stays the startup one (the menu then asks for a restart). */
    const int live = install_jump(recreate[0], GOW3_SCENE_RECREATE_HOOK_LENGTH, recreate_stub[0]) &&
                     install_jump(recreate[1], GOW3_SCENE_RECREATE_HOOK_LENGTH, recreate_stub[1]);
    if (live) gow3gpu_scene_size_register((uint32_t *)cell);
    printf("Scene resolution: 3D scene %ux%u, display and UI 1920x1080 (views at image+0x%" PRIx64 ")%s\n",
           width, height, rect, live ? ", live quality changes" : "");
}

#ifdef _WIN32
/* Free camera (src/free_camera_hook.h): F9 or the menu's switch detaches the player view's
 * camera, F10 puts it back where the game's camera is. The pose starts at the game's and the
 * game's pad stays neutral meanwhile (runtime_pad_free_camera). A different view object (a new
 * level, a cutscene camera) ends it. */
static Gow3FreeCamera free_camera;
static uintptr_t free_camera_view;
static LARGE_INTEGER free_camera_time;
static ABI void free_camera_visit(uintptr_t view, uintptr_t refresh) {
    float move[3], look[4], speed;
    unsigned keys;
    const int input = runtime_pad_free_camera_input(move, look, &speed, &keys);
    volatile uint8_t *flag = cheat_flags[GOW3_CHEAT_FREE_CAMERA];
    if ((keys & 1) && flag) *flag = !*flag;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = free_camera_time.QuadPart ? (float)(now.QuadPart - free_camera_time.QuadPart) / frequency.QuadPart : 0;
    free_camera_time = now;
    if (seconds < 0 || seconds > 0.1f) seconds = 0.1f;
    float *pose = (float *)(view + 0x10);
    if (view != free_camera_view) {
        free_camera.active = 0;
        free_camera_view = view;
    }
    const int wanted = flag && *flag;
    if (wanted && (!free_camera.active || (keys & 2))) {
        if (!gow3_free_camera_start(&free_camera, pose)) *flag = 0;
    } else if (!wanted) {
        free_camera.active = 0;
    }
    if (free_camera.active) {
        Gow3FreeCameraInput in = {0};
        if (input) {
            in = (Gow3FreeCameraInput){move[0], move[1], move[2], look[2], look[3], look[0], look[1], speed};
        }
        if (gow3_free_camera_step(&free_camera, &in, seconds)) {
            memcpy(pose, free_camera.matrix, sizeof(free_camera.matrix));
        } else {
            free_camera.active = 0;
            if (flag) *flag = 0;
        }
    }
    runtime_pad_free_camera(free_camera.active);
    ((ABI void (*)(uintptr_t))refresh)(view);
}
#endif

static void install_free_camera(Segment *segments, uint64_t count, uint64_t size) {
#ifdef _WIN32
    const uint64_t at = gow3_signature_find(image, size, &gow3_free_camera_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_FREE_CAMERA_LENGTH) ||
        memcmp(image + at, gow3_free_camera_original, GOW3_FREE_CAMERA_LENGTH)) {
        printf("Free camera: unavailable (camera code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = hook_page();
    if (!gow3_free_camera_build_stub(stub, (uintptr_t)stub, (uintptr_t)free_camera_visit,
                                     (uintptr_t)(image + at + GOW3_FREE_CAMERA_LENGTH))) {
        printf("Free camera: unavailable (stub outside relative range)\n");
        return;
    }
    protect(stub, page_size, 5);
    if (!install_jump(at, GOW3_FREE_CAMERA_LENGTH, stub)) {
        printf("Free camera: unavailable (hook outside relative-jump range)\n");
        return;
    }
    gow3_cheat_status(GOW3_CHEAT_FREE_CAMERA);
    printf("Free camera: ready (F9 toggle, F10 recenter; camera at image+0x%" PRIx64 ")\n", at);
#else
    (void)segments; (void)count; (void)size;
#endif
}

/* Hor+ for outputs wider than 16:9 (src/ultrawide_hook.h): the aspect of the output the launcher
 * chose (GOW3_OUTPUT_RES), whose scene the loader renders at that shape (GOW3_RENDER_RES). */
static void install_ultrawide(Segment *segments, uint64_t count, uint64_t size) {
    const char *res = getenv("GOW3_OUTPUT_RES");
    unsigned width = 0, height = 0;
    if (!res || !*res || sscanf(res, "%ux%u", &width, &height) != 2 || !width || !height) return;
    const float k = ((float)width / (float)height) / (16.0f / 9.0f);
    if (k <= 1.01f) return;
    const uint64_t at = gow3_signature_find(image, size, &gow3_ultrawide_frustum_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, GOW3_ULTRAWIDE_HOOK_LENGTH)) {
        printf("Ultrawide: unavailable (camera frustum code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = hook_page();
    if (!gow3_ultrawide_build_stub(stub, (uintptr_t)stub, (uintptr_t)(image + at), k)) {
        printf("Ultrawide: unavailable (stub outside relative range)\n");
        return;
    }
    protect(stub, page_size, 5);
    if (!install_jump(at, GOW3_ULTRAWIDE_HOOK_LENGTH, stub)) {
        printf("Ultrawide: unavailable (hook outside relative-jump range)\n");
        return;
    }
    printf("Ultrawide: %ux%u, Hor+ field of view x%.4f (camera frustum at image+0x%" PRIx64 ")\n",
           width, height, k, at);
}

/* Native PC button prompts (src/prompt_hook.h): for each icon set (keyboard and mouse, Xbox) one
 * copy per distinct game atlas, a slot claimed by the atlas texture's address (state 1 preparing,
 * 2 ready, 3 refused). */
static Gow3PromptAtlas prompt_atlases[GOW3_PROMPT_SETS][GOW3_PROMPT_ATLASES];
static uint64_t prompt_sources[GOW3_PROMPT_ATLASES];
static int prompt_states[GOW3_PROMPT_SETS][GOW3_PROMPT_ATLASES];
static int prompt_any_ready;
static unsigned prompt_draws[GOW3_PROMPT_SETS];
/* Rich-text ImageMaps (the menu legends), a slot claimed by the map's address like the atlases. */
static Gow3PromptMenu prompt_menus[GOW3_PROMPT_SETS][GOW3_PROMPT_MENUS];
static uint64_t prompt_menu_maps[GOW3_PROMPT_MENUS];
static int prompt_menu_states[GOW3_PROMPT_SETS][GOW3_PROMPT_MENUS];
static unsigned prompt_text_images[GOW3_PROMPT_SETS];
static unsigned prompt_sheet_draws[GOW3_PROMPT_SETS];
static unsigned prompt_widget_draws[GOW3_PROMPT_SETS];
static const Gow3PromptBinding prompt_bindings[GOW3_PROMPT_SETS] = {
    runtime_pad_prompt_binding, runtime_pad_prompt_xbox_binding};
static const char *const prompt_set_names[GOW3_PROMPT_SETS] = {"keyboard and mouse", "Xbox"};

static int prompt_read(uintptr_t address, void *out, size_t size) {
    if (address < 0x10000 || address > UINTPTR_MAX - size) return 0;
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
#else
    extern uint64_t runtime_memory_clamp(uintptr_t, uint64_t);
    if (runtime_memory_clamp(address, size) != size) return 0;
    memcpy(out, (const void *)address, size);
    return 1;
#endif
}

/* A game texture of our pixels, built as the generated font texture creator 0x31a540 does but
 * RGBA8888 (format 15) over guest pixels the texture does not own. Kept until exit. */
static uint64_t prompt_create(const uint32_t *pixels, unsigned width, unsigned height) {
    typedef void *(ABI *NativeNew)(size_t);
    typedef void (ABI *NativeCtor)(void *);
    typedef uint64_t (ABI *NativeTexture)(void *, uint32_t, uint32_t, const uint32_t *, uint32_t, uint32_t,
                                         uint16_t, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint32_t, char);
    const size_t bytes = (size_t)width * height * 4;
    uint32_t *data = runtime_memory_map_host_pixels(bytes);
    if (!data || ((uintptr_t)data & 255)) {
        printf("Button prompts: no memory for the atlas; PS4 icons kept\n");
        return 0;
    }
    memcpy(data, pixels, bytes);
    void *texture = ((NativeNew)(image + GOW3_PROMPT_NEW))(0xc0);
    if (!texture) return 0;
    ((NativeCtor)(image + GOW3_PROMPT_CTOR))(texture);
    const uint32_t description[3] = {width, height, 15};
    if (!((NativeTexture)(image + GOW3_PROMPT_TEXTURE_INIT))(texture, 0, 1, description, 0, 0, 1, 0, 0, 2,
                                                            (uintptr_t)data, 256, 0, 0))
        return 0;
    /* The game's own record of it: size, pitch, and a T# over our pixels (linear or 2D tiled). */
    const unsigned char *object = texture;
    uint32_t dims[2], pitch;
    uint64_t srd[2];
    memcpy(dims, object + 0x18, 8);
    memcpy(&pitch, object + 0x8c, 4);
    memcpy(srd, object + 0x90, 16);
    const unsigned tile = (unsigned)(srd[1] >> 52) & 31;
    if (dims[0] != width || dims[1] != height ||
        pitch != width * 4 || ((srd[0] & UINT64_C(0x3fffffffff)) << 8) != (uintptr_t)data ||
        ((srd[0] >> 52) & 63) != 10 || (srd[1] & 0x3fff) != width - 1 ||
        ((srd[1] >> 14) & 0x3fff) != height - 1 || (tile != 8 && tile != 31)) {
        printf("Button prompts: the game refused the atlas (pitch %u, tile %u); PS4 icons kept\n", pitch, tile);
        return 0;
    }
    return (uintptr_t)texture;
}

/* The icon lookups' entry (rdi: the glyph collection, which the lookup itself reads). A new
 * atlas is prepared here, on the game's thread, before any of its icons is drawn. */
static ABI void prompt_lookup(uintptr_t collection, uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;
    if (!collection) return;
    const int32_t glyphs = *(const int32_t *)(collection + 0x18);
    if (glyphs <= 0 || glyphs > GOW3_PROMPT_GLYPHS) return;
    const uint64_t source = *(const uint64_t *)(collection + 0x50);
    if (!source) return;
    /* Only a button atlas claims one of the few slots (any glyph count since #91). */
    uint64_t vtable = 0;
    uint32_t dims[2] = {0, 0};
    if (!prompt_read(source, &vtable, 8) || vtable != (uintptr_t)image + GOW3_PROMPT_ATLAS_VTABLE ||
        !prompt_read(source + 0x18, dims, sizeof(dims)) || dims[0] != GOW3_PROMPT_ATLAS_WIDTH ||
        dims[1] != GOW3_PROMPT_ATLAS_HEIGHT) return;
    unsigned slot = GOW3_PROMPT_ATLASES;
    for (unsigned i = 0; i < GOW3_PROMPT_ATLASES; ++i) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&prompt_sources[i], &expected, source, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) { slot = i; break; }
        if (expected == source) return; /* known (or being prepared by another thread) */
    }
    if (slot == GOW3_PROMPT_ATLASES) return;
    for (unsigned set = 0; set < GOW3_PROMPT_SETS; ++set) {
        __atomic_store_n(&prompt_states[set][slot], 1, __ATOMIC_RELEASE);
        Gow3PromptAtlas *atlas = &prompt_atlases[set][slot];
        const int ready = gow3_prompt_prepare(atlas, (uintptr_t)image, collection, prompt_read,
                                              prompt_bindings[set], prompt_create);
        __atomic_store_n(&prompt_states[set][slot], ready ? 2 : 3, __ATOMIC_RELEASE);
        if (ready) __atomic_store_n(&prompt_any_ready, 1, __ATOMIC_RELEASE);
        printf("Button prompts: atlas %u %s %s (icons with art %04x, %u glyphs)\n", slot, prompt_set_names[set],
               ready ? "copy ready" : "not recognized, PS4 icons kept", atlas->supported, atlas->glyphs);
    }
    fflush(stdout);
}

/* Rich-text images left as they are, logged once each (texture and rect), to name new ones. */
static uint64_t prompt_diag_seen[64];
static int prompt_diag_new(uint64_t key) {
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&prompt_diag_seen[i], &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
        if (expected == key) return 0;
    }
    return 0;
}
static int prompt_menu_swap(Gow3PromptMenu *menu, uintptr_t command, uint64_t size, uintptr_t uv) {
    for (unsigned s = 0; s < menu->sheets; ++s)
        if (menu->sheet[s].glyphs && gow3_prompt_swap(&menu->sheet[s], command, size, uv)) return 1;
    return 0;
}

/* Before each UI quad draw (rdi command, rdx size, rcx UV quad). */
static ABI void prompt_draw(uintptr_t command, uint64_t size, uintptr_t uv) {
    if (!__atomic_load_n(&prompt_any_ready, __ATOMIC_ACQUIRE)) return;
    const int wanted = runtime_pad_prompt_set();
    /* A command left on another set's copy (the setting or the device changed) goes back to the
     * game's holder first: each set's swap only knows its own holders. */
    for (unsigned set = 0; set < GOW3_PROMPT_SETS; ++set) {
        if ((int)set == wanted) continue;
        for (unsigned i = 0; i < GOW3_PROMPT_ATLASES; ++i)
            if (__atomic_load_n(&prompt_states[set][i], __ATOMIC_ACQUIRE) == 2)
                gow3_prompt_release(&prompt_atlases[set][i], command);
        for (unsigned i = 0; i < GOW3_PROMPT_MENUS; ++i)
            if (__atomic_load_n(&prompt_menu_states[set][i], __ATOMIC_ACQUIRE) == 2)
                for (unsigned s = 0; s < prompt_menus[set][i].sheets; ++s)
                    gow3_prompt_release(&prompt_menus[set][i].sheet[s], command);
    }
    if (wanted < 0) return;
    for (unsigned i = 0; i < GOW3_PROMPT_ATLASES; ++i) {
        if (__atomic_load_n(&prompt_states[wanted][i], __ATOMIC_ACQUIRE) != 2 ||
            !gow3_prompt_swap(&prompt_atlases[wanted][i], command, size, uv)) continue;
        if (__atomic_add_fetch(&prompt_draws[wanted], 1, __ATOMIC_RELAXED) == 1) {
            printf("Button prompts: first %s icon drawn\n", prompt_set_names[wanted]);
            fflush(stdout);
        }
        return;
    }
    for (unsigned i = 0; i < GOW3_PROMPT_MENUS; ++i) {
        if (__atomic_load_n(&prompt_menu_states[wanted][i], __ATOMIC_ACQUIRE) != 2 ||
            !prompt_menu_swap(&prompt_menus[wanted][i], command, size, uv)) continue;
        if (__atomic_add_fetch(&prompt_sheet_draws[wanted], 1, __ATOMIC_RELAXED) == 1) {
            printf("Button prompts: first %s icon drawn from a button sheet (menu widgets)\n",
                   prompt_set_names[wanted]);
            fflush(stdout);
        }
        return;
    }
}

/* A rich-text inline image about to be added (rdi the text builder, *texture and *rect its r8
 * and r9, see src/prompt_hook.h): atlas glyphs and ImageMap buttons with a key are added from our
 * textures instead. Runs while text is laid out, so a device switch shows on the next layout. */
static ABI void prompt_text_image(uintptr_t builder, uint64_t *texture, uint64_t *rect) {
    const int wanted = runtime_pad_prompt_set();
    if (!*texture || !*rect || wanted < 0) return;
    if (__atomic_load_n(&prompt_any_ready, __ATOMIC_ACQUIRE)) {
        int32_t glyph[4];
        uint64_t render = 0;
        if (!prompt_read(*rect, glyph, sizeof(glyph)) || !prompt_read(*texture + 0x18, &render, 8)) return;
        for (unsigned i = 0; i < GOW3_PROMPT_ATLASES; ++i) {
            if (__atomic_load_n(&prompt_states[wanted][i], __ATOMIC_ACQUIRE) != 2) continue;
            Gow3PromptAtlas *pc = &prompt_atlases[wanted][i];
            if (!gow3_prompt_text_texture(pc, render, glyph)) continue;
            /* The first resource seen lends its header (written once, before it is handed out). */
            if (!__atomic_load_n(&pc->resource[3], __ATOMIC_ACQUIRE)) {
                if (!prompt_read(*texture, pc->resource, 0x18)) return;
                __atomic_store_n(&pc->resource[3], pc->holder[0], __ATOMIC_RELEASE);
            }
            *texture = (uintptr_t)pc->resource;
            goto swapped;
        }
    }
    /* The ImageMap of the builder's current style. */
    int32_t depth = 0;
    uint64_t styles = 0, style = 0, map = 0;
    if (!prompt_read(builder + 0x30, &depth, 4) || depth <= 0 || !prompt_read(builder + 0x38, &styles, 8) ||
        !prompt_read(styles + (uint64_t)(depth - 1) * 8, &style, 8) || !prompt_read(style + 0x30, &map, 8) || !map)
        return;
    unsigned slot = GOW3_PROMPT_MENUS;
    for (unsigned i = 0; i < GOW3_PROMPT_MENUS && slot == GOW3_PROMPT_MENUS; ++i) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&prompt_menu_maps[i], &expected, map, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            for (unsigned set = 0; set < GOW3_PROMPT_SETS; ++set) {
                Gow3PromptMenu *menu = &prompt_menus[set][i];
                __atomic_store_n(&prompt_menu_states[set][i], 1, __ATOMIC_RELEASE);
                const int ready = gow3_prompt_prepare_menu(menu, map, prompt_read, prompt_bindings[set],
                                                           prompt_create);
                __atomic_store_n(&prompt_menu_states[set][i], ready ? 2 : 3, __ATOMIC_RELEASE);
                if (ready) __atomic_store_n(&prompt_any_ready, 1, __ATOMIC_RELEASE);
                printf("Button prompts: text images %u %s %s (%u buttons with art in %u sheets; other images: %s)\n",
                       i, prompt_set_names[set], ready ? "copy ready" : "without buttons, kept", menu->images,
                       menu->sheets, menu->others);
                if (ready) printf("Button prompts: text images %u buttons: %s\n", i, menu->buttons);
            }
            fflush(stdout);
            slot = i;
        } else if (expected == map) {
            slot = i;
        }
    }
    if (slot == GOW3_PROMPT_MENUS || __atomic_load_n(&prompt_menu_states[wanted][slot], __ATOMIC_ACQUIRE) != 2) return;
    int32_t r[4] = {0, 0, 0, 0};
    uint64_t render = 0;
    if (!prompt_read(*rect, r, sizeof(r)) || !prompt_read(*texture + 0x18, &render, 8)) return;
    const uint64_t key = gow3_prompt_menu_image(&prompt_menus[wanted][slot], render, r);
    if (!key) {
        uint32_t dims[2] = {0, 0};
        prompt_read(render + 0x18, dims, sizeof(dims));
        if (prompt_diag_new(render ^ ((uint64_t)(uint32_t)r[0] << 40) ^ ((uint64_t)(uint32_t)r[1] << 52))) {
            printf("Button prompts: text image kept: texture %llx %ux%u rect %d,%d-%d,%d map %llx slot %u\n",
                   (unsigned long long)render, dims[0], dims[1], r[0], r[1], r[2], r[3], (unsigned long long)map, slot);
            fflush(stdout);
        }
        return;
    }
    *texture = key;
swapped:
    if (__atomic_add_fetch(&prompt_text_images[wanted], 1, __ATOMIC_RELAXED) == 1) {
        printf("Button prompts: first %s icon in rich text (menus)\n", prompt_set_names[wanted]);
        fflush(stdout);
    }
}

/* A TextureWidget about to draw (rdi; see src/prompt_hook.h): one cut from a button sheet points
 * at the sheet's copy while every element is a button with a key and the prompts are PC. */
static ABI void prompt_texture_widget(uintptr_t widget, uint64_t unused1, uint64_t unused2) {
    (void)unused1; (void)unused2;
    if (!__atomic_load_n(&prompt_any_ready, __ATOMIC_ACQUIRE)) return;
    uint64_t *bound = (uint64_t *)(widget + 0x340);
    const uint64_t render = *bound;
    if (!render) return;
    const int wanted = runtime_pad_prompt_set();
    /* The sheet it is cut from (or any set's copy of it), then the wanted set's copy when every
     * element has art there. */
    for (unsigned i = 0; i < GOW3_PROMPT_MENUS; ++i) {
        uint64_t original = 0;
        for (unsigned set = 0; set < GOW3_PROMPT_SETS && !original; ++set) {
            if (__atomic_load_n(&prompt_menu_states[set][i], __ATOMIC_ACQUIRE) != 2) continue;
            const Gow3PromptMenu *menu = &prompt_menus[set][i];
            for (unsigned s = 0; s < menu->sheets && !original; ++s)
                if (menu->sheet[s].glyphs && (render == menu->sheet[s].original_texture ||
                                              render == menu->sheet[s].holder[0]))
                    original = menu->sheet[s].original_texture;
        }
        if (!original) continue;
        uint64_t pc = 0;
        if (wanted >= 0 && __atomic_load_n(&prompt_menu_states[wanted][i], __ATOMIC_ACQUIRE) == 2) {
            const Gow3PromptMenu *menu = &prompt_menus[wanted][i];
            const uint64_t resource = *(const uint64_t *)(widget + 0x328);
            uint32_t dims[2] = {0, 0};
            for (unsigned s = 0; s < menu->sheets && !pc; ++s) {
                const Gow3PromptAtlas *sheet = &menu->sheet[s];
                if (sheet->glyphs && sheet->original_texture == original && resource &&
                    prompt_read(original + 0x18, dims, sizeof(dims)) &&
                    gow3_prompt_widget_art(sheet, dims[0], dims[1], resource, prompt_read))
                    pc = sheet->holder[0];
            }
        }
        *bound = pc ? pc : original;
        if (pc && __atomic_add_fetch(&prompt_widget_draws[wanted], 1, __ATOMIC_RELAXED) == 1) {
            printf("Button prompts: first %s icon in a texture widget (menu tabs)\n", prompt_set_names[wanted]);
            fflush(stdout);
        }
        return;
    }
}

static void install_prompts(Segment *segments, uint64_t count, uint64_t size) {
    int found = mapped(segments, count, GOW3_PROMPT_DRAW, 27) && gow3_prompt_code_match(image, size);
    for (unsigned i = 0; i < GOW3_PROMPT_LOOKUPS; ++i)
        found = found && mapped(segments, count, gow3_prompt_lookups[i].address, gow3_prompt_lookups[i].replay);
    found = found && mapped(segments, count, GOW3_PROMPT_TEXT_IMAGE, GOW3_PROMPT_ENTRY_LENGTH) &&
            mapped(segments, count, GOW3_PROMPT_TEXTURE_WIDGET, GOW3_PROMPT_ENTRY_LENGTH);
    if (!found) {
        printf("Button prompts: PC icons unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stubs = hook_page();
    gow3_prompt_build_stub(stubs, (uintptr_t)prompt_draw, image + GOW3_PROMPT_DRAW, GOW3_PROMPT_ENTRY_LENGTH,
                           (uintptr_t)(image + GOW3_PROMPT_DRAW + GOW3_PROMPT_ENTRY_LENGTH));
    for (unsigned i = 0; i < GOW3_PROMPT_LOOKUPS; ++i) {
        const uint32_t at = gow3_prompt_lookups[i].address;
        const unsigned replay = gow3_prompt_lookups[i].replay;
        gow3_prompt_build_stub(stubs + 256 * (i + 1), (uintptr_t)prompt_lookup, image + at, replay,
                               (uintptr_t)(image + at + replay));
    }
    unsigned char *text_stub = stubs + 256 * (GOW3_PROMPT_LOOKUPS + 1);
    gow3_prompt_build_text_stub(text_stub, (uintptr_t)prompt_text_image, image + GOW3_PROMPT_TEXT_IMAGE,
                                GOW3_PROMPT_ENTRY_LENGTH,
                                (uintptr_t)(image + GOW3_PROMPT_TEXT_IMAGE + GOW3_PROMPT_ENTRY_LENGTH));
    unsigned char *widget_stub = stubs + 256 * (GOW3_PROMPT_LOOKUPS + 2);
    gow3_prompt_build_stub(widget_stub, (uintptr_t)prompt_texture_widget, image + GOW3_PROMPT_TEXTURE_WIDGET,
                           GOW3_PROMPT_ENTRY_LENGTH,
                           (uintptr_t)(image + GOW3_PROMPT_TEXTURE_WIDGET + GOW3_PROMPT_ENTRY_LENGTH));
    protect(stubs, page_size, 5);
    int installed = 1;
    for (unsigned i = 0; i < GOW3_PROMPT_LOOKUPS && installed; ++i)
        installed = install_jump(gow3_prompt_lookups[i].address, gow3_prompt_lookups[i].replay, stubs + 256 * (i + 1));
    if (!installed || !install_jump(GOW3_PROMPT_DRAW, GOW3_PROMPT_ENTRY_LENGTH, stubs) ||
        !install_jump(GOW3_PROMPT_TEXT_IMAGE, GOW3_PROMPT_ENTRY_LENGTH, text_stub) ||
        !install_jump(GOW3_PROMPT_TEXTURE_WIDGET, GOW3_PROMPT_ENTRY_LENGTH, widget_stub)) {
        printf("Button prompts: PC icons unavailable (hook outside relative-jump range)\n");
        return;
    }
    printf("Button prompts: PC icons ready (%u icon lookups, UI quad draw at image+0x%x, rich-text images at "
           "image+0x%x, texture widgets at image+0x%x)\n", GOW3_PROMPT_LOOKUPS, GOW3_PROMPT_DRAW,
           GOW3_PROMPT_TEXT_IMAGE, GOW3_PROMPT_TEXTURE_WIDGET);
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t damage_at = gow3_signature_find(image, size, &gow3_damage_site);
    const uint64_t pickup_at = gow3_signature_find(image, size, &gow3_orb_pickup_site);
    int damage = damage_at != UINT64_MAX && mapped(segments, count, damage_at, GOW3_DAMAGE_HOOK_LENGTH) &&
                 gow3_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = pickup_at != UINT64_MAX && mapped(segments, count, pickup_at, GOW3_ORB_PICKUP_LENGTH) &&
                 gow3_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? hook_page() : NULL;
    if (damage) {
        gow3_damage_build_stub(stubs, gow3_damage_multiplier(1), gow3_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + GOW3_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        gow3_orb_pickup_build_stub(stubs + 256, gow3_orb_pickup_multiplier(0), gow3_orb_pickup_multiplier(1),
                                   gow3_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + GOW3_ORB_PICKUP_LENGTH));
    }
    if (stubs) protect(stubs, page_size, 5);
    damage = damage && install_jump(damage_at, GOW3_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, GOW3_ORB_PICKUP_LENGTH, stubs + 256);
    gow3_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
/* G3PATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"G3PATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
    /* Started by the launcher (GOW3_LAUNCHER_RESTART=1): it starts the game again itself on exit
     * code 75, recomputing every option (upscaler and frame generation chosen in the menu too). */
    const char *launcher=getenv("GOW3_LAUNCHER_RESTART");
    if (launcher && launcher[0]=='1') {
        puts("Runtime: restarting through the launcher");
        fflush(NULL);
        _exit(75);
    }
#ifdef _WIN32
    /* run.py sets GOW3_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("GOW3_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: GOW3_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    const char *command = getenv("GOW3_RESTART_COMMAND");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    if (command && *command) {
        puts("Runtime: restarting through GOW3_RESTART_COMMAND");
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    } else {
        puts("Runtime: restarting through run.sh");
        execlp("bash", "bash", "run.sh", (char *)NULL);
    }
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

/* The guest code needs the PS4's instruction sets (#33, #66); GOW3_CPU_CHECK=0 skips this. A
 * missing fatal set prints STOP and returns 0; the rewritable ones go to cpu_missing_sets, named
 * in missing. */
static int cpu_supported(char *missing, size_t size) {
    missing[0] = 0;
    const char *check = getenv("GOW3_CPU_CHECK");
    if (check && !strcmp(check, "0")) return 1;
    const CpuFeatureWords words = cpu_feature_words();
    if (cpu_missing_fatal(words, missing, size)) {
        printf("STOP: this CPU lacks instruction sets the game's code uses: %s "
               "(Intel Sandy Bridge 2011 or newer, or AMD FX or newer, is needed)\n", missing);
        return 0;
    }
    cpu_missing_sets = cpu_missing_rewritable(words, missing, size);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    char missing[128];
    if (argc == 2 && !strcmp(argv[1], "--cpu-features")) {
        if (!cpu_supported(missing, sizeof missing)) return 20;
        if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
        else puts("CPU: complete");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--gamepad")) {
        /* Before the memory reservation: this only asks SDL, nothing of the game is set up yet. */
        runtime_pad_describe();
        return 0;
    }
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *isa_file=NULL;
    const char *mounts[8]; int mount_count=0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--mount") && i+1<argc) {
            /* guest=host; the guest part never has '=', so the first one splits */
            const char *spec=argv[++i], *eq=strchr(spec,'=');
            if (mount_count==8 || !eq || eq==spec || !eq[1]) { fprintf(stderr, "Bad --mount (guest=host, at most 8): %s\n", spec); return 1; }
            mounts[mount_count++]=spec;
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--isa-rewrite file] [--mount guest=host]... [--timeout seconds] | --vulkan-only | --cpu-features | --gamepad\n", argv[0]);
        return 1;
    }
    if (!cpu_supported(missing, sizeof missing)) return 20;
    if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
    /* GOW3_ISA_REWRITE=0 never rewrites, =1 rewrites every site; by default the sets the CPU lacks. */
    const char *policy = getenv("GOW3_ISA_REWRITE");
    unsigned isa_sets = policy && !strcmp(policy, "1") ? 0xffffffffu : policy && !strcmp(policy, "0") ? 0 : cpu_missing_sets;
    if (cpu_missing_sets && !isa_sets) {
        printf("STOP: this CPU lacks %s and the rewrite is disabled (GOW3_ISA_REWRITE=0)\n", missing);
        return 20;
    }
    if (cpu_missing_sets && !isa_file) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"G3CONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        for (int i = 0; i < mount_count; ++i) {
            const char *eq = strchr(mounts[i], '=');
            char guest[64];
            size_t glen = (size_t)(eq - mounts[i]);
            if (glen >= sizeof(guest)) { fprintf(stderr, "Cannot mount %s: guest path longer than 63 characters\n", mounts[i]); return 1; }
            memcpy(guest, mounts[i], glen); guest[glen] = 0;
            if (runtime_file_mount(guest, eq + 1)) { fprintf(stderr, "Cannot mount %s: too many mounts or host path longer than 511 characters\n", mounts[i]); return 1; }
            printf("Runtime: mount %s -> %s\n", guest, eq + 1);
        }
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        /* GOW3_SAVEDATA_ID (launcher): one save folder for every region of the game. */
        const char *save_id=getenv("GOW3_SAVEDATA_ID");
        if (save_id && *save_id)
            runtime_savedata_configure(save_id);
        else if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    gow3gpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("GOW3_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"GOW3_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); /* int3 ISA sites */
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8) && memcmp(magic,"G3PROBE6",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int with_dynamic=!memcmp(magic,"G3PROBE6",8);
    int multi=!memcmp(magic,"G3PROBE5",8) || with_dynamic;
    int linked=!memcmp(magic,"G3PROBE3",8) || !memcmp(magic,"G3PROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* G3PROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"G3PROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    /* G3PROBE6: modules the game loads itself (sceKernelLoadStartModule), already placed and
     * relocated in the image; registered with the runtime once the image is mapped. */
    uint64_t dynamic_count=with_dynamic ? read64(f) : 0;
    if (dynamic_count>8) fail("invalid dynamic module count");
    RuntimeDynamicModule dynamic[8];
    for (uint64_t m=0;m<dynamic_count;++m) {
        RuntimeDynamicModule *d=&dynamic[m];
        if (fread(d->path,1,sizeof(d->path),f)!=sizeof(d->path) || !memchr(d->path,0,sizeof(d->path))) fail("truncated dynamic module path");
        d->base=read64(f); d->size=read64(f); d->export_count=read64(f);
        if (d->base>=size || !d->size || d->size>size-d->base || d->export_count>100000) fail("invalid dynamic module metadata");
        d->exports=calloc(d->export_count ? d->export_count : 1,sizeof(*d->exports));
        if (!d->exports) fail("allocation failed");
        for (uint64_t e=0;e<d->export_count;++e) {
            if (fread(d->exports[e].nid,1,16,f)!=16 || !memchr(d->exports[e].nid,0,16)) fail("truncated dynamic export");
            d->exports[e].address=read64(f);
            if (d->exports[e].address<d->base || d->exports[e].address-d->base>=d->size) fail("dynamic export outside its module");
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    isa_table = isa_sets && isa_file ? isa_table_load(isa_file) : NULL;
    if (!isa_table && cpu_missing_sets) { printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing); return 20; }
    /* The stubs follow the image in the same low mapping (rel32 jmps reach them). */
    size_t stub_area = isa_table ? round_page(isa_stub_area_size(isa_table)) : 0;
    /* PS4 TSC trampolines (tsc_patch.c) follow the ISA stubs in the same mapping. */
    size_t tsc_area = !cpu_only && runtime_tsc_calibrate() ? TSC_PATCH_AREA : 0;
    image = allocate(round_page(size) + stub_area + tsc_area + HOOK_PAGES * page_size);
    unsigned char *stubs = image + round_page(size);
    unsigned char *tsc_trampolines = stubs + stub_area;
    hook_pages = tsc_trampolines + tsc_area;
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    if (!cpu_only) {
        char title[128]="God of War III", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        /* GOW3_WINDOW_SIZE=<w>x<h>: the window's first size (launcher); the game image stays 1080p. */
        unsigned window_w=1920, window_h=1080;
        const char *size_env=getenv("GOW3_WINDOW_SIZE");
        if (size_env) {
            unsigned w=0, h=0;
            if (sscanf(size_env,"%ux%u",&w,&h)==2 && w>=640 && h>=360 && w<=7680 && h<=4320) { window_w=w; window_h=h; }
        }
        Gow3GpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,window_w,window_h};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (gow3gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,gow3gpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (gpu_enabled) install_orb_multiplier(segments, ns, size);
    if (gpu_enabled) install_cheats(segments, ns, size);
    if (gpu_enabled) install_actor_watch(segments, ns, size);
    if (gpu_enabled) install_stat_multipliers(segments, ns, size);
    install_mouse_look(segments, ns, size);
    if (gpu_enabled) install_fov(segments, ns, size);
    if (gpu_enabled) install_lod_distance(segments, ns, size);
    if (gpu_enabled) install_post_effects(segments, ns, size);
    if (gpu_enabled) install_scene_resolution(segments, ns, size);
    if (gpu_enabled) install_ultrawide(segments, ns, size);
    if (gpu_enabled) install_prompts(segments, ns, size);
    if (gpu_enabled) install_free_camera(segments, ns, size);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    if (tsc_area) {
        size_t used=0;
        TscReport total={0};
        for (uint64_t i=0;i<ns;++i) {
            if (!(segments[i].flags & 1)) continue;
            TscReport r=tsc_patch_segment(image,segments[i].address,segments[i].size,tsc_trampolines,&used,runtime_tsc_multiplier());
            total.found+=r.found; total.windows+=r.windows; total.moved+=r.moved; total.skipped+=r.skipped;
        }
        printf("TSC: %u rdtsc found, %u patch windows (+%u moved with a neighbour), %u left unpatched; %zu trampoline bytes\n",
               total.found,total.windows,total.moved,total.skipped,used);
        protect(tsc_trampolines,tsc_area,5);
    }
    if (isa_table) {
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            printf("STOP: this CPU lacks %s and %u sites use encodings the rewrite cannot emulate\n", missing, r.unsupported_needed);
            fflush(NULL);
            _exit(20); /* no destructors: GPU threads are running */
        }
        if (isa_sets == 0xffffffffu) printf("ISA: forced rewrite of all %u sites (GOW3_ISA_REWRITE=1), %u skipped\n", r.rewritten, r.skipped);
        else printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n",
                    missing, r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        runtime_set_report_hook(isa_report_hook);
        protect(stubs, stub_area, 5);
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations at %p\n", size, ns, nr, (void *)image);
    runtime_image_start=(uintptr_t)image; runtime_image_end=(uintptr_t)image+size;
    for (uint64_t m=0;m<dynamic_count;++m) runtime_module_register(&dynamic[m],image);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        runtime_set_guest_fpu();
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
#ifdef _WIN32
    profiler_start(describe_address);
#endif
    printf("Guest image base %p\n", (void *)image);
    if (gpu_enabled) gow3gpu_set_guest_image(image);
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
