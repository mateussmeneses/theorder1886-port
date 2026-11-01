// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise GPU uploads while keeping guest data and page protection inside this fixture.
#define runtime_memory_read_backing unused_runtime_memory_read_backing
#define runtime_memory_gpu_protect unused_runtime_memory_gpu_protect
#define runtime_memory_clamp unused_runtime_memory_clamp
#define runtime_memory_region unused_runtime_memory_region
#include "test_gpu_runtime_stubs.cpp"
#undef runtime_memory_read_backing
#undef runtime_memory_gpu_protect
#undef runtime_memory_clamp
#undef runtime_memory_region
#include <cstring>
extern "C" {
void runtime_memory_read_backing(uintptr_t, void* data, uint64_t size) { std::memset(data, 0x55, size); }
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) {}
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { return size; }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { return 0; }
}
