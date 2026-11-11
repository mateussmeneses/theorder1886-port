// SPDX-License-Identifier: GPL-2.0-or-later
// PortGuest::Read (gpu/shim/port_guest_memory.h): guarded copies of guest descriptor tables.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "port_guest_memory.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
__thread sigjmp_buf* runtime_fault_recover = nullptr;
std::uint64_t runtime_disabled_optimizations = 0;
#ifdef _WIN32
__attribute__((noreturn)) void port_longjmp(unsigned long long* buffer, int value);
#endif
}

#ifdef _WIN32
// The same redirect as src/probe.c's vectored handler: resume in port_longjmp at the thread's
// recovery point, outside the exception dispatcher.
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !runtime_fault_recover) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    unsigned long long* buffer = *runtime_fault_recover;
    runtime_fault_recover = nullptr;
    CONTEXT* c = info->ContextRecord;
    c->Rsp = ((c->Rsp - 128) & ~(DWORD64)15) - 40;
    c->Rcx = (DWORD64)(uintptr_t)buffer;
    c->Rdx = 1;
    c->Rip = (DWORD64)(uintptr_t)port_longjmp;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

int main() {
    std::uint32_t table[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::uint32_t out[4];

    // Null and low addresses read as zero without touching memory.
    std::memset(out, 0xAB, sizeof(out));
    assert(!PortGuest::Read(out, 0, sizeof(out)));
    for (auto v : out) assert(v == 0);
    std::memset(out, 0xAB, sizeof(out));
    assert(!PortGuest::Read(out, 0x8000, sizeof(out)));
    for (auto v : out) assert(v == 0);

    // A mapped table is copied, and no recovery point is left behind.
    assert(PortGuest::Read(out, reinterpret_cast<std::uintptr_t>(table), sizeof(out)));
    assert(std::memcmp(out, table, sizeof(out)) == 0);
    assert(runtime_fault_recover == nullptr);

    // A caller's recovery point (draw-preparation worker) is kept, not replaced.
    sigjmp_buf outer;
    runtime_fault_recover = &outer;
    assert(PortGuest::Read(out, reinterpret_cast<std::uintptr_t>(table), sizeof(out)));
    assert(runtime_fault_recover == &outer);
    runtime_fault_recover = nullptr;

#ifdef _WIN32
    // An unmapped table (the game freed it since the shader was built) reads as zero.
    void* handler = AddVectoredExceptionHandler(1, vectored_fault);
    assert(handler);
    void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    assert(page);
    std::memset(out, 0xAB, sizeof(out));
    assert(!PortGuest::Read(out, reinterpret_cast<std::uintptr_t>(page) + 16, sizeof(out)));
    for (auto v : out) assert(v == 0);
    assert(runtime_fault_recover == nullptr);
    assert(!PortGuest::Read(out, reinterpret_cast<std::uintptr_t>(page), sizeof(out))); // again
    // Once mapped, the same table reads normally.
    DWORD old = 0;
    assert(VirtualProtect(page, 4096, PAGE_READWRITE, &old));
    std::memcpy(page, table, sizeof(table));
    assert(PortGuest::Read(out, reinterpret_cast<std::uintptr_t>(page), sizeof(out)));
    assert(std::memcmp(out, table, sizeof(out)) == 0);
    VirtualFree(page, 0, MEM_RELEASE);
    RemoveVectoredExceptionHandler(handler);
#endif
    std::puts("guest-memory-test: ok");
    return 0;
}
