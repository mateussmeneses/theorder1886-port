/* isa_rewrite (src/isa_rewrite.c): the generated stubs run real patched code in an RWX region. */
#define _GNU_SOURCE   /* MAP_ANONYMOUS and REG_RIP under -std=c11 */
#include <assert.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif
#include "isa_rewrite.h"

typedef uint64_t (ABI *Snippet)(uint64_t in, uint64_t *out);
enum { PAGE = 4096, S1 = 0x000, S2 = 0x040, S3 = 0x080, S4 = 0x0c0, S5 = 0x100 };

/* S1: mov qword [rsp-8], 0xdeadbeef ; mov ecx, edi ; blsr ecx, ecx ; mov rdx, [rsp-8] ; mov [rsi+8], rdx ;
 *     pushfq ; pop qword [rsi] ; mov eax, ecx ; ret     -> site blsr at +0x0b (5 bytes, JMP)
 *     (the red-zone slot is read before pushfq overwrites it; mov never touches flags) */
static const uint8_t S1_CODE[] = {0x48, 0xc7, 0x44, 0x24, 0xf8, 0xef, 0xbe, 0xad, 0xde, 0x89, 0xf9,
                                  0xc4, 0xe2, 0x70, 0xf3, 0xc9, 0x48, 0x8b, 0x54, 0x24, 0xf8, 0x48,
                                  0x89, 0x56, 0x08, 0x9c, 0x8f, 0x06, 0x89, 0xc8, 0xc3};
/* S2: mov ecx, esi ; movbe [rdi], ecx ; mov eax, ecx ; ret  -> site movbe at +2 (4 bytes) + relocated mov eax, ecx */
static const uint8_t S2_CODE[] = {0x89, 0xf1, 0x0f, 0x38, 0xf1, 0x0f, 0x89, 0xc8, 0xc3};
/* S3: mov ecx, edi ; blsr ecx, ecx ; blsr ecx, ecx ; mov eax, ecx ; ret  -> two adjacent JMP sites at +2, +7 */
static const uint8_t S3_CODE[] = {0x89, 0xf9, 0xc4, 0xe2, 0x70, 0xf3, 0xc9, 0xc4, 0xe2, 0x70, 0xf3, 0xc9, 0x89, 0xc8, 0xc3};
/* S4: lzcnt eax, edi ; ret  -> 4-byte site, mode INT3 (only patched here) */
static const uint8_t S4_CODE[] = {0xf3, 0x0f, 0xbd, 0xc7, 0xc3};
/* S5: lzcnt eax, edi ; 15 x nop ; ret  -> JMP_RELOCATE site with the longest relocated instruction */
static const uint8_t S5_CODE[] = {0xf3, 0x0f, 0xbd, 0xc7, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xc3};
/* andn eax, ecx, edx */
static const uint8_t ANDN_CODE[] = {0xc4, 0xe2, 0x70, 0xf2, 0xc2};
enum { ANDN_AT = 0x0e0 };
/* S6: vcvtph2ps xmm1, xmm1 ; ret  -> F16C site, int3, emulated on the trapped CONTEXT (Windows) */
static const uint8_t S6_CODE[] = {0xc4, 0xe2, 0x79, 0x13, 0xc9, 0xc3};
enum { S6 = 0x200 };
/* S7: movbe eax, [rdi+0] ; ret   S8: movbe rax, [rdi+8] ; ret   S9: movbe r9d, [rdi] ; mov eax, r9d ; ret
 * -> MOVBE loads emitted inline (mov + bswap) instead of the emulator call */
static const uint8_t S7_CODE[] = {0x0f, 0x38, 0xf0, 0x47, 0x00, 0xc3};
static const uint8_t S8_CODE[] = {0x48, 0x0f, 0x38, 0xf0, 0x47, 0x08, 0xc3};
static const uint8_t S9_CODE[] = {0x44, 0x0f, 0x38, 0xf0, 0x0f, 0x44, 0x89, 0xc8, 0xc3};
enum { S7 = 0x240, S8 = 0x260, S9 = 0x280 };
/* BEXTR inline: S10 bextr eax,edi,esi ; ret
 * S11 mov edx,edi ; mov eax,esi ; bextr ecx,edx,eax ; mov eax,ecx ; ret   (every operand a saved scratch)
 * S12 bextr rax,rdi,rsi ; ret
 * S13 mov r10d,edi ; mov r11d,esi ; bextr r9d,r10d,r11d ; mov eax,r9d ; ret */
static const uint8_t S10_CODE[] = {0xc4, 0xe2, 0x48, 0xf7, 0xc7, 0xc3};
static const uint8_t S11_CODE[] = {0x89, 0xfa, 0x89, 0xf0, 0xc4, 0xe2, 0x78, 0xf7, 0xca, 0x89, 0xc8, 0xc3};
static const uint8_t S12_CODE[] = {0xc4, 0xe2, 0xc8, 0xf7, 0xc7, 0xc3};
static const uint8_t S13_CODE[] = {0x41, 0x89, 0xfa, 0x41, 0x89, 0xf3, 0xc4, 0x42, 0x20, 0xf7, 0xca, 0x44, 0x89, 0xc8, 0xc3};
enum { S10 = 0x2a0, S11 = 0x2c0, S12 = 0x2e0, S13 = 0x300 };

static unsigned char *region, *image, *stubs;
static unsigned char pristine[PAGE];

static IsaSite site_at(uint64_t offset, uint8_t length, uint8_t kind, uint8_t mode, uint8_t dst, uint8_t src1) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.offset = offset;
    s.length = length;
    s.kind = kind;
    s.width = 32;
    s.mode = mode;
    s.dst = dst;
    s.src1 = src1;
    s.src2 = ISA_NO_REG;
    s.mem_slot = ISA_MEM_NONE;
    s.base = ISA_NO_REG;
    s.index = ISA_NO_REG;
    memcpy(s.original, pristine + offset, length);
    return s;
}

static IsaTable *table_of(const IsaSite *sites, uint32_t count) {
    IsaTable *t = calloc(1, sizeof *t);
    t->sites = calloc(count, sizeof *sites);
    t->hits = calloc(count, sizeof *t->hits);
    memcpy(t->sites, sites, count * sizeof *sites);
    t->count = count;
    t->image_size = PAGE;
    return t;
}

/* S1-S4 sites, ascending. */
static IsaTable *snippet_table(void) {
    IsaSite sites[5];
    sites[0] = site_at(S1 + 0x0b, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[1] = site_at(S2 + 2, 4, ISA_MOVBE_STORE, ISA_MODE_JMP_RELOCATE, ISA_NO_REG, 1);
    sites[1].mem_slot = ISA_MEM_DST;
    sites[1].base = 7;
    sites[1].scale = 1;
    sites[1].relocated_length = 2;
    memcpy(sites[1].relocated, pristine + S2 + 6, 2);
    sites[2] = site_at(S3 + 2, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[3] = site_at(S3 + 7, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[4] = site_at(S4, 4, ISA_LZCNT, ISA_MODE_INT3, 0, 7);
    return table_of(sites, 5);
}

static void reset_region(void) {
    memcpy(image, pristine, PAGE);
    memset(stubs, 0, 2 * PAGE);
}

static int32_t rel32_at(const unsigned char *at) {
    int32_t v;
    memcpy(&v, at, 4);
    return v;
}

static uint64_t native_blsr_flags(uint32_t in) {
    uint64_t flags;
    __asm__("blsrl %[v], %[v]\n\tleaq -128(%%rsp), %%rsp\n\tpushfq\n\tpopq %[f]\n\tleaq 128(%%rsp), %%rsp"
            : [v] "+r"(in), [f] "=r"(flags) : : "cc");
    return flags;
}

static void test_jmp_site(void) {
    reset_region();
    IsaTable *t = snippet_table();
    IsaReport r = isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(r.rewritten == 5 && r.jmp == 3 && r.relocated == 1 && r.int3 == 1 && r.skipped == 0);
    assert(t->image == image && t->stubs == stubs);
    unsigned char *site = image + S1 + 0x0b;
    assert(site[0] == 0xe9 && site + 5 + rel32_at(site + 1) == stubs);
    static const uint8_t prologue[] = {0x48, 0x8d, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff};
    assert(memcmp(stubs, prologue, sizeof prologue) == 0);
    Snippet s1 = (Snippet)(void *)(image + S1);
    static const uint32_t inputs[] = {0, 1, 0xf0, 0x80000000};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        uint64_t out[2] = {0, 0};
        uint64_t result = s1(inputs[i], out);
        assert(result == _blsr_u32(inputs[i]));
        assert(((out[0] ^ native_blsr_flags(inputs[i])) & isa_defined_flags(ISA_BLSR)) == 0);
        assert(out[1] == 0xffffffffdeadbeefull);
    }
    assert(s1(0xf0, (uint64_t[2]){0, 0}) == 0xe0);
    assert(t->hits[0] == 5);
}

static void test_relocated_site(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    unsigned char *site = image + S2 + 2;
    assert(site[0] == 0xe9 && site + 5 + rel32_at(site + 1) == stubs + 1 * ISA_STUB_BYTES && site[5] == 0x90);
    uint32_t buffer = 0;
    Snippet s2 = (Snippet)(void *)(image + S2);
    uint64_t result = s2((uint64_t)(uintptr_t)&buffer, (uint64_t *)(uintptr_t)0x11223344);
    assert(buffer == 0x44332211 && result == 0x11223344);
}

static void test_adjacent_sites(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    Snippet s3 = (Snippet)(void *)(image + S3);
    assert(s3(0xf0, NULL) == _blsr_u32(_blsr_u32(0xf0)));
    assert(s3(0xf0, NULL) == 0xc0);
}

#define XMM_LOAD(n) "movdqu " #n "*16(%%rbx), %%xmm" #n "\n\t"
#define XMM_STORE(n) "movdqu %%xmm" #n ", " #n "*16(%%rbx)\n\t"
#define XMM_ALL(op) op(0) op(1) op(2) op(3) op(4) op(5) op(6) op(7) op(8) op(9) op(10) op(11) op(12) op(13) op(14) op(15)

static void test_xmm_preserved(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    unsigned char pattern[256], xmm[256];
    for (int i = 0; i < 256; i++) pattern[i] = (unsigned char)(i * 37 + 11);
    memcpy(xmm, pattern, sizeof xmm);
    uint64_t out[2] = {0, 0};
    uint64_t in = 0xf0, *out_ptr = out;
    void *fn = image + S1;
    /* The call steps over this function's red zone; rbx is preserved by the snippet. */
    __asm__ volatile(XMM_ALL(XMM_LOAD) "leaq -128(%%rsp), %%rsp\n\tcall *%%rax\n\tleaq 128(%%rsp), %%rsp\n\t"
                     XMM_ALL(XMM_STORE)
                     : "+a"(fn), "+D"(in), "+S"(out_ptr)
                     : "b"(xmm)
                     : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3",
                       "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                       "xmm14", "xmm15");
    assert((uint64_t)(uintptr_t)fn == 0xe0);
    assert(memcmp(xmm, pattern, sizeof xmm) == 0);
}

static void test_sets_filter(void) {
    reset_region();
    IsaTable *t = snippet_table();
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_LZCNT);
    assert(r.rewritten == 1 && r.int3 == 1);
    assert(image[S4] == 0xcc && memcmp(image + S4 + 1, pristine + S4 + 1, 3) == 0);
    assert(memcmp(image, pristine, S4) == 0 && memcmp(image + S4 + 1, pristine + S4 + 1, PAGE - S4 - 1) == 0);
}

static void test_skipped_when_bytes_differ(void) {
    reset_region();
    image[S1 + 0x0b + 4] = 0xc8;
    IsaTable *t = snippet_table();
    IsaReport r = isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(r.skipped == 1 && r.rewritten == 4);
    static const uint8_t expected[] = {0xc4, 0xe2, 0x70, 0xf3, 0xc8};
    assert(memcmp(image + S1 + 0x0b, expected, 5) == 0);
}

static void test_table_mismatch(void) {
    static const uint8_t abc_sha256[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                           0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                           0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    IsaTable t;
    memset(&t, 0, sizeof t);
    t.image_size = 3;
    memcpy(t.sha256, abc_sha256, 32);
    assert(isa_table_matches(&t, (const unsigned char *)"abc", 3) == 0);
    assert(isa_table_matches(&t, (const unsigned char *)"abc", 2) != 0);
    assert(isa_table_matches(&t, (const unsigned char *)"abd", 3) != 0);
}

static void test_unsupported_blocks_apply(void) {
    reset_region();
    IsaSite sites[2];
    sites[0] = site_at(S1 + 0x0b, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[1] = site_at(ANDN_AT, 5, ISA_ANDN, ISA_MODE_UNSUPPORTED, 0, 1);
    IsaTable *t = table_of(sites, 2);
    IsaReport r = isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(r.unsupported_needed == 1 && r.rewritten == 0);
    assert(memcmp(image, pristine, PAGE) == 0);
    for (int i = 0; i < 2 * PAGE; i++) assert(stubs[i] == 0);
}

static void test_largest_stub_fits(void) {
    reset_region();
    IsaSite site = site_at(S5, 4, ISA_LZCNT, ISA_MODE_JMP_RELOCATE, 0, 7);
    site.relocated_length = 15;
    memcpy(site.relocated, pristine + S5 + 4, 15);
    IsaTable *t = table_of(&site, 1);
    IsaReport r = isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(r.rewritten == 1 && r.relocated == 1 && r.skipped == 0);
    assert(stubs[0] == 0x48 && stubs[ISA_STUB_BYTES - 1] == 0);
    for (int i = 5; i < 19; i++) assert(image[S5 + i] == 0x90);
    Snippet s5 = (Snippet)(void *)(image + S5);
    assert(s5(0x00f00000, NULL) == _lzcnt_u32(0x00f00000));
    assert(s5(0, NULL) == 32);
}

static void test_table_load_fixture(const char *path) {
    IsaTable *t = isa_table_load(path);
    assert(t != NULL);
    assert(t->count == 6 && t->unsupported == 0);
    assert(t->sites[0].kind == ISA_BLSR);
    assert(t->sites[1].mode == ISA_MODE_JMP_RELOCATE);
    assert(t->sites[5].mode == ISA_MODE_INT3);
    assert(isa_stub_area_size(t) == 6 * 256);
    assert(isa_site_set(&t->sites[0]) == CPU_SET_BMI1);
    assert(isa_site_set(&t->sites[1]) == CPU_SET_MOVBE);
}

/* The probe's int3 handler (Task 7) in miniature: registers in, isa_rewrite_trap, registers out. */
static IsaTable *trap_table;

#ifdef _WIN32
static LONG WINAPI trap_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    DWORD64 *gpr[16] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                        &c->R8,  &c->R9,  &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
    if (isa_rewrite_trap_context(trap_table, c, (uintptr_t)info->ExceptionRecord->ExceptionAddress))
        return EXCEPTION_CONTINUE_EXECUTION;
    IsaRegs regs;
    for (int i = 0; i < 16; i++) regs.gpr[i] = *gpr[i];
    regs.rflags = c->EFlags;
    uintptr_t rip;
    if (!isa_rewrite_trap(trap_table, (uintptr_t)info->ExceptionRecord->ExceptionAddress, &regs, &rip))
        return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 16; i++) *gpr[i] = regs.gpr[i];
    c->EFlags = (DWORD)regs.rflags;
    c->Rip = rip;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void install_trap_handler(void) { assert(AddVectoredExceptionHandler(1, trap_handler) != NULL); }
#else
static void trap_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    static const int index[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                  REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    IsaRegs regs;
    for (int i = 0; i < 16; i++) regs.gpr[i] = (uint64_t)g[index[i]];
    regs.rflags = (uint64_t)g[REG_EFL];
    uintptr_t rip;
    if (!isa_rewrite_trap(trap_table, (uintptr_t)g[REG_RIP] - 1, &regs, &rip)) abort();
    for (int i = 0; i < 16; i++) g[index[i]] = (greg_t)regs.gpr[i];
    g[REG_EFL] = (greg_t)regs.rflags;
    g[REG_RIP] = (greg_t)rip;
}

static void install_trap_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGTRAP, &sa, NULL) == 0);
}
#endif

/* F16C on a host that has it, through the int3 path a Sandy Bridge host takes: the trapped context's
 * xmm1 gets the four singles and ymm1's upper half is zeroed, as the instruction does. */
static void test_vector_int3_via_handler(void) {
#ifdef _WIN32
    reset_region();
    IsaSite site = site_at(S6, 5, ISA_VCVTPH2PS, ISA_MODE_INT3, 1, 1);
    site.width = 0;
    IsaTable *t = table_of(&site, 1);
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_F16C);
    assert(r.unsupported_needed == 0 && r.int3 == 1 && image[S6] == 0xcc);
    trap_table = t;
    /* halves (low first): largest normal, largest subnormal, -1, -inf; the upper lanes are garbage */
    uint64_t lanes[4] = {0xfc00bc0003ff7bffull, 0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull};
    uint64_t result[4] = {0, 0, 0, 0};
    void *fn = image + S6;
    __asm__ volatile("vmovdqu (%[in]), %%ymm1\n\tleaq -128(%%rsp), %%rsp\n\tcall *%[fn]\n\t"
                     "leaq 128(%%rsp), %%rsp\n\tvmovdqu %%ymm1, (%[out])\n\tvzeroupper"
                     :
                     : [in] "r"(lanes), [out] "r"(result), [fn] "r"(fn)
                     : "memory", "xmm1");
    assert(result[0] == 0x387fc000477fe000ull && result[1] == 0xff800000bf800000ull);
    assert(result[2] == 0 && result[3] == 0);
    assert(t->hits[0] == 1);
#endif
}

static IsaSite movbe_load_at(uint64_t offset, uint8_t length, uint8_t width, uint8_t dst, int64_t disp) {
    IsaSite s = site_at(offset, length, ISA_MOVBE_LOAD, ISA_MODE_JMP, dst, ISA_NO_REG);
    s.width = width;
    s.mem_slot = ISA_MEM_SRC;
    s.base = 7;   /* rdi */
    s.scale = 1;
    s.disp = disp;
    return s;
}

static uint64_t call_keeping_cf(void *fn, const void *arg, unsigned char *cf) {
    uint64_t result;
    __asm__ volatile("stc\n\tleaq -128(%%rsp), %%rsp\n\tcall *%[fn]\n\tleaq 128(%%rsp), %%rsp\n\tsetc %[cf]"
                     : "=a"(result), [cf] "=r"(*cf)
                     : [fn] "r"(fn), "D"(arg)
                     : "rsi", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    return result;
}

static void test_inline_movbe_load(void) {
    reset_region();
    IsaSite sites[3] = {movbe_load_at(S7, 5, 32, 0, 0), movbe_load_at(S8, 6, 64, 0, 8), movbe_load_at(S9, 5, 32, 9, 0)};
    IsaTable *t = table_of(sites, 3);
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_MOVBE);
    assert(r.rewritten == 3 && r.jmp == 3 && r.skipped == 0);
    for (int i = 0; i < 3; i++) {   /* no emulator call in these stubs: mov rax, isa_emulate is absent */
        const uint64_t target = (uint64_t)(uintptr_t)isa_emulate;
        for (int at = 0; at + 10 <= ISA_STUB_BYTES; at++)
            assert(!(stubs[i * ISA_STUB_BYTES + at] == 0x48 && stubs[i * ISA_STUB_BYTES + at + 1] == 0xb8 &&
                     memcmp(stubs + i * ISA_STUB_BYTES + at + 2, &target, 8) == 0));
    }
    const uint64_t data[2] = {0x0102030405060708ull, 0x1122334455667788ull};
    unsigned char cf = 0;
    assert(call_keeping_cf(image + S7, data, &cf) == 0x08070605ull && cf == 1);
    assert(call_keeping_cf(image + S8, data, &cf) == 0x8877665544332211ull && cf == 1);
    assert(call_keeping_cf(image + S9, data, &cf) == 0x08070605ull && cf == 1);
    assert(call_keeping_cf(image + S7, data, &cf) == 0x08070605ull);
    assert(t->hits[0] == 2 && t->hits[1] == 1 && t->hits[2] == 1);
}

/* rax and ZF/CF/OF (bits 0-2) after calling fn(rdi = a, rsi = b) */
static uint64_t call_flags(void *fn, uint64_t a, uint64_t b, unsigned *flags) {
    uint64_t result;
    unsigned char zf, cf, of;
    __asm__ volatile("stc\n\tleaq -128(%%rsp), %%rsp\n\tcall *%[fn]\n\tleaq 128(%%rsp), %%rsp\n\t"
                     "setz %[zf]\n\tsetc %[cf]\n\tseto %[of]"
                     : "=a"(result), [zf] "=r"(zf), [cf] "=r"(cf), [of] "=r"(of), "+D"(a), "+S"(b)
                     : [fn] "r"(fn)
                     : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    *flags = zf | cf << 1 | of << 2;
    return result;
}

static uint64_t native_bextr(uint64_t src, uint64_t ctrl, int w, unsigned *flags) {
    uint64_t r;
    unsigned char zf, cf, of;
    if (w) __asm__("bextr %4, %3, %0\n\tsetz %1\n\tsetc %2" : "=r"(r), "=r"(zf), "=r"(cf) : "r"(src), "r"(ctrl) : "cc");
    else {
        uint32_t r32;
        __asm__("bextr %k4, %k3, %k0\n\tsetz %1\n\tsetc %2" : "=r"(r32), "=r"(zf), "=r"(cf) : "r"(src), "r"(ctrl) : "cc");
        r = r32;
    }
    of = 0;   /* BEXTR clears OF */
    *flags = zf | cf << 1 | of << 2;
    return r;
}

static void test_inline_bextr(void) {
    reset_region();
    IsaSite sites[4] = {site_at(S10, 5, ISA_BEXTR, ISA_MODE_JMP, 0, 7), site_at(S11 + 4, 5, ISA_BEXTR, ISA_MODE_JMP, 1, 2),
                        site_at(S12, 5, ISA_BEXTR, ISA_MODE_JMP, 0, 7), site_at(S13 + 6, 5, ISA_BEXTR, ISA_MODE_JMP, 9, 10)};
    sites[0].src2 = 6;
    sites[1].src2 = 0;
    sites[2].src2 = 6;
    sites[2].width = 64;
    sites[3].src2 = 11;
    IsaTable *t = table_of(sites, 4);
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_BMI1);
    assert(r.rewritten == 4 && r.skipped == 0);
    const uint64_t target = (uint64_t)(uintptr_t)isa_emulate;
    for (int i = 0; i < 4; i++)
        for (int at = 0; at + 10 <= ISA_STUB_BYTES; at++)
            assert(!(stubs[i * ISA_STUB_BYTES + at] == 0x48 && stubs[i * ISA_STUB_BYTES + at + 1] == 0xb8 &&
                     memcmp(stubs + i * ISA_STUB_BYTES + at + 2, &target, 8) == 0));
    void *fns[4] = {image + S10, image + S11, image + S12, image + S13};
    static const uint8_t fields[] = {0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 200, 255};
    for (int f = 0; f < 4; f++) {
        const int w = f == 2;
        for (unsigned i = 0; i < sizeof fields; i++)
            for (unsigned j = 0; j < sizeof fields; j++)
                for (int v = 0; v < 3; v++) {
                    const uint64_t src = v == 0 ? 0 : v == 1 ? 0xffffffffffffffffull : 0x8badf00ddeadbeefull;
                    const uint64_t ctrl = fields[i] | (uint64_t)fields[j] << 8 | 0xabcd0000ull;
                    unsigned got_flags, want_flags;
                    const uint64_t got = call_flags(fns[f], src, ctrl, &got_flags);
                    const uint64_t want = native_bextr(src, ctrl, w, &want_flags);
                    if ((w ? got : (uint32_t)got) != want || got_flags != want_flags) {
                        fprintf(stderr, "bextr snippet %d src %llx ctrl %llx: %llx flags %x, native %llx flags %x\n", f,
                                (unsigned long long)src, (unsigned long long)ctrl, (unsigned long long)got, got_flags,
                                (unsigned long long)want, want_flags);
                        abort();
                    }
                    if (!w) assert(got >> 32 == 0);
                }
    }
}

static void test_int3_site_via_handler(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(image[S4] == 0xcc);
    trap_table = t;
    install_trap_handler();
    Snippet s4 = (Snippet)(void *)(image + S4);
    assert(s4(0x1000, NULL) == 19 && _lzcnt_u32(0x1000) == 19);
    assert(t->hits[4] == 1);
}

static void test_trap_unknown_address(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    IsaRegs regs, before;
    for (int i = 0; i < 16; i++) regs.gpr[i] = 0x1111111111111111ull * (uint64_t)(i + 1);
    regs.rflags = 0x202;
    before = regs;
    uintptr_t rip = 0x1234;
    assert(isa_rewrite_trap(t, (uintptr_t)image + 0x3ff, &regs, &rip) == 0);
    assert(memcmp(&regs, &before, sizeof regs) == 0 && rip == 0x1234);
    assert(isa_rewrite_trap(NULL, (uintptr_t)image + S4, &regs, &rip) == 0);
}

static void test_site_of_stub(void) {
    reset_region();
    IsaTable *t = snippet_table();
    isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(isa_rewrite_site_of_stub(t, (uintptr_t)stubs + 256 + 10) == 1);
    assert(isa_rewrite_site_of_stub(t, (uintptr_t)stubs - 1) == -1);
    assert(isa_rewrite_site_of_stub(t, (uintptr_t)stubs + t->count * 256) == -1);
}

static void test_report_top_sites(void) {
    static const uint8_t kinds[4] = {ISA_BLSR, ISA_MOVBE_LOAD, ISA_LZCNT, ISA_BEXTR};
    IsaSite sites[4];
    memset(sites, 0, sizeof sites);
    for (int i = 0; i < 4; i++) {
        sites[i].offset = 0x10 * (uint64_t)(i + 1);
        sites[i].kind = kinds[i];
    }
    uint64_t hits[4] = {5, 0, 7, 1};
    IsaTable t;
    memset(&t, 0, sizeof t);
    t.sites = sites;
    t.hits = hits;
    t.count = 4;
    FILE *f = tmpfile();
    assert(f != NULL);
    isa_rewrite_report(&t, f);
    rewind(f);
    char text[256];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    assert(strcmp(text, "ISA: most executed sites:\n  0x30 lzcnt 7\n  0x10 blsr 5\n  0x40 bextr 1\n"
                        "ISA: hits by instruction: lzcnt 7, bextr 1, blsr 5\n") == 0);
}

/* The instruction after a JMP_RELOCATE site changed (a patch or hook): the site falls back to int3,
 * so the changed instruction runs instead of the table's copy. */
static void test_relocated_next_changed(void) {
    reset_region();
    image[S2 + 6] = 0x31;   /* mov eax, ecx -> xor eax, eax */
    image[S2 + 7] = 0xc0;
    IsaTable *t = snippet_table();
    IsaReport r = isa_rewrite_apply(t, image, stubs, 0xffffffffu);
    assert(r.rewritten == 5 && r.jmp == 3 && r.relocated == 0 && r.int3 == 2 && r.skipped == 0);
    assert(t->sites[1].mode == ISA_MODE_INT3);
    assert(image[S2 + 2] == 0xcc && memcmp(image + S2 + 3, pristine + S2 + 3, 3) == 0);
    assert(image[S2 + 6] == 0x31 && image[S2 + 7] == 0xc0);
    trap_table = t;
    install_trap_handler();
    uint32_t buffer = 0;
    Snippet s2 = (Snippet)(void *)(image + S2);
    uint64_t result = s2((uint64_t)(uintptr_t)&buffer, (uint64_t *)(uintptr_t)0x11223344);
    assert(buffer == 0x44332211 && result == 0);
    assert(t->hits[1] == 1);
}

/* A copy of the fixture, `size` bytes long, with byte `at` set to `value`, must not load. */
static void expect_loaded(const unsigned char *fixture, long size) {
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/port-isa-stub-test-ok.bin", dir);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(fixture, 1, (size_t)size, f) == (size_t)size && fclose(f) == 0);
    assert(isa_table_load(path) != NULL);
    remove(path);
}

static void expect_rejected(const unsigned char *fixture, long size, long at, unsigned char value) {
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/port-isa-stub-test.bin", dir);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    for (long i = 0; i < size; i++) fputc(i == at ? value : fixture[i], f);
    assert(fclose(f) == 0);
    assert(isa_table_load(path) == NULL);
    remove(path);
}

enum { RECORD0 = 64, RECORD1 = 128, R_LENGTH = 8, R_KIND = 9, R_WIDTH = 10, R_MODE = 11, R_DST = 12, R_SRC1 = 13, R_MEM_SLOT = 15,
       R_BASE = 16, R_SCALE = 18 };

/* Last: each rejection prints one "ISA: ..." line on stderr. */
static void test_table_load_rejects(const char *path) {
    unsigned char fixture[64 + 6 * 64 + 1];
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    long size = (long)fread(fixture, 1, sizeof fixture - 1, f);
    assert(size == (long)sizeof fixture - 1 && fgetc(f) == EOF);
    fclose(f);
    fixture[size] = 0;
    assert(fixture[RECORD0 + R_MODE] == ISA_MODE_JMP && fixture[RECORD0 + R_LENGTH] == 5);
    assert(fixture[RECORD1 + R_MEM_SLOT] == ISA_MEM_DST && fixture[RECORD1 + R_SCALE] == 1);
    expect_rejected(fixture, size, 0, 'X');                                /* magic */
    expect_rejected(fixture, size + 1, -1, 0);                             /* file size */
    expect_rejected(fixture, size - 1, -1, 0);
    expect_rejected(fixture, size, RECORD1, fixture[RECORD0]);             /* offsets not ascending */
    expect_rejected(fixture, size, RECORD0 + R_DST, 16);                   /* register 16 */
    expect_rejected(fixture, size, RECORD1 + R_BASE, 16);
    expect_rejected(fixture, size, RECORD0 + R_WIDTH, 8);                  /* width 8 */
    expect_rejected(fixture, size, RECORD1 + R_SCALE, 3);                  /* scale 3 */
    expect_rejected(fixture, size, RECORD0 + R_MODE, ISA_MODE_INT3);       /* 5-byte int3 */
    /* ... except VCVTPH2PS xmm, xmm, which traps at its full length (the F16C sites are 5 bytes) */
    unsigned char vector[sizeof fixture];
    memcpy(vector, fixture, sizeof vector);
    vector[RECORD0 + R_KIND] = ISA_VCVTPH2PS;
    vector[RECORD0 + R_MODE] = ISA_MODE_INT3;
    vector[RECORD0 + R_SRC1] = 2;
    expect_loaded(vector, size);
    expect_rejected(vector, size, RECORD0 + R_MEM_SLOT, ISA_MEM_SRC);     /* vcvtph2ps xmm, [mem] */
    expect_rejected(vector, size, RECORD0 + R_SRC1, ISA_NO_REG);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: isa-stub-test tests/data/isa_fixture.bin\n");
        return 2;
    }
#ifdef _WIN32
    region = VirtualAlloc(NULL, 3 * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(region != NULL);
#else
    region = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
#endif
    image = region;
    stubs = region + PAGE;
    memset(pristine, 0xcc, sizeof pristine);
    memcpy(pristine + S1, S1_CODE, sizeof S1_CODE);
    memcpy(pristine + S2, S2_CODE, sizeof S2_CODE);
    memcpy(pristine + S3, S3_CODE, sizeof S3_CODE);
    memcpy(pristine + S4, S4_CODE, sizeof S4_CODE);
    memcpy(pristine + S5, S5_CODE, sizeof S5_CODE);
    memcpy(pristine + ANDN_AT, ANDN_CODE, sizeof ANDN_CODE);
    memcpy(pristine + S6, S6_CODE, sizeof S6_CODE);
    memcpy(pristine + S7, S7_CODE, sizeof S7_CODE);
    memcpy(pristine + S8, S8_CODE, sizeof S8_CODE);
    memcpy(pristine + S9, S9_CODE, sizeof S9_CODE);
    memcpy(pristine + S10, S10_CODE, sizeof S10_CODE);
    memcpy(pristine + S11, S11_CODE, sizeof S11_CODE);
    memcpy(pristine + S12, S12_CODE, sizeof S12_CODE);
    memcpy(pristine + S13, S13_CODE, sizeof S13_CODE);
    test_jmp_site();
    test_relocated_site();
    test_adjacent_sites();
    test_xmm_preserved();
    test_sets_filter();
    test_skipped_when_bytes_differ();
    test_table_mismatch();
    test_unsupported_blocks_apply();
    test_largest_stub_fits();
    test_table_load_fixture(argv[1]);
    test_int3_site_via_handler();
    test_vector_int3_via_handler();
    test_inline_movbe_load();
    test_inline_bextr();
    test_trap_unknown_address();
    test_site_of_stub();
    test_report_top_sites();
    test_relocated_next_changed();
    test_table_load_rejects(argv[1]);
    puts("isa-stub-test ok");
    return 0;
}
