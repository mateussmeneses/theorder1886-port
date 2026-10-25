// SPDX-License-Identifier: GPL-2.0-or-later
// Facial-depth mitigation adapted from Igor Augusto / TOport:
// https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951
// Modified for this port (THEORDER_* configuration, defaults and diagnostics).
#include "bb_face_depth_fix.h"
#include "bb_face_depth_spirv.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace BbFaceDepth {
bool Enabled() {
    static const bool enabled = [] {
        const char* p = std::getenv("THEORDER_FACE_DEPTH_FIX");
        if (!p) p = std::getenv("BB_FACE_DEPTH_FIX");
        const bool active = !p || std::strcmp(p, "0") != 0;
        if (active)
            std::printf("Face depth fix: enabled; separate terrace/initial-scene draw gates\n");
        return active;
    }();
    return enabled;
}
bool SecondaryEnabled() {
    static const bool enabled = [] {
        const char* p = std::getenv("THEORDER_FACE_SECONDARY_FIX");
        if (!p) p = std::getenv("BB_FACE_SECONDARY_FIX");
        const bool active = Enabled() && (!p || std::strcmp(p, "0") != 0);
        if (active)
            std::printf("Face secondary fix: VS418001f4 PS39e98e7c/33522 or PSdc16510f/38166; captured facial pixel proofs run48/run53\n");
        return active;
    }();
    return enabled;
}
namespace {
std::mutex sources_mutex;
struct Source { u64 vertex_hash; std::vector<u32> words; };
std::unordered_map<VkShaderModule, Source> sources;
}
void Register(vk::ShaderModule module, std::span<const u32> original, u64 vertex_hash) {
    if (!Enabled() || (vertex_hash != VertexHash &&
        !(SecondaryEnabled() && vertex_hash == SecondaryVertexHash)) || original.size() > 262144) return;
    std::scoped_lock lock{sources_mutex};
    const auto key = static_cast<VkShaderModule>(module);
    // Separate quotas: secondary permutations cannot displace the approved VS.
    // At most 32 primary + 8 secondary modules / 40 MiB of retained source.
    const auto count = std::count_if(sources.begin(), sources.end(), [vertex_hash](const auto& item) {
        return item.second.vertex_hash == vertex_hash;
    });
    if (count >= (vertex_hash == VertexHash ? 32 : 8) && !sources.contains(key)) return;
    sources.insert_or_assign(key, Source{vertex_hash, std::vector<u32>(original.begin(), original.end())});
}
vk::UniqueShaderModule Create(vk::ShaderModule original, vk::Device device, u64 pixel_hash) {
    if (pixel_hash != PixelHash && pixel_hash != InitialPixelHash &&
        pixel_hash != CutscenePixelHash && pixel_hash != SecondaryPixelHash &&
        pixel_hash != CutsceneSecondaryPixelHash) return {};
    std::optional<std::vector<u32>> adjusted;
    u64 vertex_hash{};
    {
        std::scoped_lock lock{sources_mutex};
        const auto it = sources.find(static_cast<VkShaderModule>(original));
        if (it == sources.end()) return {};
        vertex_hash = it->second.vertex_hash;
        const bool primary = vertex_hash == VertexHash &&
                             (pixel_hash == PixelHash || pixel_hash == InitialPixelHash ||
                              pixel_hash == CutscenePixelHash);
        const bool secondary = SecondaryEnabled() && vertex_hash == SecondaryVertexHash &&
                               (pixel_hash == SecondaryPixelHash || pixel_hash == CutsceneSecondaryPixelHash);
        if (!primary && !secondary) return {};
        adjusted = Adjust(it->second.words);
    }
    if (!adjusted) {
        std::fprintf(stderr, "Face depth fix: unsupported SPIR-V; original kept\n");
        return {};
    }
    auto module = vk::UniqueShaderModule(Vulkan::CompileSPV(*adjusted, device), device);
    static std::atomic<unsigned> logged{};
    if (logged.fetch_add(1) < 12)
        std::printf("Face depth fix: VS %08llx PS %08llx, Z/W -1e-5, original cache kept\n",
                    static_cast<unsigned long long>(vertex_hash),
                    static_cast<unsigned long long>(pixel_hash));
    return module;
}
void LogDraw(u64 vs, u64 ps, bool indexed, u32 indices, u32 instances, u32 samples,
             u32 depth_samples, bool depth_test, bool depth_write, u32 compare,
             bool adjusted, u64 geometry_address, bool indirect) {
    if (!Enabled() || (ps != PixelHash && ps != InitialPixelHash && ps != CutscenePixelHash &&
        !(SecondaryEnabled() && (ps == SecondaryPixelHash || ps == CutsceneSecondaryPixelHash)))) return;
    const unsigned group = ps == PixelHash ? 0 : ps == InitialPixelHash ? 1 :
                           ps == SecondaryPixelHash ? 2 : ps == CutscenePixelHash ? 3 : 4;
    using Signature = std::tuple<u64, u64, bool, u32, u32, u32, u32, bool, bool, u32, bool, u64, bool>;
    static std::mutex mutex;
    static std::set<Signature> seen;
    static unsigned count[5]{};
    static bool limited[5]{};
    std::scoped_lock lock{mutex};
    if (count[group] >= 64) {
        if (!std::exchange(limited[group], true))
            std::printf("Face draw scope: diagnostic limit 64 signatures PS %08llx\n",
                        static_cast<unsigned long long>(ps));
        return;
    }
    if (!seen.emplace(vs, ps, indexed, indices, instances, samples,
                      depth_samples, depth_test, depth_write, compare,
                      adjusted, geometry_address, indirect).second) return;
    ++count[group];
    const auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static bool file_attempted = false;
    static std::FILE* trace{};
    if (!std::exchange(file_attempted, true)) {
        if (const char* path = std::getenv("THEORDER_FACE_DRAW_TRACE_FILE")) {
            trace = std::fopen(path, "w");
            if (trace) std::fprintf(trace, "epoch_ms,vs,ps,indexed,indices,instances,samples,depth_samples,depth_test,depth_write,compare,adjusted,route,geometry_address\n");
        }
    }
    if (trace) {
        std::fprintf(trace, "%llu,%08llx,%08llx,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%llx\n",
            static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(vs),
            static_cast<unsigned long long>(ps), unsigned(indexed), indices, instances,
            samples, depth_samples, unsigned(depth_test), unsigned(depth_write), compare,
            unsigned(adjusted), indirect ? "indirect" : "direct",
            static_cast<unsigned long long>(geometry_address));
        std::fflush(trace);
    }
}
} // namespace BbFaceDepth
