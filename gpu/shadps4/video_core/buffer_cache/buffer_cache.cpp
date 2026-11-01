// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "port_backtrace.h"
#include "port_copy.h"
#include "port_toggles.h"
#include <xxhash.h>

#include "common/alignment.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
constexpr std::optional<u32> CmaskColorExpandedValue(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return 0xFFFFFFFF;
    case 2:
        return 0xDDDDDDDD;
    case 4:
        return 0xEEEEEEEE;
    case 8:
        return 0xFFFFFFFF;
    default:
        return 0;
    }
}
static_assert(CmaskColorExpandedValue(1) == 0xFFFFFFFF);
static_assert(CmaskColorExpandedValue(2) == 0xDDDDDDDD);
static_assert(CmaskColorExpandedValue(4) == 0xEEEEEEEE);
static_assert(CmaskColorExpandedValue(8) == 0xFFFFFFFF);

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    arena_memory_type_bits = reqs.memoryTypeBits;
    // port (bloodborne_pc 5f65e2d): discrete GPUs keep a reserve of VRAM for the guest memory
    // backing (EnsureResident). It only grows as areas load and, on NVIDIA, cannot be outside
    // VRAM (sparse buffers accept VRAM types only): on 4 GB cards textures filled VRAM and the
    // next area stopped the game with ErrorOutOfDeviceMemory. Textures keep their placement.
    if (!instance.IsIntegrated()) {
        const auto& props = instance.GetMemoryProperties();
        const u64 heap =
            props.memoryHeaps[props.memoryTypes[arena_memory_type_index].heapIndex].size;
        const u64 size = Common::AlignDown(std::min<u64>(512_MB, heap / 8), block_size);
        const vk::MemoryAllocateFlagsInfo reserve_flags = {
            .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
        };
        const auto [result, memory] = device.allocateMemory({
            .pNext = &reserve_flags,
            .allocationSize = size,
            .memoryTypeIndex = arena_memory_type_index,
        });
        if (result == vk::Result::eSuccess) {
            reserve_memory = memory;
            reserve_size = size;
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    if (RegionManager::ReadWatchActive()) {
        UnwatchCpuWritten(device_addr, size);
    }
    // Byte-precise writes were tried here (2026-10-18): every write-fault download landed beside
    // the GPU's bytes, but a ring buffer at 0x105b502000 is written by the CPU over bytes the
    // GPU wrote a frame earlier, after an earlier write had unprotected the page: skipping the
    // GPU's bytes at upload dropped the CPU's new data and the game hung at its first frames.
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, false, assume_locks);
    });
}

bool BufferCache::TouchesGpuBytes(VAddr device_addr, u64 size) {
    std::scoped_lock lock{gpu_ranges_mutex};
    return gpu_modified_ranges.Intersects(device_addr, size);
}

void BufferCache::UnwatchCpuWritten(VAddr device_addr, u64 size) {
    std::scoped_lock lock{read_watch_mutex};
    if (read_watched.empty()) {
        return;
    }
    // The ranges that start before the write's end and end after its start.
    auto it = read_watched.upper_bound(device_addr + size);
    while (it != read_watched.begin()) {
        --it;
        if (it->first + it->second <= device_addr) {
            if (device_addr - it->first > ReadWatchMaxBytes) {
                break; // ranges are at most ReadWatchMaxBytes long
            }
            continue;
        }
        never_watched.insert(it->first);
        memory_tracker->WatchReads(it->first, it->second, false);
        it = read_watched.erase(it);
    }
}

// port: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const PortCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const PortCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !PortToggle::Disabled(PortToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    PortCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("THEORDER_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= BYTES_PER_PAGE || PortToggle::Disabled(PortToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

extern thread_local const void* port_fault_rip; // page_manager.cpp

namespace {
/// port: THEORDER_FRAME_STATS: guest accesses that download GPU-written buffer memory first (each
/// download waits for the GPU), by download window: CPU writes and reads, the time taken, and
/// the instruction of the write fault that asked most often (none: a runtime write hook).
struct DownloadNote {
    u64 writes = 0, reads = 0;
    double ms = 0;
    std::unordered_map<const void*, u64> rips;
};

void NoteDownload(VAddr window, bool is_write, std::chrono::steady_clock::duration took,
                  const void* rip) {
    static const bool stats = std::getenv("THEORDER_FRAME_STATS") != nullptr;
    if (!stats) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<VAddr, DownloadNote> notes;
    static auto since = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    auto& note = notes[window];
    ++(is_write ? note.writes : note.reads);
    note.ms += std::chrono::duration<double, std::milli>(took).count();
    ++note.rips[rip];
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - since).count();
    if (seconds < 5.0) {
        return;
    }
    std::vector<std::pair<VAddr, DownloadNote>> top(notes.begin(), notes.end());
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    std::printf("Buffer downloads by time, per second:");
    for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
        const auto& [address, n] = top[i];
        const auto rip = std::ranges::max_element(
            n.rips, [](const auto& a, const auto& b) { return a.second < b.second; });
        std::printf(" %#llx: %.0f CPU writes, %.0f reads, %.1f ms, mostly from %p;",
                    static_cast<unsigned long long>(address), n.writes / seconds, n.reads / seconds,
                    n.ms / seconds, rip == n.rips.end() ? nullptr : rip->first);
    }
    std::printf("\n");
    notes.clear();
    since = now;
}
} // namespace

void BufferCache::UnmarkRegionAsGpuModified(VAddr device_addr, u64 size, bool is_write) {
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, is_write);
    std::scoped_lock lock{gpu_ranges_mutex};
    gpu_modified_ranges.Subtract(device_addr, size);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool async,
                             bool assume_locks) {
    // The callers of the first writes from each instruction that ask (the faulting thread's
    // stack, for llvm-symbolizer).
    static const bool stats = std::getenv("THEORDER_FRAME_STATS") != nullptr;
    if (stats && is_write) {
        static std::mutex traced_mutex;
        static std::unordered_map<const void*, u32> traced;
        std::scoped_lock lock{traced_mutex};
        if (traced[port_fault_rip]++ < 2) {
            std::printf("Buffer download for a write at %#llx from %p\n",
                        static_cast<unsigned long long>(device_addr), port_fault_rip);
            PortPrintBacktrace("buffer download");
        }
    }
    const auto flush_request = [this, device_addr, size, is_write, async,
                                rip = port_fault_rip] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        if (is_write && stats) {
            // Whether the CPU write lands on bytes the GPU wrote or only shares their page.
            static u64 inside = 0, outside = 0;
            ++(TouchesGpuBytes(device_addr, size) ? inside : outside);
            if ((inside + outside) % 512 == 0) {
                std::printf("Write-fault downloads: %llu on GPU-written bytes, %llu beside them\n",
                            static_cast<unsigned long long>(inside),
                            static_cast<unsigned long long>(outside));
            }
        }

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        const auto started = std::chrono::steady_clock::now();
        const bool sun_page = SynchronizedPage(device_addr, size);
        if (sun_page) {
            DeliverSunCopies();
        }
        // port: a write marks only the faulting range CPU-modified, not the whole window
        // (upstream), so the window's other pages stay protected and are not uploaded again.
        DownloadMemory(arena, window_start, window_end - window_start, false, async);
        NoteDownload(window_start, is_write, std::chrono::steady_clock::now() - started, rip);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
        if (RegionManager::ReadWatchActive()) {
            PromoteReadback(device_addr);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::KeepPageSynchronized(VAddr device_addr, u64 size, u64 pgm_hash) {
    static const bool enabled = [] {
        const char* env = std::getenv("THEORDER_SUN_READBACK_SYNC");
        if (!env) env = std::getenv("BB_SUN_READBACK_SYNC");
        return !(env && env[0] == '0');
    }();
    if (!enabled || device_addr == 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{synchronized_mutex};
    if (synchronized_ranges.Intersects(device_addr, size)) {
        return;
    }
    synchronized_ranges.Add(device_addr, size);
    static u32 logged = 0;
    if (logged++ < 32) {
        std::printf("Readback sync: %#llx size %llu (shader %016llx)\n",
                    static_cast<unsigned long long>(device_addr),
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(pgm_hash));
    }
}

void BufferCache::RecordSunCopies() {
    std::scoped_lock lock{synchronized_mutex};
    std::vector<std::pair<VAddr, VAddr>> ranges;
    synchronized_ranges.ForEach([&](VAddr start, VAddr end) { ranges.emplace_back(start, end); });
    if (ranges.empty()) {
        return;
    }
    u64 total = 0;
    for (const auto& [start, end] : ranges) {
        total += Common::AlignUp(end - start, 16);
    }
    SunBatch batch{};
    batch.staging = staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    u64 offset = 0;
    for (const auto& [start, end] : ranges) {
        const u64 size = end - start;
        const auto* arena = GetArena(start >> block_shift, (end - 1) >> block_shift);
        runtime.CopyBuffer(arena, batch.staging.buffer,
                           std::array{vk::BufferCopy{start - arena->cpu_addr,
                                                     batch.staging.offset + offset, size}});
        batch.items.push_back({start, size, offset});
        offset += Common::AlignUp(size, 16);
    }
    batch.tick = scheduler.CurrentTick();
    sun_batches.push_back(std::move(batch));
    // Completed batches older than the newest completed one are no longer needed.
    while (sun_batches.size() > 1 && scheduler.IsFree(sun_batches[1].tick)) {
        staging_pool.FreeDeferred(sun_batches.front().staging);
        sun_batches.pop_front();
    }
    // A stalled GPU must not grow the queue without limit.
    while (sun_batches.size() > 16) {
        staging_pool.FreeDeferred(sun_batches.front().staging);
        sun_batches.pop_front();
    }
}

bool BufferCache::DeliverSunCopies() {
    std::scoped_lock lock{synchronized_mutex};
    const SunBatch* newest = nullptr;
    for (const auto& batch : sun_batches) {
        if (scheduler.IsFree(batch.tick)) {
            newest = &batch;
        }
    }
    if (!newest) {
        return false;
    }
    newest->staging.buffer->Invalidate(newest->staging.offset, newest->staging.size);
    for (const auto& item : newest->items) {
        memory->TryWriteBacking(reinterpret_cast<void*>(item.addr),
                                newest->staging.mapped + item.staging_offset, item.size);
    }
    return true;
}

// The 4 KiB pages of a CPU write fault, against the registered ranges.
bool BufferCache::SynchronizedPage(VAddr addr, u64 size) {
    const VAddr start = Common::AlignDown(addr, 4_KB);
    const VAddr end = Common::AlignUp(addr + size, 4_KB);
    std::scoped_lock lock{synchronized_mutex};
    return synchronized_ranges.Intersects(start, end - start);
}

void BufferCache::NoteSmallWrite(VAddr device_addr, u32 size) {
    std::scoped_lock lock{read_watch_mutex};
    if (const auto it = readback_buffers.find(device_addr); it != readback_buffers.end()) {
        it->second = std::max(it->second, size);
        if (std::ranges::find(readback_dirty, device_addr) == readback_dirty.end()) {
            readback_dirty.push_back(device_addr);
        }
        return;
    }
    // Most small written buffers are never read by the CPU: watched once, they cost nothing
    // until a read. The cap bounds the map if an engine writes many distinct small ranges.
    if (read_watched.size() < 4096 && !never_watched.contains(device_addr) &&
        read_watched.emplace(device_addr, size).second) {
        memory_tracker->WatchReads(device_addr, size, true);
        const size_t n = read_watched.size() + never_watched.size();
        if ((n & (n - 1)) == 0 && n >= 16) {
            std::printf("Read watch: %zu ranges watched, %zu written by the CPU\n",
                        read_watched.size(), never_watched.size());
        }
    }
}

void BufferCache::PromoteReadback(VAddr device_addr) {
    std::scoped_lock lock{read_watch_mutex};
    // port: every watched range that shares the accessed page, not only one holding the address:
    // The Order polls a fence beside two shader-written blocks in its command buffer, and each
    // read (and each packet the driver writes there) waited for the GPU.
    const VAddr page = Common::AlignDown(device_addr, 4_KB);
    for (auto it = read_watched.lower_bound(page > ReadWatchMaxBytes ? page - ReadWatchMaxBytes : 0);
         it != read_watched.end() && it->first < page + 4_KB;) {
        const VAddr address = it->first;
        const u32 size = it->second;
        if (address + size <= page) {
            ++it;
            continue;
        }
        it = read_watched.erase(it);
        readback_buffers.emplace(address, size);
        memory_tracker->WatchReads(address, size, false);
        std::printf("Buffer readback: the CPU accesses %#llx's page (%#llx, %u bytes) after GPU "
                    "writes; copied back after each write from now on\n",
                    static_cast<unsigned long long>(device_addr),
                    static_cast<unsigned long long>(address), size);
    }
}

void BufferCache::QueueReadbacks() {
    SettleReadbacks();
    std::vector<VAddr> dirty;
    {
        std::scoped_lock lock{read_watch_mutex};
        if (readback_dirty.empty()) {
            return;
        }
        dirty.swap(readback_dirty);
    }
    for (const VAddr address : dirty) {
        u32 size;
        {
            std::scoped_lock lock{read_watch_mutex};
            size = readback_buffers[address];
        }
        const u64 first_block = address >> block_shift;
        const u64 last_block = (address + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        if (!texture_cache.DeferredReadbacks()) {
            DownloadMemory(arena, address, size, false, false);
            continue;
        }
        const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferCopy copy{
            .srcOffset = arena->Offset(address),
            .dstOffset = staging.offset,
            .size = size,
        };
        runtime.CopyBuffer(arena, staging.buffer, std::span{&copy, 1});
        u64 token;
        {
            std::scoped_lock lock{gpu_ranges_mutex};
            token = ++settle_counter;
            settle_tokens[address] = {size, token};
        }
        texture_cache.QueueBufferReadback(address, size, staging, token);
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size, bool is_write,
                                 bool async) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        std::scoped_lock lock{gpu_ranges_mutex};
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download =
        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 16, async);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    // Synchronize prior GPU writes to this arena buffer (e.g. compute SSBO writes) before the transfer
    // read. Without this barrier the copy can read stale L2 cache contents written before the
    // dispatch, which surfaced as all-zero readbacks of the cascade AABB ring buffer (TdlifePeng 6d27ee8e).
    // End any open dynamic-rendering instance first: a vkCmdPipelineBarrier2 recorded inside a render
    // pass is illegal without dynamicRenderingLocalRead (VUID-vkCmdPipelineBarrier2-None-09553/09554),
    // and the producer/consumer ordering it guards would otherwise not be honored (matches the sibling
    // EndRendering-before-CopyBuffer in ReadEdgeImagePages).
    scheduler.EndRendering();
    scheduler.Record([handle = arena->Handle(), size = arena->SizeBytes()](vk::CommandBuffer cmdbuf) {
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = handle,
            .offset = 0,
            .size = size,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlags{},
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
    });
    runtime.CopyBuffer(arena, download.buffer, copies);

    const auto write_data = [this, arena_base, device_addr, size, download, is_write, async,
                             copies = std::move(copies)] {
        download.Invalidate();
        for (const auto& copy : copies) {
            auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
            memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                    copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }

        if (async) {
            staging_pool.FreeDeferred(download);
        }
    };

    if (async) {
        scheduler.DeferOperation(std::move(write_data));
    } else {
        scheduler.Finish();
        write_data();
    }
}

namespace {
// port: THEORDER_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = PortStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("THEORDER_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        ObtainBufferFlags flags) {
    const bool is_written = True(flags & ObtainBufferFlags::IsWritten);
    const bool is_texel_buffer = True(flags & ObtainBufferFlags::IsTexelBuffer);
    const bool skip_stream_buffer = True(flags & ObtainBufferFlags::IgnoreStreamBuffer);
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    // port: a shader reading an HTILE surface sees what the hardware would have written there
    // (TextureCache::HtileReadValue); nothing here writes HTILE while depth is drawn.
    // THEORDER_HTILE_READ=0 leaves the guest bytes (A/B tests).
    static const bool htile_read = [] {
        const char* env = std::getenv("THEORDER_HTILE_READ");
        return !env || env[0] != '0';
    }();
    if (!is_written && htile_read) {
        if (const auto htile = texture_cache.HtileReadValue(device_addr)) {
            const u64 first_block = device_addr >> block_shift;
            const u64 last_block = (device_addr + size - 1) >> block_shift;
            const auto* arena = GetArena(first_block, last_block);
            EnsureResident(arena, first_block, last_block);
            runtime.FillBuffer(arena, arena->Offset(device_addr), Common::AlignUp(size, 4u),
                               *htile);
            static std::mutex reported_mutex;
            static std::unordered_map<VAddr, u32> reported;
            {
                std::scoped_lock lock{reported_mutex};
                if (reported.size() < 64 && reported.emplace(device_addr, *htile).second) {
                    std::printf("HTILE read: %#llx, %u bytes, word %#010x\n",
                                static_cast<unsigned long long>(device_addr), size, *htile);
                }
            }
            return {arena, arena->Offset(device_addr)};
        }
    }
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && !skip_stream_buffer && size <= STREAM_THRESHOLD &&
        !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // port: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !PortToggle::Disabled(PortToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = PortStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    if (is_written) {
        texture_cache.MarkAsMaybeGpuDirty(device_addr, size);
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            PortStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        NoteGpuWrite(device_addr, size);
        texture_cache.NoteMetaShaderWrite(device_addr);
        if (size <= ReadWatchMaxBytes && RegionManager::ReadWatchActive()) {
            NoteSmallWrite(device_addr, size);
        }
        // port: THEORDER_GPU_WRITE_WATCH=<hex address>: the first writable bindings in its 64 KiB
        // (the GPU-modified state is kept per page).
        static const VAddr watch = [] {
            const char* env = std::getenv("THEORDER_GPU_WRITE_WATCH");
            return env ? std::strtoull(env, nullptr, 16) & ~VAddr{0xffff} : 0;
        }();
        static std::atomic<int> watched{0};
        if (watch && device_addr < watch + 0x10000 && watch < device_addr + size &&
            watched.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::printf("GPU write watch: %#llx has a written binding %#llx, %u bytes\n",
                        static_cast<unsigned long long>(watch),
                        static_cast<unsigned long long>(device_addr), size);
            PortPrintBacktrace("GPU write watch");
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!PortToggle::Disabled(PortToggle::DeferredUploads)) {
        // port: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const PortCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                PortCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::MarkRegionAsGpuModified(VAddr addr, size_t size) {
    texture_cache.MarkAsMaybeGpuDirty(addr, size);
    memory_tracker->MarkRegionAsGpuModified(addr, size);
    NoteGpuWrite(addr, size);
}

void BufferCache::NoteGpuWrite(VAddr address, u64 size) {
    std::scoped_lock lock{gpu_ranges_mutex};
    gpu_modified_ranges.Add(address, size);
    // Copies recorded before this write no longer bring the newest bytes.
    for (auto it = settle_tokens.begin(); it != settle_tokens.end();) {
        const bool overlaps = it->first < address + size && address < it->first + it->second.first;
        it = overlaps ? settle_tokens.erase(it) : std::next(it);
    }
}

void BufferCache::NoteReadbackPublished(VAddr address, u64 size, u64 token) {
    std::scoped_lock lock{settled_mutex};
    settled.emplace_back(address, size, token);
}

void BufferCache::SettleReadbacks() {
    std::vector<std::tuple<VAddr, u64, u64>> published;
    {
        std::scoped_lock lock{settled_mutex};
        published.swap(settled);
    }
    if (published.empty()) {
        return;
    }
    boost::container::small_vector<VAddr, 8> clean_pages;
    {
        std::scoped_lock lock{gpu_ranges_mutex};
        for (const auto& [address, size, token] : published) {
            const auto it = settle_tokens.find(address);
            if (it == settle_tokens.end() || it->second.second != token) {
                continue; // written again since the copy, or not settled by copies
            }
            settle_tokens.erase(it);
            gpu_modified_ranges.Subtract(address, size);
            for (VAddr page = Common::AlignDown(address, 4_KB); page < address + size;
                 page += 4_KB) {
                if (!gpu_modified_ranges.Intersects(page, 4_KB)) {
                    clean_pages.push_back(page);
                }
            }
        }
    }
    // Nothing marks pages GPU-modified meanwhile: GPU writes come from this thread and the draw
    // pipe, which is drained here.
    for (const VAddr page : clean_pages) {
        memory_tracker->UnmarkRegionAsGpuModified(page, 4_KB, false);
    }
}

void BufferCache::ReadEdgeImagePages(const Image& image) {
    // May happen that after downloading the image and invalidating region,
    // that there were GPU modified ranges that are lost due to CPU reuploading.
    // This doesn't change tracker state and it is spected to call DownloadImageMemory after this.
    const VAddr image_addr = image.info.guest_address;
    const u64 image_size = image.info.guest_size;
    const VAddr image_end = image_addr + image_size;
    const VAddr page_start = PageManager::GetPageAddr(image_addr);
    const VAddr page_end = PageManager::GetNextPageAddr(image_end - 1);
    boost::container::small_vector<vk::BufferCopy, 2> copies;
    u64 total_size_bytes = 0;
    const auto [buffer, offset] = ObtainBuffer(page_start, page_end - page_start);
    const auto add_download = [&](VAddr start, VAddr end) {
        const u64 new_offset = start - buffer->CpuAddr();
        const u64 new_size = end - start;
        copies.push_back(vk::BufferCopy{
            .srcOffset = new_offset,
            .dstOffset = total_size_bytes,
            .size = new_size,
        });
        // Align up to avoid cache conflicts
        constexpr u64 align = 64ULL;
        constexpr u64 mask = ~(align - 1ULL);
        total_size_bytes += (new_size + align - 1) & mask;
    };
    {
        std::scoped_lock lock{gpu_ranges_mutex};
        gpu_modified_ranges.ForEachInRange(page_start, image_addr - page_start, add_download);
        gpu_modified_ranges.ForEachInRange(image_end, page_end - image_end, add_download);
        gpu_modified_ranges.Subtract(page_start, page_end - page_start);
    }
    if (total_size_bytes == 0) {
        return;
    }
    const auto download =
        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 16, true);
    for (auto& copy : copies) {
        // Modify copies to have the staging offset in mind
        copy.dstOffset += download.offset;
    }
    scheduler.EndRendering();
    runtime.CopyBuffer(buffer, download.buffer, copies);
    scheduler.DeferOperation(
        [this, base = buffer->CpuAddr(), download, copies = std::move(copies)]() {
            download.Invalidate();
            auto* memory = Core::Memory::Instance();
            for (const auto& copy : copies) {
                const VAddr copy_device_addr = base + copy.srcOffset;
                const u64 dst_offset = copy.dstOffset - download.offset;
                memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr),
                                        download.mapped + dst_offset, copy.size);
            }
            staging_pool.FreeDeferred(download);
        });
}

bool BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    bool needs_barrier{};
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        const auto* arena = address_space[page];
        SynchronizeMemory(arena, device_addr, size, false, false);
        needs_barrier |= runtime.IsBufferAccessed(arena, arena->Offset(device_addr), size);
    }
    return needs_barrier;
}

void BufferCache::MarkDmaBuffersAccessed(vk::PipelineStageFlags2 stage) {
    // BDA reads bypass descriptors, but later uploads must still wait for those shader reads.
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        const auto* arena = address_space[page];
        runtime.AccessBuffer(arena, arena->Offset(device_addr), size, stage,
                             vk::AccessFlagBits2::eShaderRead);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    PortStats::Timer timer{PortStats::t_resident};

    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    vk::MemoryAllocateInfo alloc_info = {
        .pNext = &alloc_flags,
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    // THEORDER_ARENA_LIMIT_MB=N: VRAM counts as full once N MiB are backed here (tests the reserve).
    static const u64 arena_limit = [] {
        const char* env = std::getenv("THEORDER_ARENA_LIMIT_MB");
        if (!env) env = std::getenv("BB_ARENA_LIMIT_MB");
        return (env ? std::strtoull(env, nullptr, 10) : 2560) << 20;
    }();
    static u64 arena_allocated = 0;
    vk::Result alloc_result = vk::Result::eErrorOutOfDeviceMemory;
    vk::DeviceMemory device_memory{};
    if (!arena_limit || arena_allocated + alloc_info.allocationSize <= arena_limit) {
        std::tie(alloc_result, device_memory) = instance.GetDevice().allocateMemory(alloc_info);
        if (alloc_result == vk::Result::eSuccess) {
            arena_allocated += alloc_info.allocationSize;
        }
    }
    u64 memory_base{}; ///< the backing's offset in `device_memory` (non-zero in the reserve)
    if (alloc_result == vk::Result::eErrorOutOfDeviceMemory) {
        // port (bloodborne_pc): VRAM full. The guest memory the GPU uses is backed here and only
        // grows as areas load. Another memory type the arena accepts, outside VRAM (system memory
        // the GPU reaches), is slower but keeps the game going; then the reserve.
        const auto& props = instance.GetMemoryProperties();
        const u32 vram_heap = props.memoryTypes[arena_memory_type_index].heapIndex;
        for (u32 i = 0; i < props.memoryTypeCount && alloc_result != vk::Result::eSuccess; ++i) {
            if (((arena_memory_type_bits >> i) & 1) == 0 ||
                props.memoryTypes[i].heapIndex == vram_heap) {
                continue;
            }
            alloc_info.memoryTypeIndex = i;
            std::tie(alloc_result, device_memory) = instance.GetDevice().allocateMemory(alloc_info);
        }
        if (alloc_result == vk::Result::eSuccess) {
            Vulkan::NoteOverBudget("buffer");
        } else if (reserve_memory && reserve_size - reserve_used >= alloc_info.allocationSize) {
            device_memory = reserve_memory;
            memory_base = reserve_used;
            const u64 quarter_before = reserve_used * 4 / reserve_size;
            reserve_used += alloc_info.allocationSize;
            alloc_result = vk::Result::eSuccess;
            if (memory_base == 0 || reserve_used * 4 / reserve_size != quarter_before) {
                std::printf("GPU: video memory full: game memory from its reserve (%llu of %llu "
                            "MiB used)\n",
                            static_cast<unsigned long long>(reserve_used >> 20),
                            static_cast<unsigned long long>(reserve_size >> 20));
                std::fflush(stdout);
            }
        } else {
            std::printf("GPU: video memory full: no room for %llu MiB of game memory (only VRAM "
                        "can hold it on this GPU). A lower output resolution or upscaler preset "
                        "needs less.\n",
                        static_cast<unsigned long long>(alloc_info.allocationSize >> 20));
            std::fflush(stdout);
        }
    }
    device_memory = Vulkan::Check(vk::ResultValue<vk::DeviceMemory>{alloc_result, device_memory});

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{memory_base};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    // port: through UploadCopies, which copies the guest data on the copy threads
    if (const Buffer* src_buffer = UploadCopies(arena, copies, total_size_bytes)) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    PortStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // port: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!PortToggle::Disabled(PortToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!PortToggle::Disabled(PortToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            PortCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        PortCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (const auto meta = texture_cache.GetMetaInfo(device_addr)) {
        if (meta->type == TextureCache::MetaType::HTile) {
            const auto value = texture_cache.HtileReadValue(device_addr);
            if (!value) {
                // Shader-written HTILE already lives in the arena; preserve its encoded words.
                return false;
            }
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, *value);
            return true;
        } else if (meta->type == TextureCache::MetaType::CMask) {
            const auto expanded_value = CmaskColorExpandedValue(meta->num_samples);
            if (!expanded_value) {
                LOG_WARNING(Render_Vulkan,
                            "Unsupported CMask sample count: address={:#x}, size={:#x}, "
                            "samples={}, owner={}:{}",
                            device_addr, size, meta->num_samples, meta->owner_id.index,
                            meta->owner_uid);
                return false;
            }
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, *expanded_value);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}",
                        magic_enum::enum_name(meta->type));
        }
    }
    // port: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !PortToggle::Disabled(PortToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // port: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
