// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <map>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/elf_info.h"
#include "common/hash.h"
#include "common/path_util.h"
#include "common/serdes.h"
#include "common/scope_exit.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "common/thread.h"
#include "port_overlay.h"
#include "core/emulator_settings.h"
#include "bb_face_depth_fix.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ssbo_barrier_cache_version.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_amd_paths.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_lighting_test.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// port: past both histories (port 9/8/5, upstream 9/6/3 and #5365's binary 10): caches of
// either are rebuilt. The meta layout lost the fetch shader (StageSpecialization carries it) and
// keeps fs_interpolation; pipelines keep the per-vertex geometry shader (binary 12, meta 10, key 7).
static constexpr u32 ShaderBinaryVersion = 15u; // GCN NaN clamp, 2D NumLayers and volumetric clear.
static constexpr u32 ShaderMetaVersion = 11u;   // SamplerResource::associated_images.
static constexpr u32 PipelineKeyVersion = 7u;
} // namespace Serialization

namespace Vulkan {

extern std::atomic<u64> g_port_pipeline_driver_ns;

namespace {
// port: the test knobs that change recompiled SPIR-V (THEORDER_LIGHTING_TEST=dref,
// THEORDER_AMD_PATHS_OFF=shaderext, THEORDER_DIRECT_MEMORY_ACCESS) get separate cache versions.
u32 CurrentShaderBinaryVersion() {
    return LightingTest::ShaderBinaryVersion(
        Serialization::ShaderBinaryVersion,
        LightingTest::ShaderBinarySalt(LightingTest::Current().dref,
                                       AmdPathsOff::Current().shaderext,
                                       EmulatorSettings.IsDirectMemoryAccessEnabled()));
}

// port: a translation fix that only some shaders meet raises their version alone, so only they
// are translated again (a ShaderBinaryVersion bump rebuilds every pipeline: a cold-cache session).
u32 ShaderBinaryVersionFor(const Shader::Info& info,
                           const Shader::StageSpecialization& spec,
                           bool needs_ssbo_barriers) {
    u32 version = CurrentShaderBinaryVersion();
    if (info.uses_buffer_atomic_float_min_max || info.uses_image_atomic_float_min_max) {
        version += 1u << 16; // the float atomic min/max fallback runs one atomic (2026-10-18)
    }
    if (spec.runtime_info.hw_stage == Shader::HwStage::Vertex &&
        spec.runtime_info.hw.vs.motion_vectors) {
        version += 1u << 17; // motion addresses moved from specialization constants to push data
    }
    // These extra barriers only run on NVIDIA profiles. Do not retranslate
    // writable shaders on AMD/Intel, where IR and SPIR-V are unaffected.
    version += Shader::SsboBarrierCacheSalt(needs_ssbo_barriers, info.buffers);
    return version;
}
} // namespace

namespace PipelineBinaryCache {
bool ValidKey(const Key& key) {
    return !key.empty() && key.size() <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
}
std::vector<u8> Encode(const Key& global, const Key& pipeline, u64 graphics_hash,
                       const std::vector<Binary>& binaries) {
    if (!ValidKey(global) || !ValidKey(pipeline) || binaries.empty() ||
        binaries.size() > MaxBinaries) {
        return {};
    }
    std::vector<u8> bytes;
    const auto append = [&](const auto& value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(value));
    };
    const auto blob = [&](const auto& data) {
        append(u32(data.size()));
        bytes.insert(bytes.end(), data.begin(), data.end());
    };
    append(u32{0x31425047}); // GPB1, little-endian Windows/Linux cache format.
    append(u32{1});
    append(graphics_hash);
    blob(global);
    blob(pipeline);
    append(u32(binaries.size()));
    for (const auto& binary : binaries) {
        if (!ValidKey(binary.key) || binary.data.empty() || binary.data.size() > MaxFileSize ||
            bytes.size() + binary.key.size() + binary.data.size() + 16 > MaxFileSize) {
            return {};
        }
        blob(binary.key);
        blob(binary.data);
    }
    append(u64{XXH3_64bits(bytes.data(), bytes.size())});
    return bytes;
}
std::optional<std::vector<Binary>> Decode(std::span<const u8> bytes, const Key& global,
                                          const Key& pipeline, u64 graphics_hash) {
    if (bytes.size() < 8 || bytes.size() > MaxFileSize || !ValidKey(global) ||
        !ValidKey(pipeline)) {
        return std::nullopt;
    }
    u64 checksum;
    std::memcpy(&checksum, bytes.data() + bytes.size() - 8, 8);
    bytes = bytes.first(bytes.size() - 8);
    if (checksum != XXH3_64bits(bytes.data(), bytes.size())) {
        return std::nullopt;
    }
    size_t offset = 0;
    const auto read = [&](auto& value) {
        if (sizeof(value) > bytes.size() - offset) {
            return false;
        }
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    };
    const auto blob = [&](auto& data, size_t limit) {
        u32 size;
        if (!read(size) || !size || size > limit || size > bytes.size() - offset) {
            return false;
        }
        data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        offset += size;
        return true;
    };
    u32 magic, version, count;
    u64 stored_hash;
    Key stored_global, stored_pipeline;
    if (!read(magic) || magic != 0x31425047 || !read(version) || version != 1 ||
        !read(stored_hash) || stored_hash != graphics_hash || !blob(stored_global, 32) ||
        stored_global != global || !blob(stored_pipeline, 32) || stored_pipeline != pipeline ||
        !read(count) || !count || count > MaxBinaries) {
        return std::nullopt;
    }
    std::vector<Binary> binaries(count);
    for (auto& binary : binaries) {
        if (!blob(binary.key, 32) || !blob(binary.data, MaxFileSize)) {
            return std::nullopt;
        }
    }
    return offset == bytes.size() ? std::optional{std::move(binaries)} : std::nullopt;
}
bool Save(const std::filesystem::path& file, std::span<const u8> bytes) {
    if (bytes.empty() || bytes.size() > MaxFileSize) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
        return false;
    }
    // Unique siblings avoid concurrent writers truncating one another's temporary data.
    static std::atomic<u64> sequence{0};
    auto temp = file;
    temp += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "." + std::to_string(sequence++) + ".tmp";
    bool written = false;
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (stream) {
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            stream.flush();
            written = bool(stream);
            stream.close();
            written = written && !stream.fail();
        }
    }
    bool renamed = false;
    if (written) {
#ifdef _WIN32
        renamed = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        std::filesystem::rename(temp, file, error);
        renamed = !error;
#endif
    }
    if (!renamed) {
        std::filesystem::remove(temp, error);
    }
    return renamed;
}
std::optional<std::vector<Binary>> Load(const std::filesystem::path& file, const Key& global,
                                        const Key& pipeline, u64 graphics_hash) {
    // port: stdio reads these 2 MB files in half the time std::ifstream takes (libc++, MinGW);
    // a read error stays a cache miss.
#ifdef _WIN32
    std::unique_ptr<FILE, int (*)(FILE*)> stream{_wfopen(file.c_str(), L"rb"), &std::fclose};
#else
    std::unique_ptr<FILE, int (*)(FILE*)> stream{std::fopen(file.c_str(), "rb"), &std::fclose};
#endif
    if (!stream || std::fseek(stream.get(), 0, SEEK_END) != 0) {
        return std::nullopt;
    }
    const long size = std::ftell(stream.get());
    if (size <= 0 || u64(size) > MaxFileSize || std::fseek(stream.get(), 0, SEEK_SET) != 0) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), stream.get()) != bytes.size()) {
        return std::nullopt;
    }
    return Decode(bytes, global, pipeline, graphics_hash);
}
} // namespace PipelineBinaryCache

namespace {
std::atomic<u64> binary_hits{}, binary_misses{}, binary_rejected{}, binary_saved{}, binary_errors{};
std::atomic<u64> binary_restore_ns{}, binary_compile_ns{};
std::atomic<u64> binary_restore_calls{}, binary_compile_calls{};
std::atomic<u64> binary_key_ns{}, binary_load_ns{}, binary_handles_ns{};
u64 ElapsedNs(std::chrono::steady_clock::time_point started) {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - started).count());
}
PipelineBinaryCache::Key BinaryKey(const vk::PipelineBinaryKeyKHR& key) {
    if (!key.keySize || key.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
        return {};
    }
    return {key.key.begin(), key.key.begin() + key.keySize};
}

std::optional<std::vector<PipelineBinaryCache::Binary>> LoadLegacyBinary(
    const std::filesystem::path& directory, const std::string& name,
    const PipelineBinaryCache::Key& global, const PipelineBinaryCache::Key& pipeline) {
    using Index = std::unordered_multimap<std::string, std::pair<std::filesystem::path, u64>>;
    static std::mutex mutex;
    static std::map<std::filesystem::path, Index> indexes;
    std::vector<std::pair<std::filesystem::path, u64>> candidates;
    {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = indexes.try_emplace(directory);
        if (inserted) {
            std::error_code error;
            for (std::filesystem::directory_iterator files(directory, error), end;
                 !error && files != end; files.increment(error)) {
                const auto stem = files->path().stem().string();
                if (files->path().extension() != ".bin" || stem.size() < 18 || stem[16] != '_') {
                    continue;
                }
                try {
                    size_t used = 0;
                    const auto hash = std::stoull(stem.substr(0, 16), &used, 16);
                    if (used == 16) {
                        it->second.emplace(stem.substr(17), std::pair{files->path(), hash});
                    }
                } catch (const std::exception&) {
                }
            }
        }
        const auto [first, last] = it->second.equal_range(name);
        for (auto entry = first; entry != last; ++entry) {
            candidates.push_back(entry->second);
        }
    }
    for (const auto& [file, hash] : candidates) {
        if (auto binaries = PipelineBinaryCache::Load(file, global, pipeline, hash)) {
            return binaries;
        }
    }
    return std::nullopt;
}
} // namespace

// port: the binaries are a cache of driver output, 1-3 MB each on NVIDIA; permutations that
// came back under new indices every session left 41 GB of them. The most recently used are
// kept within THEORDER_PIPELINE_BINARY_CACHE_MB (default 4096): beyond it the oldest go, down to
// three quarters of it. Temporary files of writes cut short go too.
static void TrimPipelineBinaries(const std::filesystem::path& directory) {
    u64 budget = u64{4096} << 20;
    if (const char* env = std::getenv("THEORDER_PIPELINE_BINARY_CACHE_MB")) {
        budget = std::strtoull(env, nullptr, 10) << 20;
    }
    using Clock = std::filesystem::file_time_type::clock;
    const auto stale_temp = Clock::now() - std::chrono::minutes(10);
    struct File {
        std::filesystem::file_time_type time;
        u64 size;
        std::filesystem::path path;
    };
    std::vector<File> files;
    u64 total = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code entry_error;
        const auto time = it->last_write_time(entry_error);
        const u64 size = it->file_size(entry_error);
        if (entry_error || !it->is_regular_file(entry_error)) {
            continue;
        }
        if (it->path().extension() == ".tmp") {
            if (time < stale_temp) {
                std::filesystem::remove(it->path(), entry_error);
            }
            continue;
        }
        total += size;
        files.push_back({time, size, it->path()});
    }
    if (total <= budget) {
        return;
    }
    const u64 before = total;
    std::ranges::sort(files, {}, &File::time);
    size_t removed = 0;
    for (const auto& file : files) {
        if (total <= budget / 4 * 3) {
            break;
        }
        if (std::filesystem::remove(file.path, error)) {
            total -= file.size;
            ++removed;
        }
    }
    std::printf("Pipeline binaries: %llu MiB over the %llu MiB budget, %zu oldest removed "
                "(%llu MiB left)\n",
                static_cast<unsigned long long>(before >> 20),
                static_cast<unsigned long long>(budget >> 20), removed,
                static_cast<unsigned long long>(total >> 20));
}

void ReportPipelineBinaryCache() {
    const auto hits = binary_hits.load();
    const auto misses = binary_misses.load();
    const double per_pipeline = 1e6 * std::max(u64{1}, hits + misses);
    std::printf("Pipeline binary cache: %llu hits, %llu compiled, %llu rejected, %llu saved, "
                "%llu cache errors (restore %.3f ms, compile %.3f ms each; per pipeline: key "
                "%.3f ms, file %.3f ms, binary handles %.3f ms)\n",
                static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
                static_cast<unsigned long long>(binary_rejected.load()),
                static_cast<unsigned long long>(binary_saved.load()),
                static_cast<unsigned long long>(binary_errors.load()),
                binary_restore_ns.load() / 1e6 / std::max(u64{1}, binary_restore_calls.load()),
                binary_compile_ns.load() / 1e6 / std::max(u64{1}, binary_compile_calls.load()),
                binary_key_ns.load() / per_pipeline, binary_load_ns.load() / per_pipeline,
                binary_handles_ns.load() / per_pipeline);
}

thread_local bool g_pipeline_restore_only = false;
extern std::atomic<u64> g_port_driver_pipelines_created;

namespace {
template <typename PipelineCreateInfo, typename KeyType, typename Creator>
vk::ResultValue<vk::UniquePipeline> CreateCachedPipelineImpl(
    vk::Device device, vk::PipelineCache cache, const PipelineCreateInfo& info,
    const KeyType& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory, Creator&& creator) {
    using namespace PipelineBinaryCache;
    const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    const bool enabled = global_key.keySize && EmulatorSettings.IsPipelineCacheEnabled();
    const auto create = [&](vk::PipelineCache used_cache, const auto& used_info, bool restored) {
        const auto started = std::chrono::steady_clock::now();
        auto result = creator(used_cache, used_info);
        const auto ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count());
        g_port_pipeline_driver_ns += ns;
        if (enabled) {
            (restored ? binary_restore_ns : binary_compile_ns) += ns;
            ++(restored ? binary_restore_calls : binary_compile_calls);
        }
        // port: a pipeline the driver takes long for is a visible pause: which one, and whether
        // it came from a stored binary.
        if (ns > 50'000'000) {
            std::printf("Pipeline: %.1f ms in the driver %s (%s, key %016llx", ns / 1e6,
                        AsyncCompiler::OnWorker() ? "in the background" : "on the calling thread",
                        restored ? "from a stored binary" : enabled ? "compiled, not stored yet"
                                                                    : "compiled, no binary cache",
                        static_cast<unsigned long long>(XXH3_64bits(&key, sizeof(key))));
            if constexpr (requires { key.stage_hashes; }) {
                std::printf(", stages");
                for (const auto hash : key.stage_hashes) {
                    std::printf(" %016llx", static_cast<unsigned long long>(hash));
                }
            }
            std::printf(")\n");
        }
        return result;
    };
    if (!enabled) {
        return create(cache, info, false);
    }
    SCOPE_EXIT {
        const auto count = binary_hits.load() + binary_misses.load();
        if (count && count % 256 == 0) {
            ReportPipelineBinaryCache();
        }
    };
    const auto global = BinaryKey(global_key);
    vk::PipelineCreateFlags2CreateInfoKHR flags{
        .pNext = info.pNext,
        .flags = vk::PipelineCreateFlags2{static_cast<VkPipelineCreateFlags>(info.flags)} |
                 vk::PipelineCreateFlagBits2::eCaptureDataKHR,
    };
    auto captured_info = info;
    captured_info.pNext = &flags;
    vk::PipelineCreateInfoKHR key_info{.pNext = &captured_info};
    vk::PipelineBinaryKeyKHR pipeline_key;
    const auto key_started = std::chrono::steady_clock::now();
    const auto key_result = device.getPipelineKeyKHR(&key_info, &pipeline_key);
    binary_key_ns += ElapsedNs(key_started);
    if (key_result != vk::Result::eSuccess ||
        !ValidKey(global) || !ValidKey(BinaryKey(pipeline_key))) {
        ++binary_errors;
        ++binary_misses;
        return create(cache, info, false);
    }
    const auto pipeline_id = BinaryKey(pipeline_key);
    std::string name;
    for (const auto byte : pipeline_id) {
        name += fmt::format("{:02x}", byte);
    }
    // The driver key identifies the pipeline independently of emulator draw-state keys.
    const auto file = directory / (name + ".bin");
    std::vector<VkPipelineBinaryKHR> handles;
    const auto destroy = [&] {
        for (auto handle : handles) {
            if (handle) {
                dispatch.vkDestroyPipelineBinaryKHR(device, handle, nullptr);
            }
        }
        handles.clear();
    };
    SCOPE_EXIT { destroy(); };
    try {
        const auto load_started = std::chrono::steady_clock::now();
        auto binaries = Load(file, global, pipeline_id, 0);
        if (!binaries) {
            binaries = LoadLegacyBinary(directory, name, global, pipeline_id);
            if (binaries) {
                Save(file, Encode(global, pipeline_id, 0, *binaries));
            }
        }
        binary_load_ns += ElapsedNs(load_started);
        if (binaries) {
            std::vector<VkPipelineBinaryKeyKHR> keys(binaries->size());
            std::vector<VkPipelineBinaryDataKHR> data(binaries->size());
            for (size_t i = 0; i < binaries->size(); ++i) {
                keys[i].sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
                keys[i].keySize = u32((*binaries)[i].key.size());
                std::ranges::copy((*binaries)[i].key, keys[i].key);
                data[i] = {(*binaries)[i].data.size(), (*binaries)[i].data.data()};
            }
            VkPipelineBinaryKeysAndDataKHR stored{u32(keys.size()), keys.data(), data.data()};
            VkPipelineBinaryCreateInfoKHR binary_info{
                VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
            binary_info.pKeysAndDataInfo = &stored;
            handles.resize(keys.size());
            VkPipelineBinaryHandlesInfoKHR output{
                VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
            output.pipelineBinaryCount = u32(handles.size());
            output.pPipelineBinaries = handles.data();
            const auto handles_started = std::chrono::steady_clock::now();
            const auto handles_result =
                dispatch.vkCreatePipelineBinariesKHR(device, &binary_info, nullptr, &output);
            binary_handles_ns += ElapsedNs(handles_started);
            if (handles_result == VK_SUCCESS) {
                VkPipelineBinaryInfoKHR restored{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR};
                restored.pNext = &flags;
                restored.binaryCount = u32(handles.size());
                restored.pPipelineBinaries = handles.data();
                // CAPTURE_DATA is only for compilation, never for binary-backed creation.
                flags.flags =
                    vk::PipelineCreateFlags2{static_cast<VkPipelineCreateFlags>(info.flags)};
                auto restored_info = info;
                restored_info.pNext = &restored;
                auto result = create(nullptr, restored_info, true);
                if (result.result == vk::Result::eSuccess) {
                    ++binary_hits;
                    // The file's time orders TrimPipelineBinaries.
                    std::error_code error;
                    std::filesystem::last_write_time(
                        file, std::filesystem::file_time_type::clock::now(), error);
                    return result;
                }
            }
            ++binary_rejected;
            destroy();
        }
    } catch (const std::exception&) {
        ++binary_errors;
        destroy();
    }
    ++binary_misses;
    if (g_pipeline_restore_only) {
        return {vk::Result::ePipelineCompileRequired, vk::UniquePipeline{}};
    }
    static const bool miss_log = std::getenv("THEORDER_BINARY_MISS_LOG") != nullptr;
    if (miss_log) {
        std::printf("Binary miss: pipeline %016llx driver key %s\n",
                    static_cast<unsigned long long>(std::hash<KeyType>{}(key)), name.c_str());
    }
    flags.flags = vk::PipelineCreateFlags2{static_cast<VkPipelineCreateFlags>(info.flags)} |
                  vk::PipelineCreateFlagBits2::eCaptureDataKHR;
    auto result = create(nullptr, captured_info, false);
    if (result.result != vk::Result::eSuccess) {
        ++binary_errors;
        return create(cache, info, false);
    }
    VkReleaseCapturedPipelineDataInfoKHR release{
        VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR};
    release.pipeline = *result.value;
    SCOPE_EXIT {
        if (dispatch.vkReleaseCapturedPipelineDataKHR(device, &release, nullptr) != VK_SUCCESS) {
            ++binary_errors;
        }
    };
    try {
        VkPipelineBinaryCreateInfoKHR binary_info{
            VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
        binary_info.pipeline = *result.value;
        VkPipelineBinaryHandlesInfoKHR output{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        if (dispatch.vkCreatePipelineBinariesKHR(device, &binary_info, nullptr, &output) !=
                VK_SUCCESS ||
            !output.pipelineBinaryCount || output.pipelineBinaryCount > MaxBinaries) {
            ++binary_errors;
            return result;
        }
        handles.resize(output.pipelineBinaryCount);
        output.pPipelineBinaries = handles.data();
        if (dispatch.vkCreatePipelineBinariesKHR(device, &binary_info, nullptr, &output) !=
            VK_SUCCESS) {
            ++binary_errors;
            return result;
        }
        std::vector<Binary> binaries;
        size_t total_size = 0;
        for (const auto handle : handles) {
            VkPipelineBinaryDataInfoKHR data_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
            data_info.pipelineBinary = handle;
            vk::PipelineBinaryKeyKHR binary_key;
            size_t size = 0;
            auto* raw_key = reinterpret_cast<VkPipelineBinaryKeyKHR*>(&binary_key);
            if (dispatch.vkGetPipelineBinaryDataKHR(device, &data_info, raw_key, &size, nullptr) !=
                    VK_SUCCESS ||
                !size || size > MaxFileSize || total_size + size > MaxFileSize ||
                !ValidKey(BinaryKey(binary_key))) {
                ++binary_errors;
                return result;
            }
            total_size += size;
            Binary binary{BinaryKey(binary_key), std::vector<u8>(size)};
            if (dispatch.vkGetPipelineBinaryDataKHR(device, &data_info, raw_key, &size,
                                                    binary.data.data()) != VK_SUCCESS ||
                size > binary.data.size()) {
                ++binary_errors;
                return result;
            }
            binary.key = BinaryKey(binary_key);
            binary.data.resize(size);
            binaries.push_back(std::move(binary));
        }
        const auto encoded = Encode(global, pipeline_id, 0, binaries);
        if (Save(file, encoded)) {
            ++binary_saved;
        } else {
            ++binary_errors;
        }
    } catch (const std::exception&) {
        ++binary_errors;
    }
    return result;
}
} // namespace

vk::ResultValue<vk::UniquePipeline> CreateCachedGraphicsPipeline(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    const GraphicsPipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory) {
    auto res = CreateCachedPipelineImpl(device, cache, info, key, global_key, directory,
        [&](vk::PipelineCache c, const vk::GraphicsPipelineCreateInfo& i) {
            return device.createGraphicsPipelineUnique(c, i);
        });
    if (res.result == vk::Result::eSuccess && res.value) {
        g_port_driver_pipelines_created.fetch_add(1, std::memory_order_relaxed);
    }
    return res;
}

vk::ResultValue<vk::UniquePipeline> CreateCachedComputePipeline(
    vk::Device device, vk::PipelineCache cache, const vk::ComputePipelineCreateInfo& info,
    const ComputePipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory) {
    auto res = CreateCachedPipelineImpl(device, cache, info, key, global_key, directory,
        [&](vk::PipelineCache c, const vk::ComputePipelineCreateInfo& i) {
            return device.createComputePipelineUnique(c, i);
        });
    if (res.result == vk::Result::eSuccess && res.value) {
        g_port_driver_pipelines_created.fetch_add(1, std::memory_order_relaxed);
    }
    return res;
}

// port: THEORDER_PARALLEL_WARMUP=1. The cache store is still read and the shader modules created in
// order on the calling thread (program_cache and its permutation checks are not thread-safe);
// only the pipeline objects, the slow part, are built here. The runtime compiler builds
// pipelines on worker threads the same way (vk_pipeline_cache.cpp).
struct PipelineCache::WarmupPool {
    explicit WarmupPool(u32 count, bool low_priority = false) {
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, low_priority] {
                if (low_priority) {
                    Common::SetCurrentThreadName("Port:ShaderWarmup");
                    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
                }
                Run();
            });
        }
    }
    ~WarmupPool() {
        {
            std::scoped_lock lock{mutex};
            jobs.clear(); // only the running ones finish
            closing = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }
    void Push(std::function<bool()> job) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }
    /// Waits for every job; `tick` runs on this thread meanwhile (the loading screen).
    void Wait(const std::function<void()>& tick) {
        std::unique_lock lock{mutex};
        while (!jobs.empty() || running) {
            idle.wait_for(lock, std::chrono::milliseconds(50));
            lock.unlock();
            tick();
            lock.lock();
        }
    }
    /// Drops the queued jobs and waits for the running ones; the threads stay parked.
    void Drain() {
        std::unique_lock lock{mutex};
        jobs.clear();
        idle.wait(lock, [this] { return running == 0; });
    }
    bool Idle() {
        std::scoped_lock lock{mutex};
        return jobs.empty() && !running;
    }
    static u32 Threads() {
        const char* env = std::getenv("THEORDER_PARALLEL_WARMUP");
        if (!env || !*env || *env == '0') {
            return 0;
        }
        const u32 cores = std::max(1u, std::thread::hardware_concurrency());
        return std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 6u);
    }

    std::atomic<u32> built{0};
    std::atomic<u32> failed{0};

private:
    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            cv.wait(lock, [this] { return closing || !jobs.empty(); });
            if (jobs.empty()) {
                return;
            }
            auto job = std::move(jobs.front());
            jobs.pop_front();
            ++running;
            lock.unlock();
            bool ok = false;
            try {
                ok = job();
            } catch (const std::exception& e) {
                LOG_ERROR(Render_Vulkan, "port: warm-up pipeline failed: {}", e.what());
            } catch (...) {
                LOG_ERROR(Render_Vulkan, "port: warm-up pipeline failed");
            }
            (ok ? built : failed).fetch_add(1, std::memory_order_relaxed);
            lock.lock();
            --running;
            idle.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable cv, idle;
    std::deque<std::function<bool()>> jobs;
    std::vector<std::thread> threads;
    u32 running = 0;
    bool closing = false;
};

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx, u64 spv_hash, bool needs_ssbo_barriers) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(ShaderBinaryVersionFor(info, spec, needs_ssbo_barriers));

    meta.Write(perm_hash);
    meta.Write(perm_idx);
    meta.Write(spv_hash);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

u64 SpvHash(std::span<const u32> spv) {
    return XXH3_64bits(spv.data(), spv.size_bytes());
}

u64 RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    const u64 hash = SpvHash(spv);
    if (!Storage::DataBase::Instance().IsOpened()) {
        return hash;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
    return hash;
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx, u64& spv_hash,
                    bool needs_ssbo_barriers) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    // The layout is ShaderMetaVersion's; the expected binary version needs the shader's info.
    if ((binary_version & 0xffffu) != CurrentShaderBinaryVersion()) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);
    meta.Read(spv_hash);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    if (binary_version != ShaderBinaryVersionFor(info, spec, needs_ssbo_barriers)) {
        return false;
    }
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    if (warmup_pool) {
        // The slot stays null until WarmUp moves the built pipeline in (the map may rehash).
        warmup_pool->Push([this, key = compute_key, info = sel.infos[0], module = sel.modules[0],
                           sdata]() mutable {
            auto pipeline = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap,
                                                              profile, *pipeline_cache, key, *info,
                                                              module, sdata, true);
            std::scoped_lock lock{warmup_results_mutex};
            warmup_compute.emplace_back(key, std::move(pipeline));
            return true;
        });
    } else {
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key,
                                                       *sel.infos[0], sel.modules[0], sdata, true);
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    sdata.Write(per_vertex_gs);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    sdata.Read(per_vertex_gs);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);

    if (warmup_pool) {
        warmup_pool->Push([this, key = sel.graphics_key, infos = sel.infos,
                           runtime_infos = sel.runtime_infos, fetch = sel.fetch_shader,
                           modules = sel.modules, sdata]() mutable {
            auto pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, key, *pipeline_cache, infos,
                runtime_infos, fetch ? &*fetch : nullptr, modules, sdata, true);
            std::scoped_lock lock{warmup_results_mutex};
            warmup_graphics.emplace_back(key, std::move(pipeline));
            return true;
        });
    } else {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader ? &*sel.fetch_shader : nullptr, sel.modules, sdata,
            true);
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    u64 spv_hash{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx, spv_hash,
                        profile.needs_ssbo_store_load_barrier)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty() || SpvHash(spv) != spv_hash) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};
    u64 module_hash = spv_hash;

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        program->warm_partial = true;
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module ? it->module : LoadDeferredModule(*it_pgm.value(), idx);
            if (!module) {
                return false;
            }
            module_hash = it->spv_hash;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx, module_hash);
    if (it_pgm.value()->info.sw_stage == Shader::SwStage::Vertex) {
        BbFaceDepth::Register(module, spv, it_pgm.value()->info.pgm_hash);
    }

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;
    if (const auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data;
        !fetch.Empty()) {
        sel.fetch_shader = fetch;
    }

    return true;
}

// port: without a bulk warm-up (the pipeline binary cache, the default on NVIDIA) every session
// translated every shader again, the GPU thread waiting for each new graphics shader (1-2 s per
// 5 s while the prologue starts). The translated permutations of a program are stored by index
// (RegisterShaderMeta/RegisterShaderBinary): load them when the program is first needed, so the
// specialization lookup finds a ready module. Indices are read contiguously from 0; a program's
// resource lists do not depend on its T#s, so any permutation's info serves as the program's.
bool PipelineCache::LoadCachedProgram(u64 pgm_hash, Shader::HwStage stage) {
    static const bool enabled = [] {
        const char* env = std::getenv("THEORDER_LAZY_SHADER_CACHE");
        return !env || env[0] != '0';
    }();
    auto& db = Storage::DataBase::Instance();
    if (!enabled || !db.IsOpened()) {
        return false;
    }
    // Material and particle shaders keep hundreds: with 256, every session translated and
    // compiled the rest again, under indices that differ each time.
    constexpr size_t MaxCachedPermutations = 4096;
    static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
    const auto started = std::chrono::steady_clock::now();
    const char* stop = "limit";
    const auto it_existing = program_cache.find(pgm_hash);
    Program* existing = it_existing != program_cache.end() ? it_existing->second.get() : nullptr;
    if (existing) {
        existing->warm_partial = false;
    }
    std::unique_ptr<Program> program;
    size_t loaded_count = 0;
    for (size_t index = 0; index < MaxCachedPermutations; ++index) {
        std::vector<u8> meta_blob;
        const u64 perm_hash = HashCombine(pgm_hash, index);
        if (meta_prefetch && meta_prefetch->ready.load(std::memory_order_acquire)) {
            // Each program loads once a session: the entry can move out of the map.
            if (const auto found = meta_prefetch->metas.find(perm_hash);
                found != meta_prefetch->metas.end()) {
                meta_blob = std::move(found->second);
                meta_prefetch->metas.erase(found);
            }
        } else {
            const auto meta_name = fmt::format("{:#018x}", perm_hash);
            if (db.Exists(Storage::BlobType::ShaderMeta, meta_name)) {
                db.Load(Storage::BlobType::ShaderMeta, meta_name, meta_blob);
            }
        }
        if (meta_blob.empty()) {
            stop = "no meta";
            break;
        }
        Serialization::Archive meta_ar{std::move(meta_blob)};
        auto info = std::make_unique<Shader::Info>();
        Shader::StageSpecialization spec{};
        size_t perm_idx{};
        u64 spv_hash{};
        if (!LoadShaderMeta(meta_ar, *info, spec, perm_idx, spv_hash,
                            profile.needs_ssbo_store_load_barrier)) {
            stop = "meta version";
            break;
        }
        if (perm_idx != index || info->pgm_hash != pgm_hash || info->hw_stage != stage) {
            stop = "meta of another program or stage";
            break;
        }
        if (existing) {
            if (info->buffers.size() != existing->info.buffers.size() ||
                info->images.size() != existing->info.images.size() ||
                info->samplers.size() != existing->info.samplers.size() ||
                info->fmasks.size() != existing->info.fmasks.size()) {
                stop = "resources differ from existing program";
                break;
            }
            std::unique_lock lock{programs_mutex};
            if (index >= existing->modules.size() ||
                (!existing->modules[index].module && !existing->modules[index].pending_compile &&
                 !existing->modules[index].spec.Valid())) {
                spec.info = &existing->info;
                existing->InsertPermut(vk::ShaderModule{}, std::move(spec), index, spv_hash);
                ++loaded_count;
            }
            continue;
        }
        if (!program) {
            program = std::make_unique<Program>();
            program->info = std::move(*info);
        }
        spec.info = &program->info;
        // The module itself is read when this specialization is first used (LoadDeferredModule):
        // material shaders keep hundreds of permutations a session mostly does not need.
        program->InsertPermut(vk::ShaderModule{}, std::move(spec), index, spv_hash);
        ++loaded_count;
    }
    if (log) {
        LOG_WARNING(Render_Vulkan,
                    "port: cached program {:#x} ({}): {} permutations in {:.1f} ms, stopped: {}",
                    pgm_hash, u32(stage),
                    existing ? existing->modules.size() : (program ? program->modules.size() : 0),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              started)
                        .count(),
                    stop);
    }
    if (existing) {
        return loaded_count > 0;
    }
    if (!program) {
        return false;
    }
    std::unique_lock lock{programs_mutex};
    program_cache.emplace(pgm_hash, std::move(program));
    return true;
}

vk::ShaderModule PipelineCache::LoadDeferredModule(Program& program, size_t perm_idx) {
    std::vector<u32> spv;
    Storage::DataBase::Instance().Load(
        Storage::BlobType::ShaderBinary,
        fmt::format("{:#018x}_{}", program.info.pgm_hash, perm_idx), spv);
    std::unique_lock lock{programs_mutex};
    auto& entry = program.modules[perm_idx];
    if (spv.empty() || SpvHash(spv) != entry.spv_hash) {
        entry.spec = {}; // never matches again
        return {};
    }
    entry.module = CompileSPV(spv, instance.GetDevice());
    (void)program.StagePipelinePermutation(perm_idx);
    if (program.info.sw_stage == Shader::SwStage::Vertex) {
        BbFaceDepth::Register(entry.module, spv, program.info.pgm_hash);
    }
    return entry.module;
}

bool PipelineCache::CheckCacheProfile() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return false;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return false;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // port: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return false;
    }
    return true;
}

void PipelineCache::WarmUp(const std::function<void(u32, u32)>& progress) {
    if (!CheckCacheProfile()) {
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    const u32 expected =
        u32(Storage::DataBase::Instance().CountBlobs(Storage::BlobType::PipelineKey));
    const auto started = std::chrono::steady_clock::now();
    const u32 warmup_threads = WarmupPool::Threads();
    std::unique_ptr<WarmupPool> pool;
    if (warmup_threads) {
        pool = std::make_unique<WarmupPool>(warmup_threads);
        warmup_pool = pool.get();
    }
    // In parallel mode the screen follows the pipelines built, not the ones read.
    const auto report = [&] {
        if (progress) {
            const u32 done = pool ? pool->built + pool->failed : num_total_pipelines;
            progress(done, std::max(expected, num_total_pipelines));
        }
    };

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            report();

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    if (pool) {
        pool->Wait(report);
        warmup_pool = nullptr;
        // Every slot was reserved by the loaders; the ones whose build failed stay out.
        for (auto& [key, pipeline] : warmup_graphics) {
            graphics_pipelines[key] = std::move(pipeline);
        }
        for (auto& [key, pipeline] : warmup_compute) {
            compute_pipelines[key] = std::move(pipeline);
        }
        for (auto it = graphics_pipelines.begin(); it != graphics_pipelines.end();) {
            it = it->second ? std::next(it) : graphics_pipelines.erase(it);
        }
        for (auto it = compute_pipelines.begin(); it != compute_pipelines.end();) {
            it = it->second ? std::next(it) : compute_pipelines.erase(it);
        }
        num_pipelines -= pool->failed;
        warmup_graphics.clear();
        warmup_compute.clear();
        pool.reset();
    }
    std::printf("Pipeline warm-up: %u of %u pipelines in %.2f s (%u threads)\n", num_pipelines,
                num_total_pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                warmup_threads);
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    ReportPipelineBinaryCache();
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::StartOnDemand() {
    CheckCacheProfile();
    Storage::DataBase::Instance().FinishPreload();
    if (Storage::DataBase::Instance().IsOpened() &&
        Storage::DataBase::Instance().ConcurrentReads()) {
        meta_prefetch = std::make_unique<MetaPrefetch>();
        meta_prefetch->thread = std::jthread([prefetch = meta_prefetch.get()](std::stop_token stop) {
            Common::SetCurrentThreadName("port:MetaPrefetch");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            const auto started = std::chrono::steady_clock::now();
            Storage::DataBase::Instance().ForEachBlob(
                Storage::BlobType::ShaderMeta,
                [prefetch](std::vector<u8>&& data) {
                    // Meta version, binary version, then the permutation hash (RegisterShaderMeta).
                    if (data.size() >= 16) {
                        u64 perm_hash;
                        std::memcpy(&perm_hash, data.data() + 8, sizeof(perm_hash));
                        prefetch->metas.emplace(perm_hash, std::move(data));
                    }
                },
                [&stop] { return stop.stop_requested(); });
            const size_t metas_read = prefetch->metas.size();
            prefetch->ready.store(!stop.stop_requested(), std::memory_order_release);
            std::printf("Shader cache: %zu permutation metas read in %.1f s\n",
                        metas_read,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                            .count());
            if (!stop.stop_requested()) {
                TrimPipelineBinaries(Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                     Common::ElfInfo::Instance().GameSerial() /
                                     "pipeline-binaries");
            }
            // Parked until exit, like the warm-up threads (StopWarmUp): this thread ending while
            // the hot list warm-up ran crashed in its exit path (libc++, read of -1).
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }
    std::printf("Pipeline startup: on demand, no bulk warm-up\n");
    StartHotWarmUp();
}


// port: one cached shader stage read by the background warm-up, shared by every cached
// pipeline that uses it. A pool thread fills it; the GPU thread merges it into program_cache
// once (MergeWarmStage).
struct PipelineCache::WarmStage {
    vk::Device device;
    std::unique_ptr<Program> program;
    Shader::StageSpecialization spec{};
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t perm_idx{};
    u64 spv_hash{};
    vk::ShaderModule module{};
    // GPU thread, from the merge on.
    bool merged = false;
    bool module_kept = false;           ///< the module now belongs to a Program
    const Shader::Info* info = nullptr; ///< null: not usable (read failed or conflict)
    vk::ShaderModule result{};
    const char* why = "not read"; ///< why info is null (THEORDER_PERMUTATION_LOG)
    u32 start = 0; ///< the module's first descriptor binding (its specialization's start)

    ~WarmStage() {
        if (module && !module_kept) {
            device.destroyShaderModule(module);
        }
    }
};

// port: StartBackgroundWarmUp. The pool reads the store and builds the pipelines at low
// priority; the GPU thread merges the stages and publishes the pipelines a few at a time.
struct PipelineCache::BackgroundWarmup {
    struct Entry {
        std::chrono::steady_clock::time_point queued;
        bool compute = false;
        GraphicsPipelineKey graphics_key{};
        ComputePipelineKey compute_key{};
        GraphicsPipeline::SerializationSupport sdata{};
        std::array<std::shared_ptr<WarmStage>, MaxShaderStages> stages{};
    };
    struct Built {
        GraphicsPipelineKey graphics_key{};
        ComputePipelineKey compute_key{};
        std::unique_ptr<GraphicsPipeline> graphics;
        std::unique_ptr<ComputePipeline> compute;
    };

    std::unordered_map<u64, std::shared_ptr<WarmStage>> stages; ///< read job only
    WarmupInbox<Entry> read;
    WarmupInbox<Built> built;
    std::atomic<u32> total{0}, done{0};
    std::atomic<bool> reading{true}, stopping{false};
    std::string reading_name; ///< hot list reader: the store name of the entry being read
    u32 misnumbered = 0; ///< entries left to the game: a module's bindings start elsewhere
    std::atomic<u64> reader_ns{0}, entry_ns{0}, shader_io_ns{0}, module_ns{0};
    std::atomic<u64> read_queue_ns{0}, build_queue_ns{0}, build_ns{0};
    std::atomic<u32> entries{0}, modules{0}, submitted{0}, completed{0};
    u64 pump_ns = 0, pump_max_ns = 0, pump_gap_max_ns = 0;
    u32 batches = 0;
    bool finished = false;
    u32 loaded = 0, threads = 0;
    std::chrono::steady_clock::time_point started, last_batch;
    std::chrono::steady_clock::time_point last_pump{}, last_report{};
    std::unique_ptr<WarmupPool> pool; ///< last: its jobs use the members above
};

namespace {
// Pipelines kept in the list: about what a few chapters use (~650 on the London rooftop).
constexpr size_t HotListMax = 2000;

u32 BackgroundWarmupThreads(u32 cores) {
    const char* parallel = std::getenv("THEORDER_PARALLEL_WARMUP");
    return BackgroundWarmupThreadCount(cores, !parallel || *parallel != '0');
}
} // namespace

void PipelineCache::StartHotWarmUp() {
    const char* enabled = std::getenv("THEORDER_HOT_WARMUP");
    if ((enabled && *enabled == '0') || !Storage::DataBase::Instance().IsOpened() ||
        !Storage::DataBase::Instance().ConcurrentReads()) {
        return;
    }
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                      Common::ElfInfo::Instance().GameSerial() / "hot_pipelines.txt";
    // The most recent use of each key last; the list is written back without repeats.
    std::vector<std::string> keys;
    {
        std::vector<std::string> lines;
        if (std::FILE* file = std::fopen(path.string().c_str(), "r")) {
            char line[64];
            while (std::fgets(line, sizeof(line), file)) {
                std::string key{line};
                while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) {
                    key.pop_back();
                }
                if (key.size() == 20 && (key[0] == 'g' || key[0] == 'c') && key[1] == '_') {
                    lines.push_back(std::move(key));
                }
            }
            std::fclose(file);
        }
        std::unordered_set<std::string> seen;
        for (auto it = lines.rbegin(); it != lines.rend() && keys.size() < HotListMax; ++it) {
            if (seen.insert(*it).second) {
                keys.push_back(*it);
            }
        }
        std::reverse(keys.begin(), keys.end());
    }
    if (std::FILE* file = std::fopen(path.string().c_str(), "w")) {
        for (const auto& key : keys) {
            std::fprintf(file, "%s\n", key.c_str());
        }
        std::fclose(file);
    }
    hot_list = std::fopen(path.string().c_str(), "a");
    if (keys.empty()) {
        return;
    }
    background = new BackgroundWarmup;
    auto& warm = *background;
    warm.started = std::chrono::steady_clock::now();
    warm.threads = BackgroundWarmupThreads(std::max(1u, std::thread::hardware_concurrency()));
    warm.pool = std::make_unique<WarmupPool>(warm.threads, true);
    warm.total = u32(keys.size());
    // Most recent first: the scene the last session ended in is likely the next one's start.
    warm.pool->Push([this, &warm, keys = std::move(keys)] {
        const auto reader_started = std::chrono::steady_clock::now();
        auto& db = Storage::DataBase::Instance();
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            if (warm.stopping.load(std::memory_order_relaxed)) {
                break;
            }
            std::vector<u8> data;
            db.Load(Storage::BlobType::PipelineKey, *it, data);
            if (data.empty()) {
                static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
                if (log) {
                    std::printf("Warm-up skipped %s: not in the store\n", it->c_str());
                }
                warm.done.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            warm.reading_name = *it;
            ReadWarmEntry(warm, std::move(data));
        }
        warm.stages.clear();
        warm.reader_ns = ElapsedNs(reader_started);
        warm.reading = false;
        return true;
    });
    std::printf("Pipeline warm-up: %u pipelines recent sessions used, in the background "
                "(%u threads)\n", warm.total.load(), warm.threads);
}

void PipelineCache::NoteHot(const Pipeline& pipeline, char kind, u64 hash) {
    pipeline.hot_noted = true;
    if (hot_list) {
        std::fprintf(hot_list, "%c_%#018llx\n", kind, static_cast<unsigned long long>(hash));
        std::fflush(hot_list);
    }
}

bool PipelineCache::StartBackgroundWarmUp() {
    if (!Storage::DataBase::Instance().ConcurrentReads()) {
        return false;
    }
    if (!CheckCacheProfile()) {
        return true; // nothing cached yet
    }
    background = new BackgroundWarmup;
    auto& warm = *background;
    warm.started = std::chrono::steady_clock::now();
    warm.threads = BackgroundWarmupThreads(std::max(1u, std::thread::hardware_concurrency()));
    warm.pool = std::make_unique<WarmupPool>(warm.threads, true);
    // The first job reads the whole store; with one thread the builds queue behind it.
    warm.pool->Push([this, &warm] {
        const auto reader_started = std::chrono::steady_clock::now();
        auto& db = Storage::DataBase::Instance();
        warm.total = u32(db.CountBlobs(Storage::BlobType::PipelineKey));
        db.ForEachBlob(
            Storage::BlobType::PipelineKey,
            [&](std::vector<u8>&& data) { ReadWarmEntry(warm, std::move(data)); },
            [&] { return warm.stopping.load(std::memory_order_relaxed); });
        warm.stages.clear();
        warm.reader_ns = ElapsedNs(reader_started);
        warm.reading = false;
        return true;
    });
    std::printf("Pipeline warm-up: loading in the background (%u threads)\n", warm.threads);
    return true;
}

void PipelineCache::ReadWarmEntry(BackgroundWarmup& warm, std::vector<u8>&& data) {
    const auto entry_started = std::chrono::steady_clock::now();
    SCOPE_EXIT { warm.entry_ns += ElapsedNs(entry_started); ++warm.entries; };
    const auto skip = [&] { warm.done.fetch_add(1, std::memory_order_relaxed); };
    Serialization::Archive ar{std::move(data)};
    Serialization::Reader pldata{ar};
    u32 version{};
    pldata.Read(version);
    if (version != Serialization::PipelineKeyVersion) {
        std::printf("Warm-up skipped an entry of key version %u\n", version);
        return skip();
    }
    u32 is_compute{};
    pldata.Read(is_compute);

    BackgroundWarmup::Entry entry{};
    entry.compute = is_compute != 0;
    std::array<u64, MaxShaderStages> hashes{};
    if (entry.compute) {
        entry.compute_key.Deserialize(ar);
        ComputePipeline::SerializationSupport sdata{};
        sdata.Deserialize(ar);
        hashes[0] = entry.compute_key.value;
    } else {
        entry.graphics_key.Deserialize(ar);
        entry.sdata.Deserialize(ar);
        static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (log && !warm.reading_name.empty()) {
            const auto name = fmt::format("g_{:#018x}",
                                          std::hash<GraphicsPipelineKey>{}(entry.graphics_key));
            if (name != warm.reading_name) {
                std::printf("Warm-up: %s holds the key of %s\n", warm.reading_name.c_str(),
                            name.c_str());
            }
        }
        for (u32 i = 0; i < MaxShaderStages; ++i) {
            hashes[i] = entry.graphics_key.stage_hashes[i];
        }
    }

    auto& db = Storage::DataBase::Instance();
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (!hashes[i]) {
            continue;
        }
        auto& stage = warm.stages[hashes[i]];
        if (!stage) {
            stage = std::make_shared<WarmStage>();
            stage->device = instance.GetDevice();
            stage->program = std::make_unique<Program>();
            stage->spec.info = &stage->program->info;
            const auto io_started = std::chrono::steady_clock::now();
            u64 module_time = 0;
            std::vector<u8> meta_blob;
            db.Load(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", hashes[i]), meta_blob);
            const bool meta_blob_found = !meta_blob.empty();
            if (!meta_blob.empty()) {
                Serialization::Archive meta_ar{std::move(meta_blob)};
                if (LoadShaderMeta(meta_ar, stage->program->info, stage->spec, stage->perm_idx,
                                   stage->spv_hash, profile.needs_ssbo_store_load_barrier)) {
                    if (!stage->spec.fetch_shader_data.Empty()) {
                        stage->fetch = stage->spec.fetch_shader_data;
                    }
                    stage->start = stage->spec.start.unified;
                    std::vector<u32> spv;
                    db.Load(Storage::BlobType::ShaderBinary,
                            fmt::format("{:#018x}_{}", stage->program->info.pgm_hash,
                                        stage->perm_idx),
                            spv);
                    if (!spv.empty() && SpvHash(spv) == stage->spv_hash) {
                        const auto module_started = std::chrono::steady_clock::now();
                        stage->module = CompileSPV(spv, stage->device);
                        if (stage->program->info.sw_stage == Shader::SwStage::Vertex) {
                            BbFaceDepth::Register(stage->module, spv,
                                                  stage->program->info.pgm_hash);
                        }
                        module_time = ElapsedNs(module_started);
                        warm.module_ns += module_time;
                        ++warm.modules;
                    }
                }
            }
            warm.shader_io_ns += ElapsedNs(io_started) - module_time;
            if (!stage->module) {
                stage->merged = true; // unusable: info stays null
                stage->why = meta_blob_found ? "no matching SPIR-V" : "no meta";
            }
        }
        entry.stages[i] = stage;
    }
    entry.queued = std::chrono::steady_clock::now();
    warm.read.Push(std::move(entry));
}

bool PipelineCache::MergeWarmStage(WarmStage& stage) {
    stage.merged = true;
    const u64 hash = stage.program->info.pgm_hash;
    const auto it_pgm = program_cache.find(hash);
    if (it_pgm == program_cache.end()) {
        Program* program = stage.program.get();
        program->warm_partial = true;
        program->InsertPermut(stage.module, std::move(stage.spec), stage.perm_idx, stage.spv_hash);
        program_cache.emplace(hash, std::move(stage.program));
        stage.module_kept = true;
        stage.result = stage.module;
        stage.info = &program->info;
        return true;
    }
    // The game already uses this shader: its permutations are the ones that count.
    Program& program = *it_pgm->second;
    if (program.HasPending()) {
        stage.why = "program compiling";
        return false; // the game is compiling a permutation that takes the next index
    }
    // The pipeline's layout comes from the program's resource lists: a permutation stored with
    // other lists (a resource only it declares) would bind outside the layout (Vulkan
    // VUID-VkGraphicsPipelineCreateInfo-layout-07988). The game builds that one itself.
    const auto& own = stage.program->info;
    if (own.buffers.size() != program.info.buffers.size() ||
        own.images.size() != program.info.images.size() ||
        own.samplers.size() != program.info.samplers.size() ||
        own.fmasks.size() != program.info.fmasks.size()) {
        stage.why = "resources differ from the program's";
        return false;
    }
    // Stored keys name the permutation by the index it was stored at; another session may have
    // stored the same one at a lower index too. The stored slot is used when it holds it.
    auto it = stage.perm_idx < program.modules.size() &&
                      program.modules[stage.perm_idx].spec == stage.spec
                  ? program.modules.begin() + stage.perm_idx
                  : std::ranges::find(program.modules, stage.spec, &Program::Module::spec);
    if (it != program.modules.end()) {
        if (!it->module) { // a deferred cached permutation (LoadCachedProgram): use this one
            it->module = stage.module;
            it->spv_hash = stage.spv_hash;
            (void)program.StagePipelinePermutation(std::distance(program.modules.begin(), it));
            stage.module_kept = true;
        }
        stage.result = it->module;
    } else {
        if (stage.perm_idx < program.modules.size() && program.modules[stage.perm_idx].module) {
            stage.why = "index holds another permutation";
            return false;
        }
        stage.spec.info = &program.info;
        program.InsertPermut(stage.module, std::move(stage.spec), stage.perm_idx, stage.spv_hash);
        stage.module_kept = true;
        stage.result = stage.module;
    }
    stage.info = &program.info;
    return true;
}

void PipelineCache::PumpWarmUp() {
    if (!background || background->finished) {
        return;
    }
    auto& warm = *background;
    using namespace std::chrono_literals;
    const auto pump_started = std::chrono::steady_clock::now();
    if (warm.last_pump.time_since_epoch().count()) {
        warm.pump_gap_max_ns = std::max(warm.pump_gap_max_ns,
            u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    pump_started - warm.last_pump).count()));
    }
    warm.last_pump = pump_started;
    SCOPE_EXIT {
        const auto ns = ElapsedNs(pump_started);
        warm.pump_ns += ns;
        warm.pump_max_ns = std::max(warm.pump_max_ns, ns);
    };

    // Built pipelines: the first one ready for a key wins (the game may have compiled it).
    // A build already in flight when on-demand compilation failed must not revive that key.
    for (auto& built : warm.built.Take(64)) {
        const bool kept =
            built.graphics
                ? (failed_graphics.empty() || !failed_graphics.contains(built.graphics_key)) &&
                      PublishFirst(graphics_pipelines, built.graphics_key, std::move(built.graphics))
                : PublishFirst(compute_pipelines, built.compute_key, std::move(built.compute));
        warm.loaded += kept;
        warm.done.fetch_add(1, std::memory_order_relaxed);
        static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (log && !built.compute_key.value) {
            std::printf("Warm-up published g_%#018llx kept %d\n",
                        static_cast<unsigned long long>(
                            std::hash<GraphicsPipelineKey>{}(built.graphics_key)),
                        int(kept));
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - warm.last_batch < 4ms) {
        return;
    }
    warm.last_batch = now;
    ++warm.batches;
    const u32 done = warm.done + warm.pool->failed;
    PortOverlay::SetBackgroundLoading(done, std::max(warm.total.load(), done));

    static const std::array<Shader::RuntimeInfo, MaxShaderStages> no_runtime_infos{};
    for (auto& entry : warm.read.Take(32)) {
        warm.read_queue_ns += ElapsedNs(entry.queued);
        if (entry.compute ? compute_pipelines.contains(entry.compute_key)
                          : graphics_pipelines.contains(entry.graphics_key) ||
                                (!failed_graphics.empty() && failed_graphics.contains(entry.graphics_key))) {
            static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
            if (log && !entry.compute) {
                std::printf("Warm-up skipped g_%#018llx: the game has it\n",
                            static_cast<unsigned long long>(
                                std::hash<GraphicsPipelineKey>{}(entry.graphics_key)));
            }
            warm.done.fetch_add(1, std::memory_order_relaxed); // the game compiled it first
            continue;
        }
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        bool usable = true;
        {
            std::unique_lock lock{programs_mutex};
            for (u32 i = 0; i < MaxShaderStages && usable; ++i) {
                if (!entry.stages[i]) {
                    continue;
                }
                auto& stage = *entry.stages[i];
                if (!stage.merged) {
                    MergeWarmStage(stage);
                }
                usable = stage.info != nullptr;
                infos[i] = stage.info;
                modules[i] = stage.result;
                fetch = stage.fetch; // the blocking load keeps the last stage's too
            }
        }
        // Each module numbers its descriptors from where the previous stage's end (its
        // specialization's start); the layout counts them from the stages' resource lists. A
        // module whose start does not follow would use bindings outside the layout (Vulkan
        // VUID-VkGraphicsPipelineCreateInfo-layout-07988): the game builds that pipeline itself.
        for (u32 i = 0, binding = 0; usable && i < MaxShaderStages; ++i) {
            if (!infos[i]) {
                continue;
            }
            u32 count = u32(infos[i]->buffers.size() + infos[i]->samplers.size());
            for (const auto& image : infos[i]->images) {
                count += image.NumBindings(*infos[i]);
            }
            if (count && entry.stages[i]->start != binding) {
                usable = false; // the stage itself may still fit another pipeline
                ++warm.misnumbered;
            }
            binding += count;
        }
        if (!usable) {
            static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
            for (u32 i = 0; log && i < MaxShaderStages; ++i) {
                if (entry.stages[i] && !entry.stages[i]->info) {
                    std::printf("Warm-up skipped %c_%#018llx: stage %u %#llx: %s\n",
                                entry.compute ? 'c' : 'g',
                                static_cast<unsigned long long>(
                                    entry.compute ? entry.compute_key.value
                                                  : std::hash<GraphicsPipelineKey>{}(entry.graphics_key)),
                                i, static_cast<unsigned long long>(entry.compute
                                                                     ? entry.compute_key.value
                                                                     : entry.graphics_key.stage_hashes[i]),
                                entry.stages[i]->why);
                }
            }
            warm.done.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ++warm.submitted;
        warm.pool->Push([this, &warm, queued = std::chrono::steady_clock::now(),
                          compute = entry.compute, graphics_key = entry.graphics_key,
                         compute_key = entry.compute_key, sdata = entry.sdata, infos, modules,
                         fetch]() mutable {
            const auto build_started = std::chrono::steady_clock::now();
            warm.build_queue_ns += ElapsedNs(queued);
            SCOPE_EXIT { warm.build_ns += ElapsedNs(build_started); ++warm.completed; };
            BackgroundWarmup::Built built{graphics_key, compute_key};
            if (compute) {
                ComputePipeline::SerializationSupport compute_data{};
                built.compute = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile, *pipeline_cache, compute_key,
                    *infos[0], modules[0], compute_data, true);
            } else {
                built.graphics = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
                    no_runtime_infos, fetch ? &*fetch : nullptr, modules, sdata, true);
            }
            warm.built.Push(std::move(built));
            return true;
        });
    }

    if (now - warm.last_report >= 1s) {
        warm.last_report = now;
        std::printf("Warm-up progress: %.2f s, read %u, submitted %u, completed %u, done %u/%u, "
                    "batches %u, reading %u\n",
                    std::chrono::duration<double>(now - warm.started).count(),
                    warm.entries.load(), warm.submitted.load(), warm.completed.load(),
                    warm.done.load(), warm.total.load(), warm.batches, unsigned(warm.reading.load()));
    }

    if (warm.reading || !warm.read.Empty() || !warm.pool->Idle() || !warm.built.Empty()) {
        return;
    }
    std::printf("Pipeline warm-up: %u of %u pipelines in %.2f s in the background (%u threads; "
                "%u left to the game, bindings numbered for another pipeline)\n",
                warm.loaded, warm.total.load(),
                std::chrono::duration<double>(now - warm.started).count(), warm.threads,
                warm.misnumbered);
    LOG_INFO(Render, "Preloaded {} pipelines in the background", warm.loaded);
    ReportPipelineBinaryCache();
    std::printf("Warm-up profile: reader wall %.3f s, entries %.3f s, shader IO/meta %.3f s, "
                "modules %.3f s (%u); GPU pump %.3f s, max %.3f ms, gap max %.3f ms, "
                "batches %u; builds %.3f thread-s, read queue %.3f item-s, "
                "build queue %.3f item-s\n",
                warm.reader_ns.load() / 1e9, warm.entry_ns.load() / 1e9,
                warm.shader_io_ns.load() / 1e9, warm.module_ns.load() / 1e9, warm.modules.load(),
                warm.pump_ns / 1e9, warm.pump_max_ns / 1e6, warm.pump_gap_max_ns / 1e6,
                warm.batches, warm.build_ns.load() / 1e9, warm.read_queue_ns.load() / 1e9,
                warm.build_queue_ns.load() / 1e9);
    std::printf("Pipeline binary profile: keys %.3f thread-s, disk/decode/migration %.3f thread-s, "
                "binary handles %.3f thread-s, driver restore %.3f thread-s, "
                "driver compile %.3f thread-s\n",
                binary_key_ns.load() / 1e9, binary_load_ns.load() / 1e9,
                binary_handles_ns.load() / 1e9, binary_restore_ns.load() / 1e9,
                binary_compile_ns.load() / 1e9);
    extern std::atomic<u64> g_port_pipeline_total_ns, g_port_pipeline_driver_ns;
    extern std::atomic<u32> g_port_pipelines_built;
    const u32 built = std::max(1u, g_port_pipelines_built.load());
    std::printf("Pipeline build time: %u graphics pipelines, %.1f ms each (driver %.1f ms, the rest "
                "%.1f ms), %.1f s in all\n",
                built, g_port_pipeline_total_ns / 1e6 / built, g_port_pipeline_driver_ns / 1e6 / built,
                (g_port_pipeline_total_ns - g_port_pipeline_driver_ns) / 1e6 / built,
                g_port_pipeline_total_ns / 1e9);
    // The idle threads stay parked (StopWarmUp).
    warm.finished = true;
    PortOverlay::SetBackgroundLoading(0, 0);
    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::StopWarmUp() {
    if (!background) {
        return;
    }
    background->stopping = true;
    background->pool->Drain();
    // ponytail: leaked with its parked threads (this runs at exit only): a thread that ended
    // while the game ran crashed in ntdll (RtlWakeAllConditionVariable). Join them once the
    // cause is known.
    background = nullptr;
    PortOverlay::SetBackgroundLoading(0, 0);
}

void PipelineCache::Sync() {
    StopWarmUp();
    FinishCompilations();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
