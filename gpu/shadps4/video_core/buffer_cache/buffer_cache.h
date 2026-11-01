// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "port_copy.h"

#include <deque>
#include <map>
#include <tuple>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/enum.h"
#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
struct StagingBufferRef;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

enum class ObtainBufferFlags {
    None = 0,
    IsWritten = 1 << 0,
    IsTexelBuffer = 1 << 1,
    IgnoreStreamBuffer = 1 << 2,
    InvalidateTextureCache = 1 << 3,
};
DECLARE_ENUM_FLAG_OPERATORS(ObtainBufferFlags)

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // port: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (port: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    [[nodiscard]] const Buffer* GetNullIndexBuffer() const noexcept {
        return null_index_buffer.get();
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// port: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);
    /// Unmark a region as modified from the GPU.
    void UnmarkRegionAsGpuModified(VAddr device_addr, u64 size, bool is_write = false);

    /// Port: CPU writes to the pages of these ranges are synchronized immediately (THEORDER_SUN_READBACK_SYNC=0: off).
    void KeepPageSynchronized(VAddr device_addr, u64 size, u64 pgm_hash);
    /// Copies the registered ranges to host memory; a CPU write to their pages gets the newest completed copy.
    void RecordSunCopies();

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool async = false,
                    bool assume_locks = false);

    /// Flushes GPU modified ranges of the uncovered part of the edge pages of an image.
    void ReadEdgeImagePages(const Image& image);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(
        VAddr device_addr, u32 size, ObtainBufferFlags flags = ObtainBufferFlags::None);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Mark region as modified from the GPU
    void MarkRegionAsGpuModified(VAddr addr, size_t size);

    /// Synchronizes DMA buffers and returns whether their shader reads need a barrier.
    [[nodiscard]] bool SynchronizeDmaBuffers();
    void MarkDmaBuffersAccessed(vk::PipelineStageFlags2 stage);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// port: records the copy back to guest memory of every CPU-read buffer the GPU wrote since
    /// the last call (deferred like image readbacks; published when the GPU is done).
    void QueueReadbacks();

    /// port: a copy-back recorded by QueueReadbacks reached guest memory (any thread; applied by
    /// the next QueueReadbacks, see SettleReadbacks).
    void NoteReadbackPublished(VAddr address, u64 size, u64 token);

private:
    /// port: a small written binding: read-watched until the CPU reads it, then copied back
    /// after each GPU write (relaxed readbacks; The Order's per-tile depth buffer that sets
    /// the aim focus and the shadow cascades is read by the CPU every frame).
    void NoteSmallWrite(VAddr device_addr, u32 size);
    /// port: after a CPU read fault downloaded `device_addr`: its watched range becomes a
    /// readback buffer.
    void PromoteReadback(VAddr device_addr);
    /// port: a CPU write to a watched range: not a pure GPU output, never watched again (its
    /// pages flipped protection twice a frame: ~5% of the frame in the rooftop scene).
    void UnwatchCpuWritten(VAddr device_addr, u64 size);
    /// port: the GPU wrote [address, address + size): its bytes are newer than guest memory.
    void NoteGpuWrite(VAddr address, u64 size);
    /// port: published copy-backs not overtaken by a newer GPU write make their guest bytes
    /// current again; pages left with no GPU-only bytes stop being GPU-modified, so a CPU write
    /// beside GPU data there (The Order's command buffers hold shader-written blocks next to the
    /// packets and fences the CPU writes) no longer waits for the GPU. GPU thread, pipe drained.
    void SettleReadbacks();
    std::map<VAddr, std::pair<u64, u64>> settle_tokens; ///< under gpu_ranges_mutex: size, token
    u64 settle_counter = 0;
    std::mutex settled_mutex;
    std::vector<std::tuple<VAddr, u64, u64>> settled; ///< published: address, size, token
    static constexpr u32 ReadWatchMaxBytes = 64 * 1024;
    std::mutex read_watch_mutex;
    std::map<VAddr, u32> read_watched;                ///< protected for reads until read
    std::unordered_set<VAddr> never_watched;          ///< written by the CPU (UnwatchCpuWritten)
    std::unordered_map<VAddr, u32> readback_buffers;  ///< copied back after GPU writes
    std::vector<VAddr> readback_dirty;                ///< written since QueueReadbacks
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);


    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size, bool is_write,
                        bool async);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// port: batched small copies on the copy threads (PortCopy::QueueCopy).
    static void RunGuestCopy(const PortCopy::Item& item);
    void SmallGuestCopy(const PortCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // port: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    std::unique_ptr<Buffer> null_index_buffer;
    /// port: guarded by gpu_ranges_mutex: written on the draw recording thread, subtracted by
    /// downloads on the GPU thread, read by guest write faults (TouchesGpuBytes).
    RangeSet gpu_modified_ranges;
    std::mutex gpu_ranges_mutex;
    /// port: whether [device_addr, device_addr + size) holds bytes a GPU binding wrote.
    bool TouchesGpuBytes(VAddr device_addr, u64 size);

    bool SynchronizedPage(VAddr addr, u64 size);
    std::mutex synchronized_mutex;
    RangeSet synchronized_ranges;
    bool DeliverSunCopies();
    struct SunCopy {
        VAddr addr;
        u64 size;
        u64 staging_offset;
    };
    struct SunBatch {
        u64 tick{};
        Vulkan::StagingBufferRef staging{};
        std::vector<SunCopy> items;
    };
    std::deque<SunBatch> sun_batches;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 arena_memory_type_bits{}; ///< port: the types the arena accepts (fallback)
    /// port: VRAM kept for the arena when the GPU has no other room (discrete GPUs).
    vk::DeviceMemory reserve_memory{};
    u64 reserve_size{};
    u64 reserve_used{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
