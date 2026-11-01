// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime stubs for the renderpass-barrier regression. Like test_dma_runtime_stubs.cpp, but the
// arena download's synchronous write-back path reaches runtime_memory_write_backing (which the
// shared gpu stub aborts in), so this fixture no-ops it: the test only cares that the pre-transfer
// barrier was recorded outside the render pass, not that guest bytes were written back.
#define runtime_memory_read_backing unused_runtime_memory_read_backing
#define runtime_memory_write_backing unused_runtime_memory_write_backing
#define runtime_memory_gpu_protect unused_runtime_memory_gpu_protect
#define runtime_memory_clamp unused_runtime_memory_clamp
#define runtime_memory_region unused_runtime_memory_region
#include "test_gpu_runtime_stubs.cpp"
#undef runtime_memory_read_backing
#undef runtime_memory_write_backing
#undef runtime_memory_gpu_protect
#undef runtime_memory_clamp
#undef runtime_memory_region
#include <cstring>
extern "C" {
void runtime_memory_read_backing(uintptr_t, void* data, uint64_t size) { std::memset(data, 0x55, size); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t size) { return static_cast<int>(size); }
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) {}
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { return size; }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { return 0; }
}
