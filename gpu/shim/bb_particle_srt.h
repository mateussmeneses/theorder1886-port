// SPDX-License-Identifier: GPL-2.0-or-later
// Particle SRT normalization adapted from Igor Augusto / TOport:
// https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951
// Modified for this port (THEORDER_* configuration and validation differences).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <span>

// The four Order billboard VS programs have 16 guarded pairs of GI textures.
// run55: the live guard count is 7, but the walker also reads the unused tail;
// recycled data there generated 309 shader variants. Keep the live count and
// all active descriptors. Canonicalize only descriptors the shader cannot read.
namespace BbParticleSrt {
struct Layout {
    std::uint32_t words{}, count_word{}, images{}, buffers{}, first_image{}, first_table{}, second_table{};
};
inline Layout Schema(std::uint64_t hash) {
    switch (hash) {
    case 0xc75dd317: case 0xad441fb1: return {700,625,33,8,1,40,168};
    case 0xa8792bbb: case 0x8c8a4817: return {748,649,35,10,0,64,192};
    default: return {};
    }
}
inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("THEORDER_PARTICLE_SRT_FIX");
        if (!v) v = std::getenv("BB_PARTICLE_SRT_FIX");
        return !v || v[0] != '0';
    }();
    return on;
}
struct Result {bool applied{};std::uint32_t active{}, inactive_images{}, changed_words{};};
template<class Info>
Result Normalize(Info& info) {
    const auto schema = Schema(info.pgm_hash);
    if (!schema.words || info.sw_stage != decltype(info.sw_stage)::Vertex ||
        info.flattened_ud_buf.size() != schema.words || info.images.size() != schema.images ||
        info.buffers.size() != schema.buffers || info.samplers.size() != 1 || !info.fmasks.empty()) return {};
    auto& flat = info.flattened_ud_buf;
    const auto active = flat[schema.count_word];
    if (active > 16) return {}; // malformed/new schema: retain the original behavior
    // A hash alone is insufficient: check the actual resource origins as well.
    for (std::uint32_t probe = 0; probe < 16; ++probe) {
        for (std::uint32_t pair = 0; pair < 2; ++pair) {
            const auto& image = info.images[schema.first_image + probe * 2 + pair];
            if (image.is_depth || image.is_array || image.is_written || image.is_r128 ||
                image.post_op != decltype(image.post_op)::None || image.sharp_fetch.load_mask != 255) return {};
            const auto base = (pair ? schema.second_table : schema.first_table) + probe * 8;
            for (std::uint32_t w = 0; w < 8; ++w) if (image.sharp_fetch.offsets[w] != base + w) return {};
        }
    }
    Result result{true, active, 2 * (16 - active), 0};
    for (std::uint32_t probe = active; probe < 16; ++probe) {
        for (const auto table : {schema.first_table, schema.second_table}) {
            for (std::uint32_t w = 0; w < 8; ++w) {
                auto& value = flat[table + probe * 8 + w];
                result.changed_words += value != 0;
                value = 0;
            }
        }
    }
    return result;
}
} // namespace BbParticleSrt
