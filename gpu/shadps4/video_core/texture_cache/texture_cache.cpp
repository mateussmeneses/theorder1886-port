// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <algorithm>
#include <limits>
#include <xxhash.h>
#include "port_process_memory.h"
#include "video_core/amdgpu/liverpool.h"

#include "port_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "video_core/texture_cache/aliasing.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u32 MAX_IMAGES = std::numeric_limits<u16>::max();
static constexpr u32 MAX_IMAGE_VIEWS = std::numeric_limits<u16>::max();
static constexpr u32 MAX_SAMPLERS = std::numeric_limits<u16>::max();

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& page_manager_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, page_manager{page_manager_}, slot_images{MAX_IMAGES},
      slot_image_views{MAX_IMAGE_VIEWS}, slot_samplers{MAX_SAMPLERS},
      blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    std::printf("GPU: deferred image readback %s (64 copies, 64 MiB pending limit)%s\n",
                deferred_readbacks ? "enabled" : "disabled",
                stale_readbacks ? "; CPU reads use the previous frame's copy" : "");

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    const s64 min_pressure_floor =
        std::clamp<s64>(device_local_memory / 4, 256_MB, DEFAULT_PRESSURE_GC_MEMORY);
    const s64 min_critical_floor =
        std::clamp<s64>(device_local_memory / 2, 512_MB, DEFAULT_CRITICAL_GC_MEMORY);
    pressure_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      min_pressure_floor));
    critical_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      min_critical_floor));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() {
    if (deferred_readbacks) {
        scheduler.Finish();
        std::scoped_lock lock{readback_mutex};
        RetireReadbacks();
    }
}

template <bool track>
void TextureCache::WatchReadback(VAddr address, u64 size) {
    constexpr PageOp op = track ? PageOp::Track : PageOp::Untrack;
    if (stale_readbacks) {
        page_manager.UpdatePageWatchers(address, size, op, PageOp::None);
    } else {
        page_manager.UpdatePageWatchers(address, size, PageOp::None, op);
    }
}

namespace {
constexpr u64 GuardLine = 64;
constexpr u64 MaxGuardSize = 64 * 1024; // 64 KiB fence structures only

bool ReadbackGuardEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("THEORDER_READBACK_GUARD");
        const bool on = !(env && env[0] == '0');
        if (!on) {
            std::printf("Readback guard: off (THEORDER_READBACK_GUARD=0)\n");
            std::fflush(stdout);
        }
        return on;
    }();
    return enabled;
}
std::atomic<u32> g_guard_lines{0};
std::atomic<u64> g_guard_bytes{0};

void LandGuarded(VAddr address, const u8* gpu_data, u64 size, const std::vector<u8>& before) {
    if (before.empty() || size > MaxGuardSize || !ReadbackGuardEnabled()) {
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(address), gpu_data, size);
        return;
    }
    std::vector<u8> pre(size);
    Core::Memory::Instance()->CopySparseMemory(address, pre.data(), size);
    u64 cursor = 0;
    for (u64 line = 0; line < size;) {
        const u64 line_end = std::min(line + GuardLine, size);
        const u64 m = line_end - line;
        if (std::memcmp(pre.data() + line, before.data() + line, m) != 0 &&
            std::memcmp(pre.data() + line, gpu_data + line, m) != 0) {
            if (line > cursor) {
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(address + cursor),
                                                          gpu_data + cursor, line - cursor);
            }
            cursor = line_end;
            g_guard_lines.fetch_add(1, std::memory_order_relaxed);
            g_guard_bytes.fetch_add(m, std::memory_order_relaxed);
            static std::atomic<u32> logged{0};
            const u32 seen = logged.fetch_add(1, std::memory_order_relaxed);
            if (seen < 16 || (seen & (seen + 1)) == 0) {
                std::printf("Readback guard: kept %llu bytes at %#llx that the game wrote after the GPU\n",
                            static_cast<unsigned long long>(m),
                            static_cast<unsigned long long>(address + line));
                std::fflush(stdout);
            }
        }
        line = line_end;
    }
    if (size > cursor) {
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(address + cursor),
                                                  gpu_data + cursor, size - cursor);
    }
}
} // namespace

void TextureCache::RetireReadbacks() {
    pending_readbacks.Retire([this](u64 tick) { return scheduler.IsFree(tick); },
        [this](auto& entry) {
            if (entry.valid) {
                entry.payload.Invalidate();
                LandGuarded(entry.address, entry.payload.mapped, entry.size, entry.before);
                if (entry.watched) WatchReadback<false>(entry.address, entry.size);
                if (entry.token) {
                    buffer_cache.NoteReadbackPublished(entry.address, entry.size, entry.token);
                }
                ++readbacks_completed;
            }
            runtime.GetStagingPool().FreeDeferred(entry.payload);
        });
    has_pending_readbacks.store(pending_readbacks.Bytes() != 0, std::memory_order_release);
}

bool TextureCache::ResolveReadbacks(VAddr address, u64 size, bool assume_locks,
                                    ReadbackSource source) {
    if (!deferred_readbacks || !has_pending_readbacks.load(std::memory_order_acquire)) return false;
    {
        std::scoped_lock lock{readback_mutex};
        // Stale mode, byte rule: God of War III writes every frame to the page that holds a
        // 1x1 exposure copy, but not to its bytes. That write need not wait for the GPU: the
        // copy only stops protecting its page and is still published when the GPU is done.
        if (stale_readbacks && source == ReadbackSource::CpuWrite) {
            readbacks_released += pending_readbacks.ReleaseOutside(address, size, 64_KB,
                [this](auto& entry) { WatchReadback<false>(entry.address, entry.size); });
        }
        if (!pending_readbacks.RequiredTick(address, size)) return false;
    }
    const auto resolve = [this, address, size, source] {
        const bool diag = Vulkan::WaitDiagnostics::Enabled();
        std::scoped_lock lock{readback_mutex};
        bool waited = false;
        u64 wait_ns = 0;
        if (diag && (source == ReadbackSource::CpuRead || source == ReadbackSource::CpuWrite)) {
            // Which copied image the CPU touched while its copy was pending.
            for (auto& image : readback_images) {
                if (image.copies && address < image.address + image.Bytes() && image.address < address + size) {
                    ++image.cpu_accesses;
                }
            }
        }
        PortStats::Timer readback_timer{PortStats::t_readback_wait};
        if (const u64 tick = pending_readbacks.RequiredTick(address, size)) {
            ++readbacks_waited;
            waited = !scheduler.IsFree(tick);
            const auto start = diag ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            scheduler.Wait(tick);
            if (diag) {
                wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start).count();
            }
            RetireReadbacks();
        }
        if (diag) readback_sources.Record(source, waited, wait_ns, size);
    };
    if (assume_locks) resolve();
    else liverpool->SendCommand<true>(resolve);
    return true;
}

void TextureCache::QueueBufferReadback(VAddr address, u64 size, Vulkan::StagingBufferRef staging,
                                       u64 token) {
    std::scoped_lock lock{readback_mutex};
    RetireReadbacks();
    if (!pending_readbacks.CanFit(size)) {
        ++readbacks_waited;
        scheduler.Finish();
        RetireReadbacks();
    }
    WatchReadback<true>(address, size);
    std::vector<u8> before;
    if (size <= MaxGuardSize && ReadbackGuardEnabled()) {
        try {
            before.resize(size);
            Core::Memory::Instance()->CopySparseMemory(address, before.data(), size);
        } catch (...) {
            before.clear();
        }
    }
    pending_readbacks.Push(address, size, scheduler.CurrentTick(), staging, token, std::move(before));
    has_pending_readbacks.store(true, std::memory_order_release);
    ++readbacks_queued;
}

void TextureCache::CancelReadbacks(VAddr address, u64 size) {
    if (!deferred_readbacks) return;
    std::scoped_lock lock{readback_mutex};
    pending_readbacks.Cancel(address, size, [this](auto& entry) {
        if (entry.watched) WatchReadback<false>(entry.address, entry.size);
        ++readbacks_canceled;
    });
}

bool TextureCache::IsTinyReadbackImage(const Image& image) const {
    if (!tiny_readbacks || image.info.guest_address == 0 || image.info.props.is_depth ||
        image.info.size.height != 1 || image.info.size.depth != 1 ||
        image.info.resources.layers != 1 || (image.info.props.is_tiled && image.info.size.width > 1)) {
        return false;
    }
    const u64 bytes = u64{image.info.size.width} * (image.info.num_bits / 8);
    return bytes != 0 && bytes <= 4096;
}

void TextureCache::QueueTinyReadback(ImageId image_id, const Image& image) {
    if (!IsTinyReadbackImage(image)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    tiny_downloads.emplace(image_id);
}

void TextureCache::CancelTinyReadbacks(VAddr address, u64 size) {
    if (!tiny_readbacks) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    std::erase_if(tiny_downloads, [this, address, size](ImageId id) {
        if (!slot_images.IsAllocated(id)) return true;
        const auto& image = slot_images[id];
        return image.info.guest_address < address + size && address < image.info.guest_address + 4096;
    });
}

void TextureCache::ProcessTinyReadbacks() {
    if (!tiny_readbacks) {
        return;
    }
    std::unordered_set<ImageId> ids;
    {
        std::unique_lock lk{download_images_mutex};
        if (tiny_downloads.empty()) {
            return;
        }
        ids.swap(tiny_downloads);
    }
    for (const ImageId image_id : ids) {
        if (!slot_images.IsAllocated(image_id)) {
            continue;
        }
        Image& image = slot_images[image_id];
        if (False(image.flags & ImageFlagBits::Registered) ||
            False(image.flags & ImageFlagBits::GpuModified) || !IsTinyReadbackImage(image)) {
            continue;
        }
        const u32 bytes_per_texel = image.info.num_bits / 8;
        const u32 bytes = image.info.size.width * bytes_per_texel;
        const VAddr address = image.info.guest_address;
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(
            address, bytes, ObtainBufferFlags::IsWritten | ObtainBufferFlags::IgnoreStreamBuffer);
        if (offset % bytes_per_texel != 0) {
            continue;
        }
        const vk::BufferImageCopy copy = {
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .imageOffset = {0, 0, 0},
            .imageExtent = {image.info.size.width, 1, 1},
        };
        runtime.DownloadImage(&image, buffer, std::span{&copy, 1});
        std::scoped_lock lock{tiny_mutex};
        if (tiny_ranges.size() < 64) {
            tiny_ranges.emplace_back(address, bytes);
        }
    }
}

// port: every copy is recorded first and the GPU is waited for once. A wait per image (as
// before) left the GPU mostly idle in God of War III's gameplay, at about 2 FPS.
void TextureCache::ProcessDownloadImages() {
    ProcessTinyReadbacks();
    if (deferred_readbacks) {
        // Upload workers must finish reading RAM before these pages become inaccessible.
        scheduler.WaitHostCopies();
        std::scoped_lock readback_lock{readback_mutex};
        RetireReadbacks();
        std::unique_lock lock{download_images_mutex};
        for (const ImageId image_id : download_images) {
            const auto& image = slot_images[image_id];
            if (False(image.flags & ImageFlagBits::GpuModified)) continue;
            const u64 size = u64{image.info.pitch} * image.info.size.height *
                image.info.size.depth * image.info.resources.layers * (image.info.num_bits / 8);
            if (!pending_readbacks.CanFit(size)) {
                ++readbacks_waited;
                scheduler.Finish();
                RetireReadbacks();
            }
            if (auto copy = RecordImageDownload(image_id)) {
                if (!pending_readbacks.CanFit(copy->size)) {
                    scheduler.Finish();
                    copy->staging.Invalidate();
                    Core::Memory::Instance()->TryWriteBacking(
                        std::bit_cast<u8*>(copy->guest_address), copy->staging.mapped, copy->size);
                    runtime.GetStagingPool().FreeDeferred(copy->staging);
                } else {
                    WatchReadback<true>(copy->guest_address, copy->size);
                    if (Vulkan::WaitDiagnostics::Enabled()) {
                        // Which images go back to RAM each frame (the report lists the busiest).
                        auto* slot = std::ranges::find(readback_images, image.info.guest_address,
                                                       &ReadbackImageStat::address);
                        if (slot == readback_images.end()) {
                            slot = std::ranges::find(readback_images, VAddr{0}, &ReadbackImageStat::address);
                        }
                        if (slot != readback_images.end()) {
                            *slot = {image.info.guest_address, image.info.size.width,
                                     image.info.size.height, image.info.num_bits, slot->copies + 1,
                                     slot->cpu_accesses, image.info.pixel_format};
                        }
                    }
                    pending_readbacks.Push(copy->guest_address, copy->size,
                        scheduler.CurrentTick(), copy->staging);
                    has_pending_readbacks.store(true, std::memory_order_release);
                    ++readbacks_queued;
                    readbacks_peak_bytes = std::max(readbacks_peak_bytes, pending_readbacks.Bytes());
                }
            }
        }
        download_images.clear();
        if (Vulkan::WaitDiagnostics::Enabled()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - readback_report >= std::chrono::seconds(5)) {
                std::printf("Readback profile: queued %llu, completed %llu, demand/budget waits %llu, canceled %llu, pending %.2f MiB, peak %.2f MiB\n",
                    static_cast<unsigned long long>(readbacks_queued),
                    static_cast<unsigned long long>(readbacks_completed),
                    static_cast<unsigned long long>(readbacks_waited),
                    static_cast<unsigned long long>(readbacks_canceled),
                    pending_readbacks.Bytes() / 1048576.0, readbacks_peak_bytes / 1048576.0);
                readbacks_queued = readbacks_completed = readbacks_waited = readbacks_canceled = 0;
                readbacks_peak_bytes = pending_readbacks.Bytes();
                readback_sources.Report(stdout);
                if (stale_readbacks) {
                    std::printf("Readback byte rule: %llu CPU writes went on without waiting\n",
                                static_cast<unsigned long long>(readbacks_released));
                    readbacks_released = 0;
                }
                std::ranges::sort(readback_images, std::greater{}, &ReadbackImageStat::copies);
                for (u32 i = 0; i < 4 && readback_images[i].copies; ++i) {
                    const auto& r = readback_images[i];
                    std::printf("Readback image %#llx: %ux%u, %u bits, %s, %u copies, %u CPU accesses "
                                "while pending\n",
                                static_cast<unsigned long long>(r.address), r.width, r.height, r.bits,
                                vk::to_string(r.format).c_str(), r.copies, r.cpu_accesses);
                }
                readback_images = {};
                readback_report = now;
            }
        }
        return;
    }
    std::unique_lock lk{download_images_mutex};
    boost::container::small_vector<PendingDownload, 16> pending;
    for (const ImageId image_id : download_images) {
        if (auto download = RecordImageDownload(image_id)) {
            pending.push_back(*download);
        }
    }
    download_images.clear();
    if (pending.empty()) {
        return;
    }
    scheduler.Finish();
    for (const PendingDownload& download : pending) {
        download.staging.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(download.guest_address),
                                                  download.staging.mapped, download.size);
        runtime.GetStagingPool().FreeDeferred(download.staging);
    }
}

std::optional<TextureCache::PendingDownload> TextureCache::RecordImageDownload(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return std::nullopt;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    return PendingDownload{image.info.guest_address, download_size, download};
}

void TextureCache::DownloadImageMemory(ImageId image_id, DownloadMemoryFlags flags) {
    Image& image = slot_images[image_id];
    ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                     ReadbackSource::ImageDownload);
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const auto image_addr = image.info.guest_address;
    const auto image_size = image.info.guest_size;
    const auto image_mips = image.info.resources.levels;
    const auto is_sync = True(flags & DownloadMemoryFlags::Sync);
    const auto is_priority = True(flags & DownloadMemoryFlags::Priority);
    const auto discard_buffer_cache = True(flags & DownloadMemoryFlags::DiscardBufferCache);
    ASSERT(!(is_sync && is_priority));

    u32 copy_size = 0;
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image_mips; ++mip) {
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> mip, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[mip];
        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
        copy_size += mip_size;
    }
    if (buffer_copies.empty()) {
        return;
    }
    ASSERT(copy_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(copy_size, MemoryType::HostCached, 16, !is_sync);

    tile_manager.TileImage(image, buffer_copies, download.buffer, download.offset);

    const auto write_data = [this, image_addr, copy_size, download, discard_buffer_cache, is_sync] {
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image_addr), download.mapped,
                                                  copy_size);
        if (discard_buffer_cache) {
            buffer_cache.UnmarkRegionAsGpuModified(image_addr, copy_size, true);
        }
        if (!is_sync) {
            runtime.GetStagingPool().FreeDeferred(download);
        }
    };

    if (is_priority) {
        scheduler.DeferPriorityOperation(std::move(write_data));
    } else if (is_sync) {
        scheduler.Finish();
        write_data();
    } else {
        scheduler.DeferOperation(std::move(write_data));
    }
}

void TextureCache::DiagDumpImage(ImageId image_id, const char* tag) {
    Image& image = slot_images[image_id];
    // Depth: the 32-bit float depth aspect only.
    const bool depth = image.info.props.is_depth;
    const bool float_depth = image.info.pixel_format == vk::Format::eD32Sfloat ||
                             image.info.pixel_format == vk::Format::eD32SfloatS8Uint;
    const u32 bytes_per_pixel = depth ? 4 : image.info.num_bits / 8;
    if ((depth && !float_depth) || image.info.props.is_block || bytes_per_pixel == 0) {
        return;
    }
    const u32 size = image.info.pitch * image.info.size.height * bytes_per_pixel;
    const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
    const vk::BufferImageCopy copy = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource = {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                             0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    scheduler.Finish();
    download.Invalidate();
    static int sequence = 0;
    const char* dir = std::getenv("THEORDER_DIAG_DIR");
    const char* dir_path = dir && *dir ? dir : "out/diag";
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%03d_%s_%llx_%ux%u_p%u_b%u_f%d.raw", dir_path, sequence++, tag,
                  static_cast<unsigned long long>(image.info.guest_address), image.info.size.width,
                  image.info.size.height, image.info.pitch, bytes_per_pixel,
                  static_cast<int>(image.info.pixel_format));
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(download.mapped, 1, size, f);
        std::fclose(f);
        std::printf("Diag: %s\n", path);
    } else {
        std::printf("Diag: cannot write %s\n", path);
    }
}

// port: images sharing guest memory (aliases), ported from cuesta4/shadPS4 (eltutz) commit
// 189ca3b6. A write to one is copied into the others before they are read, as on the PS4,
// where they are the same memory.
static bool Covers(const Extent3D& extent, const ImageInfo& info) {
    return extent.width == info.size.width && extent.height == info.size.height &&
           extent.depth == info.size.depth;
}

static bool CanAlias(const Image& lhs, const Image& rhs) {
    return GetAliasCopyExtent(lhs.info, rhs.info) || GetAliasCopyExtent(rhs.info, lhs.info);
}

template <typename Func>
void TextureCache::ForEachAlias(const Image& image, Func&& func) {
    const auto page = page_table.find(image.info.guest_address >> Traits::PAGE_BITS);
    if (page == nullptr) {
        return;
    }
    for (const BucketEntry& entry : page->entries) {
        const ImageId candidate_id = entry.id;
        Image& candidate = slot_images[candidate_id];
        if (&candidate != &image && candidate.info.guest_address == image.info.guest_address &&
            CanAlias(candidate, image)) {
            func(candidate_id, candidate);
        }
    }
}

void TextureCache::SynchronizeAlias(ImageId image_id) {
    Image& dst = slot_images[image_id];
    if (False(dst.flags & ImageFlagBits::Aliased)) {
        return;
    }
    const auto state_it = alias_states.find(dst.info.guest_address);
    if (state_it == alias_states.end()) {
        return;
    }
    AliasState& state = state_it.value();
    if (!IsLiveImage(state.backing, state.backing_uid)) {
        if (!IsLiveImage(state.writer, state.writer_uid)) {
            state.ResetAuthority();
            return;
        }
        state.backing = state.writer, state.backing_uid = state.writer_uid;
        state.writer = {}, state.writer_uid = 0;
    }
    if (state.writer == image_id && state.writer_uid == dst.image_uid) {
        return;
    }
    if (!CommitAliasWriter(state)) {
        state.ResetAuthority();
        return;
    }
    Image& current = slot_images[state.backing];
    const std::optional extent = GetAliasCopyExtent(current.info, dst.info);
    if (state.backing == image_id || current.alias_generation <= dst.alias_generation || !extent) {
        return;
    }
    CopyAlias(state.backing, image_id, *extent);
    dst.alias_generation = current.alias_generation;
    const std::optional reverse = GetAliasCopyExtent(dst.info, current.info);
    if (reverse && Covers(*reverse, current.info) && !Covers(*extent, dst.info)) {
        state.backing = image_id, state.backing_uid = dst.image_uid;
    }
}

bool TextureCache::CommitAliasWriter(AliasState& state) {
    if (!state.writer) {
        return true;
    }
    if (!IsLiveImage(state.writer, state.writer_uid) ||
        !IsLiveImage(state.backing, state.backing_uid)) {
        return false;
    }
    Image& writer = slot_images[state.writer];
    Image& backing = slot_images[state.backing];
    if (state.writer != state.backing && writer.alias_generation > backing.alias_generation) {
        const std::optional extent = GetAliasCopyExtent(writer.info, backing.info);
        if (!extent) {
            return false;
        }
        CopyAlias(state.writer, state.backing, *extent);
        backing.alias_generation = writer.alias_generation;
    }
    state.writer = {}, state.writer_uid = 0;
    return true;
}

void TextureCache::CopyAlias(ImageId src_id, ImageId dst_id, const Extent3D& extent) {
    Image& dst = slot_images[dst_id];
    runtime.CopyAliasImage(&slot_images[src_id], &dst, {extent.width, extent.height, extent.depth});
    dst.flags |= ImageFlagBits::GpuModified;
    dst.flags &= ~ImageFlagBits::Dirty;
}

void TextureCache::PublishAliasWrite(ImageId image_id) {
    Image& image = slot_images[image_id];
    image.alias_generation = ++alias_generation;
    if (False(image.flags & ImageFlagBits::Aliased)) {
        return;
    }
    std::scoped_lock lock{mutex};
    AliasState& state = alias_states[image.info.guest_address];
    if (IsLiveImage(state.backing, state.backing_uid) &&
        !CanAlias(image, slot_images[state.backing])) {
        state.ResetAuthority();
    }
    state.members = std::max(state.members, 2u);
    if (state.writer == image_id && state.writer_uid == image.image_uid) {
        return;
    }
    if (IsLiveImage(state.backing, state.backing_uid)) {
        const ImageInfo& backing = slot_images[state.backing].info;
        const std::optional extent = GetAliasCopyExtent(image.info, backing);
        if (extent && Covers(*extent, backing)) {
            state.backing = image_id, state.backing_uid = image.image_uid;
        } else {
            state.writer = image_id, state.writer_uid = image.image_uid;
        }
    } else {
        state.backing = image_id, state.backing_uid = image.image_uid;
    }
}

void TextureCache::InvalidateAlias(Image& image) {
    image.alias_generation = 0;
    if (True(image.flags & ImageFlagBits::Aliased)) {
        if (const auto it = alias_states.find(image.info.guest_address); it != alias_states.end()) {
            it.value().ResetAuthority();
        }
    }
}

static u64 MaybeDirtyHash(const Image& image) {
    // Mip 0 is sufficient to detect CPU modifications to a sub-page/two-page texture without
    // hashing the full mip tail (+33% bytes on mipmapped textures).
    const u64 size = image.info.mips_layout[0].size ? image.info.mips_layout[0].size
                                                    : image.info.guest_size;
    return XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.cpu_hash == 0) {
        // Initialize hash
        image.cpu_hash = MaybeDirtyHash(image);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    CancelTinyReadbacks(addr, size);
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(pages_start, pages_end - pages_start,
                         [&](ImageId image_id, Image&) { image_ids.push_back(image_id); });

    for (const auto image_id : image_ids) {
        Image& image = slot_images[image_id];
        std::scoped_lock lk{image.mutex};

        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            InvalidateAlias(image);
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    }
}

void TextureCache::NoteBackingWrite(VAddr address, u64 size) {
    std::scoped_lock lock{backing_writes_mutex};
    backing_writes.emplace_back(address, size);
    has_backing_writes.store(true, std::memory_order_release);
}

void TextureCache::ApplyBackingWrites() {
    if (!has_backing_writes.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> writes;
    {
        std::scoped_lock lock{backing_writes_mutex};
        writes.swap(backing_writes);
        has_backing_writes.store(false, std::memory_order_release);
    }
    // Readbacks and buffer downloads put GPU-written bytes in guest memory without a fault;
    // textures cached there kept the old texels (gow3_pc #138: streamed textures).
    for (const auto& [address, size] : writes) {
        ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
            // Images sharing memory with a GPU writer are synchronized by SynchronizeAlias.
            if (True(image.flags & (ImageFlagBits::GpuModified | ImageFlagBits::Aliased))) {
                return;
            }
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        });
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    // Partial buffer writes still need the image's untouched bytes in guest memory.
    ResolveReadbacks(address, max_size, true, ReadbackSource::GpuPartialWrite);
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        InvalidateAlias(image);
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::MarkAsMaybeGpuDirty(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    // port: only the flag (upstream also hashes the whole image here, once per use: ~14% of the
    // draw recording thread in the rooftop scene, 16 MiB render targets hashed every frame).
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        image.flags |= ImageFlagBits::MaybeGpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    CancelReadbacks(cpu_addr, size);
    CancelTinyReadbacks(cpu_addr, size);
    std::scoped_lock lock{mutex};
    SmallVector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    // port: recreate if either dimension is too small for the requested subresources.
    bool recreate = !cache_image.info.resources.Contains(requested_info.resources);

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        // port: take the larger count per dimension; the ordered max could keep the cached
        // image's extra mips and still drop a requested layer.
        new_info.resources = {
            .levels = std::max(requested_info.resources.levels, cache_image.info.resources.levels),
            .layers = std::max(requested_info.resources.layers, cache_image.info.resources.layers),
        };
        new_info.UpdateSize();
        const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        if (new_info.num_samples == 1 &&
            (new_info.resources.layers > cache_image.info.resources.layers ||
             new_info.resources.levels > cache_image.info.resources.levels)) {
            RefreshImage(new_image);
        }
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        if (!runtime.CopyColorAndDepth(&cache_image, &new_image)) {
            // A missing transfer is not a successful cache conversion. Keep the
            // only GPU copy of the old contents; let normal overlap handling retry.
            FreeImage(new_image_id);
            return {};
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id,
                                                           bool native_reads) {
    static constexpr u64 NUM_FRAMES_BEFORE_REMOVAL = 32;

    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NUM_FRAMES_BEFORE_REMOVAL;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.guest_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.guest_samples;
        // port (gow3_pc b646683): a partial-height integer view (e.g. a 1920x800 letterboxed or
        // screen-effect pass reading a 1920x1080 target with integer coordinates) shares the same
        // pitch and tile mode: reuse the full-height cached image through a format-compatible
        // view instead of treating the height difference as pool reuse and allocating a new image.
        // - The request's width can be the pitch: at 3440x1440 the reads are 3456 wide (pitch
        //   3456) against a 3440-wide frame buffer; the extra columns are padding.
        const auto& a = image_info;
        const auto& b = cache_image.info;
        const auto is_integer = [](vk::Format format) {
            switch (format) {
            case vk::Format::eR8Uint:
            case vk::Format::eR8Sint:
            case vk::Format::eR8G8Uint:
            case vk::Format::eR8G8Sint:
            case vk::Format::eR8G8B8A8Uint:
            case vk::Format::eR8G8B8A8Sint:
            case vk::Format::eB8G8R8A8Uint:
            case vk::Format::eB8G8R8A8Sint:
            case vk::Format::eA2B10G10R10UintPack32:
            case vk::Format::eA2B10G10R10SintPack32:
            case vk::Format::eR16Uint:
            case vk::Format::eR16Sint:
            case vk::Format::eR16G16Uint:
            case vk::Format::eR16G16Sint:
            case vk::Format::eR16G16B16A16Uint:
            case vk::Format::eR16G16B16A16Sint:
            case vk::Format::eR32Uint:
            case vk::Format::eR32Sint:
            case vk::Format::eR32G32Uint:
            case vk::Format::eR32G32Sint:
            case vk::Format::eR32G32B32Uint:
            case vk::Format::eR32G32B32Sint:
            case vk::Format::eR32G32B32A32Uint:
            case vk::Format::eR32G32B32A32Sint:
                return true;
            default:
                return false;
            }
        };
        static const bool partial_views_enabled = [] {
            const char* value = std::getenv("THEORDER_PARTIAL_HEIGHT_VIEWS");
            return !value || value[0] != '0';
        }();
        if (partial_views_enabled && !PortToggle::Disabled(PortToggle::PartialHeightViews) &&
            (binding == BindingType::Texture || binding == BindingType::Storage) && native_reads &&
            a.pixel_format != b.pixel_format && is_integer(a.pixel_format) &&
            lhs_block_size == rhs_block_size && a.pitch == b.pitch && a.tile_mode == b.tile_mode &&
            a.type == b.type && a.size.width <= b.pitch && a.size.height < b.size.height &&
            a.size.depth == 1 && b.size.depth == 1 && a.resources.levels == 1 &&
            a.resources.layers == 1 && b.resources.levels == 1 && b.resources.layers == 1 &&
            !a.props.is_block && !b.props.is_block && !a.props.is_depth && !b.props.is_depth &&
            IsVulkanFormatCompatible(b.pixel_format, a.pixel_format)) {
            return {cache_image_id, -1, -1};
        }
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // port: reuse identical single-level surfaces when only tiler padding differs.
        const bool padding_only =
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1 &&
            (image_info.props.is_pow2 != cache_image.info.props.is_pow2 ||
             image_info.type == cache_image.info.type);

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        // port: expand if either dimension is missing, regardless of mip ordering.
        if (image_info.type == cache_image.info.type &&
            !cache_image.info.resources.Contains(image_info.resources)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // port: what is left is a stale T# (garbage depth and pitch over a real texture's
        // address, The Order's particle lighting slots): a chance overlap like the ones above,
        // not a reason to stop the game. The details are logged a few times.
        static std::atomic<u32> unresolvable_logged{0};
        if (unresolvable_logged.fetch_add(1, std::memory_order_relaxed) >= 4) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }
        return {merged_image_id, -1, -1};
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    new_image.flags |= src_image.flags & ImageFlagBits::GpuModified;
    FreeImage(image_id);

    return new_image_id;
}

// port: clamp views to the resolved image to prevent invalid views and barrier asserts.
static void ClampViewRange(TextureCache::ImageDesc& desc, const Image& image) {
    auto& range = desc.view_info.range;
    const auto requested = range;
    if (!range.ClampTo(image.info.resources)) {
        return;
    }
    static std::atomic<u32> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::printf("Texture cache: view mips %u+%u layers %u+%u clamped to image %#llx "
                    "(%u mips, %u layers, %s; requested %s)\n",
                    requested.base.level, requested.extent.levels, requested.base.layer,
                    requested.extent.layers, (unsigned long long)image.info.guest_address,
                    std::max<u32>(image.info.resources.levels, 1u),
                    std::max<u32>(image.info.resources.layers, 1u),
                    vk::to_string(image.info.pixel_format).c_str(),
                    vk::to_string(desc.info.pixel_format).c_str());
    }
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63 ^ u64(desc.needs_native) << 62;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.needs_native == desc.needs_native &&
        cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !PortToggle::Disabled(PortToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        // port: cached images must also bound descriptor mip and layer ranges.
        ClampViewRange(desc, image);
        return cached.image_id;
    }

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id, desc.needs_native);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (!image_resolved.info.resources.Contains(info.resources)) {
            // port (SpartanPort c22329f, shadPS4 #5524): every dimension must fit; the old
            // lexicographic `<` let an image missing mips or layers through to an invalid view.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.Insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    // port: overlap offsets can leave the view past the resolved image.
    ClampViewRange(desc, image);

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .needs_native = desc.needs_native,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    SmallVector<ImageId, 4> image_ids;
    ForEachImageWithAddress(address, [&](ImageId image_id, Image& image) {
        if (ensure_valid && !image.HasGpuValidData()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        Image& image = slot_images[image_ids[0]];
        TouchImage(image);
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                TouchImage(image);
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        } else {
            QueueTinyReadback(image_id, image);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (desc.type == BindingType::Storage) {
        PublishAliasWrite(image_id);
    }
    if (memo && !PortToggle::Disabled(PortToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

void TextureCache::RegisterMeta(VAddr address, MetaDataInfo info) {
    const auto it = surface_metas.find(address);
    if (it == surface_metas.end()) {
        surface_metas.emplace(address, info);
        return;
    }

    auto& registered = it->second;
    if (registered.type != info.type || registered.owner_uid != info.owner_uid) {
        registered = info;
        return;
    }

    // Preserve the current clear state when the same render target is rebound.
    registered.owner_id = info.owner_id;
    registered.num_samples = info.num_samples;
    registered.htile_stencil = info.htile_stencil;
}

void TextureCache::UnregisterMeta(VAddr address, u64 owner_uid) {
    const auto it = surface_metas.find(address);
    if (it != surface_metas.end() && it->second.owner_uid == owner_uid) {
        surface_metas.erase(address);
    }
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    } else {
        QueueTinyReadback(image_id, image);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);
    PublishAliasWrite(image_id);

    // Register meta data for this color buffer
    const VAddr cmask_addr = desc.info.meta_info.cmask_addr;
    if (image.info.meta_info.cmask_addr != cmask_addr) {
        UnregisterMeta(image.info.meta_info.cmask_addr, image.image_uid);
        image.info.meta_info.cmask_addr = cmask_addr;
    }
    if (cmask_addr) {
        RegisterMeta(cmask_addr, MetaDataInfo{
                                     .type = MetaType::CMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    const VAddr fmask_addr = desc.info.meta_info.fmask_addr;
    if (image.info.meta_info.fmask_addr != fmask_addr) {
        UnregisterMeta(image.info.meta_info.fmask_addr, image.image_uid);
        image.info.meta_info.fmask_addr = fmask_addr;
    }
    if (fmask_addr) {
        RegisterMeta(fmask_addr, MetaDataInfo{
                                     .type = MetaType::FMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    const VAddr htile_addr = desc.info.meta_info.htile_addr;
    if (image.info.meta_info.htile_addr != htile_addr) {
        UnregisterMeta(image.info.meta_info.htile_addr, image.image_uid);
        image.info.meta_info.htile_addr = htile_addr;
        if (htile_addr) {
            std::printf("HTILE meta: %#llx for depth %#llx %ux%u\n",
                        static_cast<unsigned long long>(htile_addr),
                        static_cast<unsigned long long>(image.info.guest_address),
                        image.info.size.width, image.info.size.height);
        }
    }
    if (htile_addr) {
        RegisterMeta(htile_addr, MetaDataInfo{
                                     .type = MetaType::HTile,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                     .clear_mask = image.info.meta_info.htile_clear_mask,
                                     .htile_stencil = desc.info.props.has_stencil != 0,
                                 });
    }

    AssociateStencil(image_id, desc.info);

    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::AssociateStencil(ImageId depth_id, const ImageInfo& depth_info) {
    if (depth_info.stencil_addr == 0 || depth_info.stencil_addr == depth_info.guest_address) {
        return;
    }
    ImageId stencil_id{};
    ForEachImageInRegion(
        depth_info.stencil_addr, depth_info.stencil_size, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address != depth_info.stencil_addr) {
                return;
            }
            if (image.info.pixel_format == vk::Format::eUndefined ||
                Vulkan::LiverpoolToVK::IsFormatStencilCompatible(image.info.pixel_format)) {
                stencil_id = image_id;
            }
        });
    if (!stencil_id) {
        ImageInfo info{};
        info.guest_address = depth_info.stencil_addr;
        info.guest_size = depth_info.stencil_size;
        info.size = depth_info.size;
        stencil_id = slot_images.Insert(instance, runtime, slot_image_views, info);
        RegisterImage(stencil_id);
    }
    Image& stencil_image = slot_images[stencil_id];
    TouchImage(stencil_image);
    stencil_image.AssociateDepth(depth_id, slot_images[depth_id].image_uid);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.guest_samples > 1) {
        return;
    }
    ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                     ReadbackSource::ImageRefresh);

    PortStats::Timer timer{PortStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.cpu_hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.cpu_hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    // port: THEORDER_FRAME_STATS: the images refreshed most (each refresh hashes or uploads all
    // of the guest copy), by bytes, every 5 s.
    static const bool refresh_stats = std::getenv("THEORDER_FRAME_STATS") != nullptr;
    if (refresh_stats) {
        struct Refreshes {
            u64 count = 0, bytes = 0;
            u32 width = 0, height = 0;
            bool gpu_modified = false, cpu_dirty = false, gpu_dirty = false;
        };
        static std::unordered_map<VAddr, Refreshes> refreshes;
        static auto since = std::chrono::steady_clock::now();
        auto& entry = refreshes[image.info.guest_address];
        ++entry.count;
        entry.bytes += image.info.guest_size;
        entry.width = image.info.size.width;
        entry.height = image.info.size.height;
        entry.gpu_modified = is_gpu_modified;
        entry.cpu_dirty = True(image.flags & ImageFlagBits::CpuDirty);
        entry.gpu_dirty = is_gpu_dirty;
        const auto now = std::chrono::steady_clock::now();
        if (const double seconds = std::chrono::duration<double>(now - since).count(); seconds >= 5) {
            std::vector<std::pair<VAddr, Refreshes>> top(refreshes.begin(), refreshes.end());
            std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
            std::printf("Image refreshes by bytes, per second:");
            for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
                const auto& [address, n] = top[i];
                std::printf(" %#llx %ux%u: %.0f, %.1f MB%s%s%s;", static_cast<unsigned long long>(address),
                            n.width, n.height, n.count / seconds, n.bytes / seconds / 1e6,
                            n.gpu_modified ? ", GPU-modified" : "", n.cpu_dirty ? ", CPU-dirty" : "",
                            n.gpu_dirty ? ", GPU-dirty" : "");
            }
            std::printf("\n");
            refreshes.clear();
            since = now;
        }
    }

    PortStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    SmallVector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    // port: a copy that reads past the source buffer faults the GPU (an NVIDIA driver crash or
    // a device that never signals again). A failed detile leaves a too-large image (512x512 with
    // 337 layers) reading 20 GB from the 4 GB guest buffer: skip what does not fit.
    const u32 block = image.info.props.is_block ? 4u : 1u;
    const u64 texel_bytes = std::max(image.info.num_bits / 8u, 1u);
    const auto too_large = [&](const vk::BufferImageCopy& copy) {
        const u64 row = Common::DivCeil(copy.bufferRowLength ? copy.bufferRowLength
                                                             : copy.imageExtent.width, block);
        const u64 rows = Common::DivCeil(copy.bufferImageHeight ? copy.bufferImageHeight
                                                                : copy.imageExtent.height, block);
        const u64 slices = u64(copy.imageSubresource.layerCount) * copy.imageExtent.depth;
        const u64 end = copy.bufferOffset +
                        ((slices - 1) * rows * row +
                         (Common::DivCeil(copy.imageExtent.height, block) - 1) * row +
                         Common::DivCeil(copy.imageExtent.width, block)) *
                            texel_bytes;
        if (end <= buffer->SizeBytes()) {
            return false;
        }
        static u32 warned = 0;
        if (warned < 8) {
            ++warned;
            LOG_ERROR(Render, "Skipping upload of {:#x} {}x{}x{} layers {} mip {}: reads {} bytes "
                      "of a {} byte buffer",
                      image.info.guest_address, image.info.size.width, image.info.size.height,
                      image.info.size.depth, copy.imageSubresource.layerCount,
                      copy.imageSubresource.mipLevel, end, buffer->SizeBytes());
        }
        return true;
    };
    image_copies.erase(std::remove_if(image_copies.begin(), image_copies.end(), too_large),
                       image_copies.end());
    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sharp,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // The S# holds an index into a mutable guest table. Snapshot the effective custom color
    // once so the cache key and the Vulkan sampler always describe the same RGBA value.
    std::optional<std::array<float, 4>> custom_border_color;
    if (sharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
        instance.IsCustomBorderColorSupported()) {
        const auto colors = border_color_base.Address<const std::array<float, 4>*>();
        custom_border_color = colors[sharp.border_color_ptr.Value()];
    }
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    u64 hash = HashCombine(HashCombine(XXH3_64bits(&sharp, sizeof(sharp)), is_depth),
                           u64(std::bit_cast<u32>(extra_lod_bias)));
    if (custom_border_color) {
        hash = HashCombine(hash, XXH3_64bits(custom_border_color->data(),
                                          sizeof(*custom_border_color)));
    }

    // port: the draw-preparation workers ask for samplers concurrently.
    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash);
    if (new_sampler) {
        it->second =
            slot_samplers.Insert(instance, sharp, custom_border_color, is_depth, extra_lod_bias);
        Sampler& sampler = slot_samplers[it->second];
        sampler.hash = hash;
        sampler_lru_cache.Insert(sampler, gc_tick);
    }

    Sampler& sampler = slot_samplers[it->second];
    sampler_lru_cache.Touch(sampler, gc_tick);
    return sampler.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    PortStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    image.memory_usage_counter = &total_used_memory;
    total_used_memory += Common::AlignUp(image.SizeBytes(), 1024);
    ++image_count;
    image_bytes += image.info.guest_size;
    image_lru_cache.Insert(image, gc_tick);
    image.lru_touched_tick = gc_tick;

    AliasState* state{};
    u32 aliases{};
    bool inserted{};
    ForEachAlias(image, [&](ImageId candidate_id, Image& candidate) {
        if (!state) {
            auto result = alias_states.try_emplace(image.info.guest_address);
            state = std::addressof(result.first.value());
            inserted = result.second;
        }
        ++aliases;
        candidate.flags |= ImageFlagBits::Aliased;
        if (!IsLiveImage(state->backing, state->backing_uid) ||
            slot_images[state->backing].alias_generation < candidate.alias_generation) {
            state->backing = candidate_id, state->backing_uid = candidate.image_uid;
        }
    });
    if (state) {
        image.flags |= ImageFlagBits::Aliased;
        state->members = inserted ? aliases + 1 : state->members + 1;
    }

    const auto& info = image.info;
    ASSERT_MSG((info.guest_address & 0xff) == 0, "Trying to register an unaligned image");
    ForEachPage(info.guest_address, info.guest_size, [this, image_id, info](u64 page) {
        page_table[page].entries.emplace_back(BucketEntry{
            .key = u32(info.guest_address >> 8),
            .size = info.guest_size,
            .id = image_id,
        });
    });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    image.memory_usage_counter = nullptr;
    image_lru_cache.Free(image);
    total_used_memory -= Common::AlignUp(image.SizeBytes(), 1024);
    --image_count;
    image_bytes -= image.info.guest_size;
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        ASSERT_MSG(page_it, "Unregistering unregistered page={:#x}", page << Traits::PAGE_BITS);
        auto& entries = page_it->entries;
        const auto vector_it = std::ranges::find(entries, image_id, &BucketEntry::id);
        ASSERT_MSG(vector_it != entries.end(), "Unregistering unregistered image in page={:#x}",
                   page << Traits::PAGE_BITS);
        entries.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    std::scoped_lock lk{image.mutex};
    if (image.IsUntracked()) {
        page_manager.UpdatePageWatchers(image_begin, image.info.guest_size, PageOp::Track);
    } else {
        if (image_begin < image.track_addr) {
            page_manager.UpdatePageWatchers(image_begin, image.track_addr - image_begin,
                                            PageOp::Track);
        }
        if (image.track_addr_end < image_end) {
            page_manager.UpdatePageWatchers(image.track_addr_end, image_end - image.track_addr_end,
                                            PageOp::Track);
        }
    }
    image.track_addr = image_begin;
    image.track_addr_end = image_end;
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (image.IsUntracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    if (size != 0) {
        page_manager.UpdatePageWatchers(addr, size, PageOp::Untrack);
    }
    image.track_addr = 0;
    image.track_addr_end = 0;
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (image.IsUntracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = page_manager.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    page_manager.UpdatePageWatchers(image_begin, size, PageOp::Untrack);

    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image.IsUntracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = page_manager.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    page_manager.UpdatePageWatchers(addr, size, PageOp::Untrack);
}

// port: once a minute, what the game holds: VRAM, the texture cache, the old GPU-written
// tiled images the collector cannot free, and the process's own memory. A log ending in a
// crash after hours of play then shows what was growing.
void TextureCache::ReportMemory() {
    const auto now = std::chrono::steady_clock::now();
    if (now - memory_report_time < std::chrono::seconds(60)) {
        return;
    }
    const bool first = memory_report_time == std::chrono::steady_clock::time_point{};
    memory_report_time = now;
    if (first) {
        return; // a minute after the first submission, not at once
    }
    // Unused for a minute: the tick at the start of the second a minute ago.
    const u64 minute_tick = gc_tick_at_second[(gc_second - 60) % gc_tick_at_second.size()];
    u64 idle = 0, idle_bytes = 0, kept = 0, kept_bytes = 0;
    {
        std::scoped_lock lock{mutex};
        image_lru_cache.ForEachItemBelow(minute_tick, [&](Image& image) {
            ++idle;
            idle_bytes += image.info.guest_size;
            if (image.info.IsTiled() && image.CanBeDownloaded()) {
                ++kept;
                kept_bytes += image.info.guest_size;
            }
        });
    }
    const s64 vram = instance.CanReportMemoryUsage() ? s64(instance.GetDeviceMemoryUsage() >> 20) : -1;
    const auto process = PortProcessMemory::Get();
    std::printf("Memory: GPU adapter %lld MiB; textures %llu (%llu MiB), unused for a minute %llu "
                "(%llu MiB), of them GPU-written tiled %llu (%llu MiB); process %lld MiB "
                "committed, %lld MiB in RAM (-1 = unavailable)\n",
                (long long)vram, (unsigned long long)image_count,
                (unsigned long long)(image_bytes >> 20), (unsigned long long)idle,
                (unsigned long long)(idle_bytes >> 20), (unsigned long long)kept,
                (unsigned long long)(kept_bytes >> 20),
                process.private_available ? (long long)(process.private_bytes >> 20) : -1ll,
                process.resident_available ? (long long)(process.working_set >> 20) : -1ll);
    std::fflush(stdout);
}

void TextureCache::GarbageCollectImages() {
    PortStats::Timer gc_timer{PortStats::t_gc};
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // port: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // THEORDER_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("THEORDER_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        if (instance.IsIntegrated() || forced_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        } else {
            // Discrete GPUs: adapt trigger to driver's current budget or cap at 3072 MB
            // to prevent textures from previous areas from occupying 4+ GB of VRAM indefinitely.
            const u64 budget = instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                const u64 process_cap = 3072_MB;
                trigger_gc_memory = std::min<u64>(budget / 10 * 6, process_cap);
                pressure_gc_memory = std::min<u64>(budget / 100 * 80, process_cap + 512_MB);
                critical_gc_memory = std::min<u64>(budget / 100 * 90, process_cap + 1024_MB);
            }
        }
    }
    // port: gc_tick (one per guest submission, hundreds a second) at each of the last 64
    // seconds, for ages in seconds. The usage compared is all of our VRAM, not only images: on a
    // discrete GPU it stays over the trigger, where an age of 16 ticks (~3 frames) would evict
    // every texture briefly off screen and upload it again. Below the pressure mark an image goes
    // once unused for THEORDER_GC_IDLE_SECONDS (10): the textures of an area left behind.
    // (From port 0.3, deadinside28/bloodborne_pc 4d4d208.)
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    ReportMemory();
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    static const u64 idle_seconds = [] {
        const char* env = std::getenv("THEORDER_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 10, 1, 63);
    }();
    // Under pressure an image goes once unused for THEORDER_GC_PRESSURE_IDLE_SECONDS (5), over the
    // critical mark for 1 s first, and only then by submissions (a fraction of a second). On
    // smaller cards the usage stayed over the pressure mark, so textures off screen for a moment
    // were evicted and uploaded again over the bus: the frame rate fell the longer the game
    // ran. (port 0.5, deadinside28/bloodborne_pc c7e3e55, via parsifal295's PR #63.)
    static const u64 pressure_idle_seconds = [] {
        const char* env = std::getenv("THEORDER_GC_PRESSURE_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 5, 0, 63);
    }();
    // The tick at the start of that second (an older one where no submission came then).
    const auto tick_seconds_ago = [&](u64 seconds) {
        return gc_tick_at_second[(second - seconds) % gc_tick_at_second.size()];
    };
    const u64 idle_tick = tick_seconds_ago(idle_seconds);
    // Last used a minute ago or more (lru_touched_tick: real use, not the collector's Touch).
    const u64 minute_tick = gc_tick_at_second[(second - 60) % gc_tick_at_second.size()];
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;

    // emergency: still over the critical mark after evicting what was unused for a second.
    const auto configure = [&](bool allow_aggressive, bool emergency = false) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        const u64 ticks_to_destroy = std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        } else if (!emergency) {
            below_tick = std::min(below_tick, tick_seconds_ago(aggresive ? 1 : pressure_idle_seconds));
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };
    const auto clean_up = [&](Image& image) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        const auto image_id = slot_images.GetSlotId(image);
        ResolveReadbacks(image.info.guest_address, image.info.guest_size, true,
                         ReadbackSource::CacheCleanup);
        const bool download = image.HasGpuValidData();
        // port: images that cannot go now (GPU-written ones that cannot be written back:
        // multisampled, with stencil, or in a format the copy cannot reinterpret) neither use up
        // the deletions nor stay first in line. The oldest ten being such render targets stopped
        // the collector for good, and VRAM grew with every area visited up to the pressure mark
        // (~10 GB on 16 GB; a 6 GB card stayed over its critical mark with 0 evictions until
        // ErrorOutOfDeviceMemory). Since shadPS4 0.20.1 tiled images are written back too
        // (CanBeDownloaded); before, every tiled GPU-written image was kept here.
        // Past the critical mark, one unused for a minute goes all the same, unwritten (render
        // targets of an area left behind): out of VRAM the game would crash instead.
        const bool stuck = download && !image.CanBeDownloaded();
        const bool drop = stuck && aggresive && image.lru_touched_tick < minute_tick;
        if (!drop && (stuck || (download && !pressured))) {
            if (stuck) {
                ++gc_kept;
                gc_kept_bytes += image.info.guest_size;
            }
            image_lru_cache.Touch(image, gc_tick);
            return false;
        }
        --num_deletions;
        if (drop) {
            ++gc_dropped;
        } else if (download) {
            // port: a buffer bound for writing overlapped the image since its last use: that
            // write is the newest data there, and it stays in the buffer cache. Upstream reads it
            // back (a GPU wait) and compares hashes; here the image is dropped unwritten.
            if (False(image.flags & ImageFlagBits::MaybeGpuDirty)) {
                // port: synchronously, while the image still protects its pages. A deferred
                // write-back landed after FreeImage had unprotected them, over whatever the game
                // had meanwhile stored there (e.g. its heap after unloading an area).
                DownloadImageMemory(image_id, DownloadMemoryFlags::Sync |
                                                  DownloadMemoryFlags::DiscardBufferCache);
                ++gc_downloads;
            }
        }
        ++gc_evictions;
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    gc_kept = gc_kept_bytes = 0;
    image_lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        gc_kept = gc_kept_bytes = 0;
        image_lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    if (total_used_memory >= critical_gc_memory) {
        // Still over it: what was used a moment ago goes too (out of memory otherwise).
        configure(true, true);
        image_lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    // port: evictions under memory pressure, at most every 5 s (THEORDER_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            // port: expose tiled or unresolved images that the latest pass cannot evict.
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report; "
                        "%llu old GPU-written tiled images kept (%llu MiB), %llu dropped\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads,
                        (unsigned long long)gc_kept, (unsigned long long)(gc_kept_bytes >> 20),
                        (unsigned long long)gc_dropped);
            gc_report_time = now;
            gc_evictions = gc_downloads = gc_dropped = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](Sampler& sampler) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        sampler_lru_cache.Free(sampler);
        samplers.erase(sampler.hash);
        const auto sampler_id = slot_samplers.GetSlotId(sampler);
        // port (SpartanPort c22329f): keep the evicted sampler alive until the GPU completes the
        // work recorded with it; destroying it here freed a handle still in use.
        scheduler.DeferOperation([retired = std::move(slot_samplers[sampler_id])] { (void)retired; });
        slot_samplers.Erase(sampler_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    {
        std::scoped_lock lock{mutex};
        ApplyBackingWrites();
    }
    GarbageCollectImages();
    GarbageCollectSamplers();
    ++gc_tick;
}

void TextureCache::TouchImage(Image& image) {
    // Image is still valid (upstream: a use clears the maybe-GPU-dirty state)
    image.flags &= ~ImageFlagBits::MaybeGpuDirty;
    image.gpu_hash = 0;
    // port: skips the LRU list (a cache miss) when already touched in this GC period.
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    image_lru_cache.Touch(image, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    CancelReadbacks(image.info.guest_address, image.info.guest_size);
    CancelTinyReadbacks(image.info.guest_address, image.info.guest_size);
    if (tiny_readbacks) {
        std::unique_lock lk{download_images_mutex};
        tiny_downloads.erase(image_id);
    }
    ASSERT_MSG(image.IsUntracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    const auto alias_it = True(image.flags & ImageFlagBits::Aliased)
                              ? alias_states.find(image.info.guest_address)
                              : alias_states.end();
    if (alias_it != alias_states.end()) {
        AliasState& state = alias_it.value();
        // ponytail: no copy here (DeleteImage also runs on guest threads through UnmapMemory,
        // which must not record commands); the aliases then re-read guest memory.
        if ((state.writer == image_id && state.writer_uid == image.image_uid) ||
            (state.backing == image_id && state.backing_uid == image.image_uid)) {
            state.ResetAuthority();
        }
        if (state.members <= 2) {
            ForEachAlias(image, [](ImageId, Image& candidate) {
                candidate.flags &= ~ImageFlagBits::Aliased;
            });
            alias_states.erase(alias_it);
        } else {
            --state.members;
        }
    }

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        UnregisterMeta(meta_info.cmask_addr, image.image_uid);
    }
    if (meta_info.fmask_addr) {
        UnregisterMeta(meta_info.fmask_addr, image.image_uid);
    }
    if (meta_info.htile_addr) {
        UnregisterMeta(meta_info.htile_addr, image.image_uid);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.Erase(image_view_id);
            }
        }
        slot_images.Erase(image_id);
    });
}

} // namespace VideoCore
