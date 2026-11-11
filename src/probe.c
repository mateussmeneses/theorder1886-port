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
#include "task_idle.h"
#include "gpu/theordergpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
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
/* Exact host stack from the x64 unwind tables (.pdata); stops at guest code, which has none. */
static void print_unwound_stack(const CONTEXT *c) {
    char where[MAX_PATH+64];
    CONTEXT u=*c;
    fprintf(stderr,"  unwound stack:\n");
    for (int depth=0; depth<32 && u.Rip; ++depth) {
        describe_address(where,sizeof(where),(uintptr_t)u.Rip);
        fprintf(stderr,"    %2d %s\n",depth,where);
        DWORD64 base=0; PVOID handler_data; DWORD64 establisher;
        PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(u.Rip,&base,NULL);
        if (fn) RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,u.Rip,fn,&u,&handler_data,&establisher,NULL);
        else if (depth==0) { u.Rip=*(DWORD64 *)u.Rsp; u.Rsp+=8; } /* leaf function */
        else break;
    }
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
    PWSTR thread_name=NULL;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(),&thread_name)) && thread_name && *thread_name)
        fprintf(stderr,"  thread name: %ls\n",thread_name);
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    fprintf(stderr,"  r8=%016llx r9=%016llx r10=%016llx r11=%016llx\n  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15);
    /* The rare startup fault in the task queue pop (docs/ENGINE_RE.md): rbx = queue, [rbx] = its
     * header (head +0, tail +0x40, free list +0x80, count +0xc0), [rbx+0x148] = node array. */
    if (image && c->Rip==(uintptr_t)image+0x20aa22) {
        uintptr_t header=0, nodes=0, words[4]={0};
        ReadProcessMemory(GetCurrentProcess(),(void *)c->Rbx,&header,8,NULL);
        ReadProcessMemory(GetCurrentProcess(),(void *)(c->Rbx+0x148),&nodes,8,NULL);
        for (int i=0;i<4;++i) ReadProcessMemory(GetCurrentProcess(),(void *)(header+i*0x40),&words[i],8,NULL);
        fprintf(stderr,"  task queue %p: header %p head=%016llx tail=%016llx free=%016llx count=%lld nodes %p\n",
                (void *)c->Rbx,(void *)header,(unsigned long long)words[0],(unsigned long long)words[1],
                (unsigned long long)words[2],(long long)words[3],(void *)nodes);
    }
    print_unwound_stack(c);
    if (image) {
        fprintf(stderr, "  globals: [0x16b1df0]=%p [0x16b3420]=%p [0x16b3418]=%p\n",
                *(void**)(image + 0x16b1df0), *(void**)(image + 0x16b3420), *(void**)(image + 0x16b3418));
    }
    if (gpu_enabled) theordergpu_dump_guest_writes(info);
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
    uintptr_t *sp = (uintptr_t *)c->Rsp;
    fprintf(stderr, "  stack trace (rsp=%p):\n", (void *)sp);
    int printed = 0;
    for (int i = 0; i < 256 && printed < 20; ++i) {
        uintptr_t val = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), &sp[i], &val, sizeof(val), &got) || got != sizeof(val))
            break;
        HMODULE mod = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)val, &mod) && mod) {
            char desc[MAX_PATH + 64];
            describe_address(desc, sizeof(desc), val);
            fprintf(stderr, "    [rsp+0x%x] %p -> %s\n", i * 8, (void *)val, desc);
            printed++;
        }
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
    if (code==EXCEPTION_BREAKPOINT && isa_table) {
        /* F16C sites work on XMM registers, which only the trapped context holds. */
        if (isa_rewrite_trap_context(isa_table,info->ContextRecord,(uintptr_t)record->ExceptionAddress))
            return EXCEPTION_CONTINUE_EXECUTION;
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
        if (gpu_enabled && theordergpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in port_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)port_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code != 0x406d1388 /* SetThreadName */ && code != 0xe06d7363 /* C++ exception */) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int in_image=(image && rip>=(uintptr_t)image && rip<(uintptr_t)image+0x3000000);
        int in_self=(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self);
        int ours=in_image || in_self;
        if (ours) return fatal_exception(info);
    }
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
static DWORD WINAPI win_watchdog_thread(LPVOID param) {
    DWORD sec = (DWORD)(uintptr_t)param;
    Sleep(sec * 1000);
    runtime_memory_report(); /* the direct memory still in use: long runs show a leak */
    if (isa_table) isa_rewrite_report(isa_table,stdout); /* what the ISA emulation ran */
    fflush(stdout);
    fprintf(stderr, "\nSTOP: watchdog timeout after %u seconds; thread stacks:\n", (unsigned)sec);
    /* As the Linux watchdog: a run that stopped presenting shows where every thread waits. */
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    if (snapshot!=INVALID_HANDLE_VALUE && Thread32First(snapshot,&entry)) {
        do {
            if (entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId())
                continue;
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,
                                     FALSE,entry.th32ThreadID);
            if (!thread) continue;
            PWSTR name=NULL;
            GetThreadDescription(thread,&name);
            CONTEXT context={.ContextFlags=CONTEXT_FULL};
            /* Left suspended: a resumed thread could take a lock the next stack walk needs. So the
             * process ends with TerminateProcess below: ExitProcess runs DLL detach, which waits
             * for the loader lock a suspended thread may hold (a process that never exits). */
            if (SuspendThread(thread)!=(DWORD)-1 && GetThreadContext(thread,&context)) {
                fprintf(stderr,"thread %lu %ls\n",entry.th32ThreadID,name && *name ? name : L"");
                print_unwound_stack(&context);
            }
            if (name) LocalFree(name);
            CloseHandle(thread);
        } while (Thread32Next(snapshot,&entry));
    }
    if (snapshot!=INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    fflush(stderr);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(),0);
    return 0;
}
/* THEORDER_SAMPLE=<part of a thread name>[|<part>...]: THEORDER_SAMPLE_START seconds after start
 * (60) and for THEORDER_SAMPLE_SECS (30), every millisecond the matching threads are suspended and
 * their eight innermost frames counted; THEORDER_SAMPLE_OUT (out/sample.txt) then gets
 * "count;thread=<name>;frame;..." lines (tools/sample_report.py names them). Nothing is allocated
 * or printed while a thread is suspended: it may hold the heap lock. The sampler never samples
 * itself ("*" used to suspend its own thread and hang). Guest frames have no unwind tables: past a
 * guest frame the walk follows the eboot's frame-pointer chain (rbp -> [saved rbp, return]), read
 * with ReadProcessMemory so a stale rbp ends the walk instead of faulting; a guest leaf without
 * a frame of its own makes the walk skip its direct caller. */
#define SAMPLE_SLOTS 65536
#define SAMPLE_DEPTH 8
#define SAMPLE_THREADS 256
typedef struct { uintptr_t frames[SAMPLE_DEPTH]; unsigned thread, count; } SampleSlot;
static SampleSlot sample_slots[SAMPLE_SLOTS];
static int sample_add(unsigned thread, const uintptr_t frames[SAMPLE_DEPTH]) {
    uint64_t h=1469598103934665603ull^thread;
    for (int i=0;i<SAMPLE_DEPTH;++i) h=(h^frames[i])*1099511628211ull;
    for (unsigned probe=0;probe<SAMPLE_SLOTS;++probe) {
        SampleSlot *s=&sample_slots[(h+probe)&(SAMPLE_SLOTS-1)];
        if (!s->count) { memcpy(s->frames,frames,sizeof(s->frames)); s->thread=thread; s->count=1; return 1; }
        if (s->thread==thread && !memcmp(s->frames,frames,sizeof(s->frames))) { ++s->count; return 1; }
    }
    return 0; /* table full */
}
/* One frame-pointer step out of guest code: 0 when c->Rip is not guest code or the chain ends. */
static int sample_guest_frame(CONTEXT *c) {
    if (!image || c->Rip-(uintptr_t)image>=0x20000000 || !c->Rbp || c->Rbp&7 || c->Rbp<c->Rsp) return 0;
    DWORD64 link[2]; /* saved rbp, return address */
    SIZE_T got=0;
    if (!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)c->Rbp,link,sizeof(link),&got) ||
        got!=sizeof(link) || link[0]<=c->Rbp)
        return 0;
    c->Rsp=c->Rbp+16; c->Rip=link[1]; c->Rbp=link[0];
    return 1;
}
static DWORD WINAPI win_sampler_thread(LPVOID param) {
    const char *part=(const char *)param, *start=getenv("THEORDER_SAMPLE_START"),
               *secs=getenv("THEORDER_SAMPLE_SECS"), *out=getenv("THEORDER_SAMPLE_OUT");
    Sleep((start ? (DWORD)atoi(start) : 60)*1000);
    static wchar_t wpart[256], names[SAMPLE_THREADS][64];
    MultiByteToWideChar(CP_UTF8,0,part,-1,wpart,256);
    static HANDLE threads[SAMPLE_THREADS];
    int count=0;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    if (snapshot!=INVALID_HANDLE_VALUE && Thread32First(snapshot,&entry)) {
        do {
            if (entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId() ||
                count==SAMPLE_THREADS)
                continue;
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,
                                     FALSE,entry.th32ThreadID);
            if (!thread) continue;
            PWSTR name=NULL;
            int match=!strcmp(part,"*"); /* "*": every thread, named or not (a hang) */
            if (SUCCEEDED(GetThreadDescription(thread,&name)) && name && *name && !match) {
                for (const wchar_t *p=wpart; *p && !match;) { /* each '|'-separated part */
                    size_t n=wcscspn(p,L"|");
                    for (const wchar_t *s=name; *s && !match; ++s) match=n && !wcsncmp(s,p,n);
                    p+=n+(p[n]==L'|');
                }
            }
            if (match) { wcsncpy(names[count],name ? name : L"",63); names[count][63]=0; threads[count++]=thread; }
            else CloseHandle(thread);
            if (name) LocalFree(name);
        } while (Thread32Next(snapshot,&entry));
    }
    if (snapshot!=INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    unsigned long long samples=0, dropped=0;
    const ULONGLONG end=GetTickCount64()+(secs ? (ULONGLONG)atoi(secs) : 30)*1000;
    timeBeginPeriod(1);
    while (count && GetTickCount64()<end) {
        for (int i=0;i<count;++i) {
            if (SuspendThread(threads[i])==(DWORD)-1) continue;
            CONTEXT c={.ContextFlags=CONTEXT_FULL};
            uintptr_t frames[SAMPLE_DEPTH]={0};
            if (GetThreadContext(threads[i],&c)) {
                for (int depth=0; depth<SAMPLE_DEPTH && c.Rip; ++depth) { /* as print_unwound_stack */
                    frames[depth]=c.Rip;
                    DWORD64 base=0; PVOID handler_data; DWORD64 establisher;
                    PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(c.Rip,&base,NULL);
                    if (fn) RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,fn,&c,&handler_data,&establisher,NULL);
                    else if (sample_guest_frame(&c)) continue;
                    else if (depth==0) { c.Rip=*(DWORD64 *)c.Rsp; c.Rsp+=8; }
                    else break;
                }
            }
            ResumeThread(threads[i]);
            if (frames[0]) { ++samples; dropped+=!sample_add((unsigned)i,frames); }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    const char *path=out ? out : "out/sample.txt";
    FILE *f=fopen(path,"w");
    if (f) {
        char where[MAX_PATH+64];
        for (unsigned i=0;i<SAMPLE_SLOTS;++i) {
            if (!sample_slots[i].count) continue;
            fprintf(f,"%u;thread=%ls",sample_slots[i].count,names[sample_slots[i].thread]);
            for (int depth=0; depth<SAMPLE_DEPTH && sample_slots[i].frames[depth]; ++depth) {
                describe_address(where,sizeof(where),sample_slots[i].frames[depth]);
                fprintf(f,";%s",where);
            }
            fputc('\n',f);
        }
        fclose(f);
    }
    fprintf(stderr,"Sampler: %d threads matching \"%s\", %llu samples (%llu dropped) in %s\n",
            count,part,samples,dropped,path);
    return 0;
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
    if (gpu_enabled && sig == SIGSEGV && theordergpu_handle_fault(context, info->si_addr)) return;
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
    if (gpu_enabled) theordergpu_dump_guest_writes(context);
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
/* Whether the code at entry calls target (call rel32) within its first 64 bytes. */
static int entry_calls(const unsigned char *image, uint64_t entry, uint64_t target) {
    for (uint64_t at = entry; at < entry + 64; ++at) {
        int32_t rel;
        if (image[at] != 0xe8) continue;
        memcpy(&rel, image + at + 1, sizeof rel);
        if (at + 5 + (int64_t)rel == target) return 1;
    }
    return 0;
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
/* THEORDER_FOV_SITES (run.py, from patches.py): image offsets of the game's field of view
 * constants (doubles in read-only data). Their pages become writable for the in-game menu. */
static void set_fov_sites(Segment *segments, uint64_t ns) {
    const char *text = getenv("THEORDER_FOV_SITES");
    double *sites[16];
    int count = 0;
    for (char *end; text && *text && count < 16; text = end) {
        uint64_t offset = strtoull(text, &end, 0);
        if (end == text) break;
        for (uint64_t i = 0; i < ns; ++i) {
            if (!(offset % 8) && offset >= segments[i].address && offset + 8 <= segments[i].address + segments[i].size) {
                uint64_t page = offset & ~(uint64_t)(page_size - 1);
                protect(image + page, page_size, (unsigned)segments[i].flags | 2);
                sites[count++] = (double *)(image + offset);
                break;
            }
        }
    }
    if (count) theordergpu_set_fov_sites(sites, count);
    printf("FOV: %d sites\n", count);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    theordergpu_flush_cache();
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets THEORDER_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command = getenv("THEORDER_RESTART_COMMAND");
    if (!command || !*command) command = getenv("THEORDER_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: THEORDER_RESTART_COMMAND is not set\n", stderr); _exit(1); }
    char *line = _strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = GetStdHandle(STD_INPUT_HANDLE), .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
                            .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "runtime_restart: CreateProcess failed (%lu)\n", GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(), 0);
#else
    const char *command = getenv("THEORDER_RESTART_COMMAND");
    if (!command || !*command) command = getenv("THEORDER_RESTART_COMMAND");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    if (command && *command) {
        puts("Runtime: restarting through restart command");
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    } else {
        puts("Runtime: restarting through run.sh");
        execlp("bash", "bash", "run.sh", (char *)NULL);
    }
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

/* THEORDER_TASK_IDLE (src/task_idle.h): native idle callback of the job workers. Its stub is
 * the last bytes of the stub area; the diagnostic hooks below fill that area from its start. */
#define TASK_IDLE_STUB_SPACE 64
static void task_idle_sleep(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={(time_t)(ns/1000000000u),(long)(ns%1000000000u)};
    while (nanosleep(&t,&t) && errno==EINTR) {}
#endif
}
static void install_task_idle(unsigned char *stub, uint64_t image_size) {
    TaskIdleConfig idle=task_idle_config_from_env();
    if (!idle.mode) return;
    TaskIdleHooks hooks={runtime_thread_self,runtime_thread_yield,task_idle_sleep};
    const char *why=task_idle_install(image,image_size,&task_idle_eboot_0102,stub,TASK_IDLE_STUB_SPACE,&idle,&hooks);
    if (why) printf("Task idle: mode %d not installed: %s\n",idle.mode,why);
    else printf("Task idle: mode %d installed at 0x%" PRIx64 " (spin yields %u, sleep %" PRIu64 " us)\n",idle.mode,
                task_idle_eboot_0102.offset,idle.spin_yields,idle.sleep_ns/1000);
}
static unsigned char *g_diag_stubs;
static size_t g_diag_stub_offset;

static void hook_call_site(uint64_t call_site_offset, void *target_fn) {
    if (!g_diag_stubs) return;
    unsigned char *stub = g_diag_stubs + g_diag_stub_offset;
    g_diag_stub_offset += 32;
    stub[0] = 0x49; stub[1] = 0xbb;
    uintptr_t addr = (uintptr_t)target_fn;
    memcpy(stub + 2, &addr, 8);
    stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3;
    
    int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + call_site_offset + 5);
    int32_t disp = (int32_t)delta;
    image[call_site_offset] = 0xe8;
    memcpy(image + call_site_offset + 1, &disp, 4);
}

static ABI void log_failure_jump(uint64_t site, uint64_t rax_val, uint64_t rbx_val) {
    printf("--> FAILURE JUMP from guest offset 0x%" PRIx64 " with rax=0x%" PRIx64 " (%lld), rbx=0x%" PRIx64 "\n",
           site, rax_val, (long long)rax_val, rbx_val);
    fflush(stdout);
}

static void hook_cond_jump(uint64_t site_offset, uint64_t orig_target_offset) {
    if (!g_diag_stubs) return;
    unsigned char *stub = g_diag_stubs + g_diag_stub_offset;
    g_diag_stub_offset += 128;
    
    unsigned char code[] = {
        0x50, // push %rax (alignment)
        0x57, // push %rdi
        0x56, // push %rsi
        0x52, // push %rdx
        0x51, // push %rcx
        0x41, 0x50, // push %r8
        0x41, 0x51, // push %r9
        0x41, 0x52, // push %r10
        0x41, 0x53, // push %r11
        0x50, // push %rax
        0x48, 0xbf, 0,0,0,0,0,0,0,0, // movabs $site, %rdi
        0x48, 0x89, 0xc6, // mov %rax, %rsi
        0x48, 0x89, 0xda, // mov %rbx, %rdx
        0x48, 0xb8, 0,0,0,0,0,0,0,0, // movabs $log_failure_jump, %rax
        0xff, 0xd0, // call *%rax
        0x58, // pop %rax
        0x41, 0x5b, // pop %r11
        0x41, 0x5a, // pop %r10
        0x41, 0x59, // pop %r9
        0x41, 0x58, // pop %r8
        0x59, // pop %rcx
        0x5a, // pop %rdx
        0x5e, // pop %rsi
        0x5f, // pop %rdi
        0x58, // pop alignment %rax
        0x49, 0xbb, 0,0,0,0,0,0,0,0, // movabs $target, %r11
        0x41, 0xff, 0xe3 // jmp *%r11
    };
    uintptr_t target = (uintptr_t)(image + orig_target_offset);
    uintptr_t log_fn = (uintptr_t)log_failure_jump;
    memcpy(code + 16, &site_offset, 8);
    memcpy(code + 32, &log_fn, 8);
    memcpy(code + 58, &target, 8);
    memcpy(stub, code, sizeof(code));
    
    int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site_offset + 6);
    int32_t disp = (int32_t)delta;
    memcpy(image + site_offset + 2, &disp, 4);
}

static ABI int hook_fn40(void *rdi_obj) {
    printf("--> hook_fn40 called! rdi_obj=%p\n", rdi_obj);
    if (!rdi_obj) {
        printf("    rdi_obj is NULL!\n");
        return 1;
    }
    void **vtable = *(void***)rdi_obj;
    printf("    vtable=%p (guest offset 0x%" PRIxPTR ")\n", vtable, (uintptr_t)vtable - (uintptr_t)image);
    typedef ABI int (*Fn40)(void *obj, void *rsp58);
    Fn40 fn40 = (Fn40)vtable[0x40 / 8];
    printf("    fn40=%p (guest offset 0x%" PRIxPTR ")\n", fn40, (uintptr_t)fn40 - (uintptr_t)image);
    uint32_t val = 0;
    int res = fn40(rdi_obj, &val);
    printf("<-- fn40 returned %d, out_val=%u\n", res, val);
    return res;
}

static void hook_fn40_call(uint64_t site_offset, void *target_fn) {
    if (!g_diag_stubs) return;
    unsigned char *stub = g_diag_stubs + g_diag_stub_offset;
    g_diag_stub_offset += 64;
    stub[0] = 0x49; stub[1] = 0xbb; // movabs $target_fn, %r11
    uintptr_t addr = (uintptr_t)target_fn;
    memcpy(stub + 2, &addr, 8);
    stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3; // jmpq *%r11
    
    unsigned char patch[18] = {
        0xe8, 0,0,0,0, // call stub
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 13 NOPs
    };
    int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site_offset + 5);
    int32_t disp = (int32_t)delta;
    memcpy(patch + 1, &disp, 4);
    memcpy(image + site_offset, patch, 18);
}

static ABI int hook_sound_init_step(void *r12_reg) {
    void *sound_obj = *(void**)((char*)r12_reg + 0x90);
    printf("--> hook_sound_init_step: r12=%p, sound_obj=%p\n", r12_reg, sound_obj);
    if (!sound_obj) {
        printf("    sound_obj is NULL!\n");
        return 1;
    }
    void **vtable = *(void***)sound_obj;
    printf("    vtable=%p (guest offset 0x%" PRIxPTR ")\n", vtable, (uintptr_t)vtable - (uintptr_t)image);
    typedef ABI int (*Method48)(void *rdi);
    Method48 m48 = (Method48)vtable[0x48 / 8];
    printf("    m48=%p (guest offset 0x%" PRIxPTR ")\n", m48, (uintptr_t)m48 - (uintptr_t)image);
    int res = m48(sound_obj);
    printf("<-- m48 returned %d\n", res);
    return res;
}

static void hook_sound_call(uint64_t site_offset, void *target_fn) {
    if (!g_diag_stubs) return;
    unsigned char *stub = g_diag_stubs + g_diag_stub_offset;
    g_diag_stub_offset += 64;
    stub[0] = 0x49; stub[1] = 0xbb; // movabs $target_fn, %r11
    uintptr_t addr = (uintptr_t)target_fn;
    memcpy(stub + 2, &addr, 8);
    stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3; // jmpq *%r11
    
    unsigned char patch[14] = {
        0x4c, 0x89, 0xe7, // movq %r12, %rdi
        0xe8, 0,0,0,0,    // call stub
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 6 NOPs
    };
    int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site_offset + 3 + 5);
    int32_t disp = (int32_t)delta;
    memcpy(patch + 4, &disp, 4);
    memcpy(image + site_offset, patch, 14);
}

static ABI int trace_assert(const char *msg, int line, const char *file, const char *extra, ...) {
    printf("GAME ASSERTION FAILED: %s:%d: \"%s\" (extra: %s)\n",
           file ? file : "null", line, msg ? msg : "null", extra ? extra : "null");
    return 1;
}

static ABI int trace_a82860(void *rdi, void *rsi) {
    printf("--> Entering 0xa82860(rdi=%p, rsi=%p)\n", rdi, rsi);
    typedef int (ABI *OrigFn)(void *, void *);
    OrigFn orig = (OrigFn)(image + 0xa82860);
    int ret = orig(rdi, rsi);
    printf("<-- Exited 0xa82860 = %d, [16b3418]=%p, [16b1df0]=%p, [16b3420]=%p\n",
           ret, *(void**)(image + 0x16b3418), *(void**)(image + 0x16b1df0), *(void**)(image + 0x16b3420));
    return ret;
}

static ABI int trace_8a10a0(void *rdi, void *rsi) {
    uint32_t dmem = *(uint32_t*)(image + 0x1682dd0);
    printf("--> Entering 0x8a10a0(rdi=%p, rsi=%p), [1682dd0]=0x%x\n", rdi, rsi, dmem);
    typedef int (ABI *OrigFn)(void *, void *);
    OrigFn orig = (OrigFn)(image + 0x8a10a0);
    int ret = orig(rdi, rsi);
    printf("<-- Exited 0x8a10a0 = %d, [1682dd0]=0x%x\n", ret, *(uint32_t*)(image + 0x1682dd0));
    return ret;
}

static ABI int trace_8aae90(void *rdi) {
    printf("--> Entering 0x8aae90(rdi=%p)\n", rdi);
    typedef int (ABI *OrigFn)(void *);
    OrigFn orig = (OrigFn)(image + 0x8aae90);
    int ret = orig(rdi);
    printf("<-- Exited 0x8aae90 = 0x%x\n", ret);
    return ret;
}

static ABI int trace_8a5e60(void *rdi, uint32_t esi, void *rdx, uint32_t ecx) {
    printf("--> Entering 0x8a5e60(rdi=%p, esi=0x%x, rdx=%p, ecx=%u)\n", rdi, esi, rdx, ecx);
    typedef int (ABI *OrigFn)(void *, uint32_t, void *, uint32_t);
    OrigFn orig = (OrigFn)(image + 0x8a5e60);
    int ret = orig(rdi, esi, rdx, ecx);
    printf("<-- Exited 0x8a5e60 = %d\n", ret);
    return ret;
}

static ABI int trace_8f7470(void *rdi, void *rsi, void *rdx) {
    printf("--> Entering 0x8f7470(rdi=%p, rsi=%p, rdx=%p)\n", rdi, rsi, rdx);
    typedef int (ABI *OrigFn)(void *, void *, void *);
    OrigFn orig = (OrigFn)(image + 0x8f7470);
    int ret = orig(rdi, rsi, rdx);
    printf("<-- Exited 0x8f7470 = %d\n", ret);
    return ret;
}

#define DEF_STEP(id, site, target) \
    static ABI int trace_step_##id(void *a, void *b, void *c, void *d) { \
        typedef int (ABI *F)(void *, void *, void *, void *); \
        int res = ((F)(image + (target)))(a, b, c, d); \
        printf("  [AK::SoundEngine::Init step " #id "] returned %d\n", res); \
        return res; \
    }

DEF_STEP(698f5, 0x698f5, 0x963f0)
DEF_STEP(69986, 0x69986, 0x7e200)
DEF_STEP(699d7, 0x699d7, 0x72a30)
DEF_STEP(69a9a, 0x69a9a, 0x90a10)
DEF_STEP(69b44, 0x69b44, 0x91b90)
DEF_STEP(69b95, 0x69b95, 0x95120)
DEF_STEP(69bf0, 0x69bf0, 0xa0aa0)
DEF_STEP(69cae, 0x69cae, 0x8b8c0)
DEF_STEP(69cf9, 0x69cf9, 0x9cdf0)
DEF_STEP(69d83, 0x69d83, 0xa1310)
DEF_STEP(69dcc, 0x69dcc, 0x6ca10)
static ABI int trace_step_69de2(void *a, void *b, void *c, void *d) {
    printf("--> trace_step_69de2(a=%p):\n", a);
    if (a) {
        printf("    *(void**)(a+0xa8) = %p\n", *(void**)((char*)a + 0xa8));
        printf("    *(void**)(a+0x48) = %p\n", *(void**)((char*)a + 0x48));
        printf("    *(void**)(a+0x78) = %p\n", *(void**)((char*)a + 0x78));
    }
    typedef int (ABI *F)(void *, void *, void *, void *);
    int res = ((F)(image + 0x71710))(a, b, c, d);
    printf("  [AK::SoundEngine::Init step 69de2 -> 0x71710] returned %d\n", res);
    return res;
}

static ABI int trace_sound_engine_init(void *rdi, void *rsi) {
    printf("--> Entering AK::SoundEngine::Init(rdi=%p, rsi=%p)\n", rdi, rsi);
    typedef int (ABI *OrigFn)(void *, void *);
    OrigFn orig = (OrigFn)(image + 0x696c0);
    int res = orig(rdi, rsi);
    printf("<-- Exited AK::SoundEngine::Init = %d\n", res);
    return res;
}

/* The hooks below are bring-up traces of the 01.02 eboot's sound engine start, at that build's
 * offsets: in another build those bytes are other code (a 01.00 eboot crashed at 0x9b47f9, next
 * to the 0x9b47fc call site, issue #3). They go in only where every site holds the instruction
 * they replace: each call calling the function its trace calls, each branch reaching its target,
 * and the two sequences the fn40/sound hooks rewrite. */
static int order1886_hook_sites_match(void) {
    static const uint64_t calls[][2] = {
        {0x44d0a4,0xa82860}, {0xa82ab7,0x8a10a0}, {0xa82acf,0x8aae90}, {0xa82aed,0x8a5e60},
        {0x89dda4,0x8f7470}, {0x8a41b8,0x203650}, {0x9b47fc,0x696c0}, {0x698f5,0x963f0},
        {0x69986,0x7e200}, {0x699d7,0x72a30}, {0x69a9a,0x90a10}, {0x69b44,0x91b90}, {0x69b95,0x95120},
        {0x69bf0,0xa0aa0}, {0x69cae,0x8b8c0}, {0x69cf9,0x9cdf0}, {0x69d83,0xa1310}, {0x69dcc,0x6ca10},
        {0x69de2,0x71710}};
    static const uint64_t branches[][2] = {
        {0x8a37f7,0x8a41bf}, {0x8a380f,0x8a41bf}, {0x8a3843,0x8a41bf}, {0x8a3ac4,0x8a41bf},
        {0x8a3de4,0x8a41bf}, {0x8a3e2f,0x8a41bf}, {0x8a3e9f,0x8a41bf}, {0x8a3fed,0x8a41bf},
        {0x8a4004,0x8a41bf}, {0x9b476e,0x9b493f}, {0x9b4804,0x9b491c}, {0x9b4824,0x9b492c},
        {0x9b4838,0x9b493f}};
    static const unsigned char fn40[18]={0x48,0x8b,0x07,0x48,0x8b,0x40,0x40,0x89,0x5c,0x24,0x58,0x48,0x8d,0x74,0x24,0x58,0xff,0xd0};
    static const unsigned char sound[14]={0x49,0x8b,0xbc,0x24,0x90,0x00,0x00,0x00,0x48,0x8b,0x07,0xff,0x50,0x48};
    int32_t rel;
    for (size_t i=0; i<sizeof calls/sizeof calls[0]; ++i) {
        memcpy(&rel, image+calls[i][0]+1, 4);
        if (image[calls[i][0]]!=0xe8 || (int64_t)calls[i][0]+5+rel!=(int64_t)calls[i][1]) return 0;
    }
    for (size_t i=0; i<sizeof branches/sizeof branches[0]; ++i) {
        memcpy(&rel, image+branches[i][0]+2, 4);
        if (image[branches[i][0]]!=0x0f || (image[branches[i][0]+1]&0xf0)!=0x80 ||
            (int64_t)branches[i][0]+6+rel!=(int64_t)branches[i][1]) return 0;
    }
    return !memcmp(image+0x8a37e1, fn40, sizeof fn40) && !memcmp(image+0x8a37fd, sound, sizeof sound);
}

static void install_order1886_diagnostics(void) {
    if (!order1886_hook_sites_match()) {
        puts("Diagnostics: this eboot.bin is not the 01.02 build the sound engine traces are written for: not installed");
        return;
    }
    hook_call_site(0x44d0a4, (void*)trace_a82860);
    hook_call_site(0xa82ab7, (void*)trace_8a10a0);
    hook_call_site(0xa82acf, (void*)trace_8aae90);
    hook_call_site(0xa82aed, (void*)trace_8a5e60);
    hook_call_site(0x89dda4, (void*)trace_8f7470);
    hook_call_site(0x8a41b8, (void*)trace_assert);
    
    hook_fn40_call(0x8a37e1, (void*)hook_fn40);
    hook_sound_call(0x8a37fd, (void*)hook_sound_init_step);
    
    hook_call_site(0x9b47fc, (void*)trace_sound_engine_init);
    
    hook_call_site(0x698f5, (void*)trace_step_698f5);
    hook_call_site(0x69986, (void*)trace_step_69986);
    hook_call_site(0x699d7, (void*)trace_step_699d7);
    hook_call_site(0x69a9a, (void*)trace_step_69a9a);
    hook_call_site(0x69b44, (void*)trace_step_69b44);
    hook_call_site(0x69b95, (void*)trace_step_69b95);
    hook_call_site(0x69bf0, (void*)trace_step_69bf0);
    hook_call_site(0x69cae, (void*)trace_step_69cae);
    hook_call_site(0x69cf9, (void*)trace_step_69cf9);
    hook_call_site(0x69d83, (void*)trace_step_69d83);
    hook_call_site(0x69dcc, (void*)trace_step_69dcc);
    hook_call_site(0x69de2, (void*)trace_step_69de2);
    
    hook_cond_jump(0x8a37f7, 0x8a41bf);
    hook_cond_jump(0x8a380f, 0x8a41bf);
    hook_cond_jump(0x8a3843, 0x8a41bf);
    hook_cond_jump(0x8a3ac4, 0x8a41bf);
    hook_cond_jump(0x8a3de4, 0x8a41bf);
    hook_cond_jump(0x8a3e2f, 0x8a41bf);
    hook_cond_jump(0x8a3e9f, 0x8a41bf);
    hook_cond_jump(0x8a3fed, 0x8a41bf);
    hook_cond_jump(0x8a4004, 0x8a41bf);
    
    hook_cond_jump(0x9b476e, 0x9b493f);
    hook_cond_jump(0x9b4804, 0x9b491c);
    hook_cond_jump(0x9b4824, 0x9b492c);
    hook_cond_jump(0x9b4838, 0x9b493f);
}

/* The guest code needs the PS4's instruction sets (#33, #66); THEORDER_CPU_CHECK=0 skips this. A
 * missing fatal set prints STOP and returns 0; the rewritable ones go to cpu_missing_sets, named
 * in missing. */
static int cpu_supported(char *missing, size_t size) {
    missing[0] = 0;
    const char *check = getenv("THEORDER_CPU_CHECK");
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

#ifdef _WIN32
/* When free physical memory runs out (GPU allocations past the VRAM budget spill to system
 * memory on a 16 GB machine) Windows stops responding and needs a hard reset. Quit first, with
 * the reason in the log. THEORDER_RAM_GUARD_MB=N: limit in MiB (default 700, 0: off).
 * From TOport (IgorAugustoHH/The-Order-1886-PC-Port, src/probe.c, BB_RAM_GUARD_MB). */
static DWORD WINAPI ram_guard(LPVOID arg) {
    const unsigned long long limit = (unsigned long long)(uintptr_t)arg << 20;
    unsigned low = 0, ticks = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
        if (GlobalMemoryStatusEx(&status)) {
            if (++ticks % 120 == 0)
                printf("Memory: system RAM %llu MiB free of %llu, commit %llu MiB free\n",
                       status.ullAvailPhys >> 20, status.ullTotalPhys >> 20,
                       status.ullAvailPageFile >> 20);
            low = status.ullAvailPhys < limit ? low + 1 : 0;
            if (low >= 4) {
                fprintf(stderr, "STOP: system RAM almost exhausted (%llu MiB free, guard %llu MiB);"
                        " quitting before Windows stops responding (THEORDER_RAM_GUARD_MB=0: off)\n",
                        status.ullAvailPhys >> 20, limit >> 20);
                fflush(stderr); fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
        Sleep(250);
    }
    return 0;
}

static void start_ram_guard(void) {
    const char *guard = getenv("THEORDER_RAM_GUARD_MB");
    const unsigned long guard_mb = guard ? strtoul(guard, NULL, 10) : 700;
    if (!guard_mb) return;
    HANDLE thread = CreateThread(NULL, 64 * 1024, ram_guard, (LPVOID)(uintptr_t)guard_mb, 0, NULL);
    if (thread) {
        SetThreadDescription(thread, L"theorder:RamGuard");
        CloseHandle(thread);
    }
}
#endif

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
    timeBeginPeriod(1);
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *isa_file=NULL;
    const char *mounts[8]; int mount_count=0;
    static const char *guest_args[64];
    int guest_arg_count = 0;
    guest_args[guest_arg_count++] = "/app0/eboot.bin";
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
        else if (!strcmp(argv[i], "--guest-arg") && i+1<argc) {
            if (guest_arg_count < 63) guest_args[guest_arg_count++] = argv[++i];
        }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    const char *env_guest_args = getenv("THEORDER_ARGS");
    if (!env_guest_args) env_guest_args = getenv("THEORDER_GUEST_ARGS");
    if (env_guest_args && *env_guest_args) {
        char *dup = strdup(env_guest_args);
        char *tok = strtok(dup, " ");
        while (tok && guest_arg_count < 63) {
            guest_args[guest_arg_count++] = tok;
            tok = strtok(NULL, " ");
        }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--isa-rewrite file] [--mount guest=host]... [--timeout seconds] | --vulkan-only | --cpu-features | --gamepad\n", argv[0]);
        return 1;
    }
    if (!cpu_supported(missing, sizeof missing)) return 20;
    if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
    /* THEORDER_ISA_REWRITE=0 never rewrites, =1 rewrites every site, a list of sets
     * (MOVBE+BMI1+LZCNT = Ivy Bridge; ',' also separates) rewrites them as well as those the CPU lacks: an older CPU's
     * path on this one. By default the sets the CPU lacks. */
    const char *policy = getenv("THEORDER_ISA_REWRITE");
    unsigned isa_sets = policy && !strcmp(policy, "1") ? 0xffffffffu : policy && !strcmp(policy, "0") ? 0 : cpu_missing_sets;
    if (policy && *policy && strcmp(policy, "1") && strcmp(policy, "0")) {
        unsigned forced = 0;
        char forced_names[64];
        if (!cpu_parse_rewrite_sets(policy, &forced, forced_names, sizeof forced_names)) {
            printf("STOP: THEORDER_ISA_REWRITE=%s: expected 0, 1 or a list of MOVBE, F16C, BMI1, LZCNT\n", policy);
            return 20;
        }
        printf("ISA: rewrite of %s forced (THEORDER_ISA_REWRITE)\n", forced_names);
        if (!cpu_missing_sets) snprintf(missing, sizeof missing, "%s", forced_names);
        isa_sets = forced | cpu_missing_sets;
    } else if (isa_sets == 0xffffffffu && !cpu_missing_sets) {
        snprintf(missing, sizeof missing, "MOVBE, F16C, BMI1, LZCNT (forced)");
    }
    if (cpu_missing_sets && !isa_sets) {
        printf("STOP: this CPU lacks %s and the rewrite is disabled (ISA_REWRITE=0)\n", missing);
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
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL)) {
            runtime_savedata_configure(id);
        }
    }
    theordergpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    start_ram_guard();
    const char *strict_handles = getenv("THEORDER_STRICT_HANDLES");
    if (strict_handles) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    if (timeout_seconds > 0) {
        HANDLE th = CreateThread(NULL, 0, win_watchdog_thread, (LPVOID)(uintptr_t)timeout_seconds, 0, NULL);
        if (th) CloseHandle(th);
    }
    const char *sample = getenv("THEORDER_SAMPLE");
    if (sample && *sample) {
        HANDLE th = CreateThread(NULL, 0, win_sampler_thread, (LPVOID)sample, 0, NULL);
        if (th) CloseHandle(th);
    }
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
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"G3PROBE5",8);
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
    /* isa_sets: what the CPU lacks, or what THEORDER_ISA_REWRITE forces (a test that must not run unrewritten). */
    if (!isa_table && isa_sets) { printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing); return 20; }
    /* The stubs follow the image in the same low mapping (rel32 jmps reach them). */
    size_t isa_stub_size = isa_table ? round_page(isa_stub_area_size(isa_table)) : 0;
    size_t stub_area = isa_stub_size + 65536;
    image = allocate(round_page(size) + stub_area);
    unsigned char *stubs = image + round_page(size);
    g_diag_stubs = stubs + isa_stub_size;
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    if (!cpu_only) {
        char title[128]="The Order: 1886", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        TheOrderGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (theordergpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,theordergpu_symbol_count());
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
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    install_order1886_diagnostics();
    if (isa_table) {
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            printf("STOP: this CPU lacks %s and %u sites use encodings the rewrite cannot emulate\n", missing, r.unsupported_needed);
            fflush(NULL);
            _exit(20); /* no destructors: GPU threads are running */
        }
        if (isa_sets == 0xffffffffu) printf("ISA: forced rewrite of all %u sites (THEORDER_ISA_REWRITE=1), %u skipped\n", r.rewritten, r.skipped);
        else printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n",
                    missing, r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        runtime_set_report_hook(isa_report_hook);
    }
    install_task_idle(stubs + stub_area - TASK_IDLE_STUB_SPACE, size);
    protect(stubs, stub_area, 5);
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    if (gpu_enabled) set_fov_sites(segments, ns);
    printf("Mapped %" PRIu64 " bytes at %p, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, (void *)image, ns, nr);
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
        runtime_orbis_fpu(); /* the PS4's floating-point state from the first guest code on */
#if defined(__x86_64__)
        {
            uint32_t mxcsr=0;
            __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
            printf("Runtime: guest FPU state MXCSR %#x (PS4: 0x9fc0, flush-to-zero and denormals-are-zero)\n",mxcsr);
        }
#endif
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    /* In ELF, DT_INIT runs before the main entry. The Order's entry (_start) calls it itself
     * after _init_env and atexit, as on the PS4: running it here too constructed every global
     * twice, mapping the engine's memory pools (1.6 GiB of direct memory) a second time. */
    uint64_t main_init = 0x20;
    if (entry_calls(image, entry, main_init)) {
        printf("Main executable initializer at offset 0x%" PRIx64 ": the entry runs it\n", main_init);
    } else if (mapped(segments, ns, main_init, 1)) {
        printf("Running main executable initializer at offset 0x%" PRIx64 "\n", main_init);
        typedef void (ABI *MainInit)(void);
        ((MainInit)(image + main_init))();
        printf("Main executable initializer finished\n");
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    guest_args[guest_arg_count] = NULL;
    runtime_set_guest_args(guest_arg_count, guest_args);
    struct { uint64_t argc; const char *argv[64]; } params;
    params.argc = (uint64_t)guest_arg_count;
    for (int a = 0; a <= guest_arg_count; ++a) params.argv[a] = guest_args[a];
    if (guest_arg_count > 1) {
        printf("Forwarding %d arguments to guest:\n", guest_arg_count);
        for (int a = 0; a < guest_arg_count; ++a) printf("  argv[%d] = %s\n", a, guest_args[a]);
    }
    entered_game = 1;
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
