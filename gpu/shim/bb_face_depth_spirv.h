// SPDX-License-Identifier: GPL-2.0-or-later
// Facial-depth SPIR-V mitigation adapted from Igor Augusto / TOport:
// https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951
// Modified for this port; see docs/RELEASE-v0.7.3-ATTRIBUTION.md.
// Targeted mitigation for facial depth conflicts replay-proven in runs39/48/53.
// This is not a translation of a newly identified guest register.
#pragma once

#include <bit>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace BbFaceDepth {
inline constexpr std::uint64_t VertexHash = 0x0e4fee1c;
inline constexpr std::uint64_t PixelHash = 0x0a54a188;
inline constexpr std::uint64_t InitialPixelHash = 0xf5c17eec;
inline constexpr std::uint64_t CutscenePixelHash = 0x02108015;
inline constexpr std::uint64_t SecondaryVertexHash = 0x418001f4;
inline constexpr std::uint64_t SecondaryPixelHash = 0x39e98e7c;
inline constexpr std::uint64_t CutsceneSecondaryPixelHash = 0xdc16510f;
inline constexpr float Tolerance = 0.00001f;
inline constexpr unsigned CapturedIndexCount = 33354;
inline constexpr unsigned InitialIndexCount = 38166;
inline constexpr unsigned SecondaryIndexCount = 33522;
inline constexpr unsigned CutsceneIndexCount = 38166;

// Terrace signature: captured GPU event 3765. Initial signature: run42 log candidate.
// Neither is a universal mesh identifier; the initial pixel failure is not replay-proven.
inline bool DrawMatches(std::uint64_t ps, bool indexed, unsigned indices, unsigned instances,
                        bool depth_test, bool depth_write, bool less_equal) {
    const unsigned expected = ps == PixelHash ? CapturedIndexCount :
                              ps == InitialPixelHash ? InitialIndexCount :
                              ps == CutscenePixelHash ? CutsceneIndexCount : 0;
    return expected && indexed && indices == expected && instances == 1 && depth_test &&
           !depth_write && less_equal;
}

inline bool Matches(std::uint64_t vs, std::uint64_t ps, unsigned samples,
                    unsigned depth_samples, unsigned colors, bool depth32,
                    bool secondary = false) {
    const bool pair = (vs == VertexHash && (ps == PixelHash || ps == InitialPixelHash ||
                                          ps == CutscenePixelHash)) ||
                      (secondary && vs == SecondaryVertexHash &&
                       (ps == SecondaryPixelHash || ps == CutsceneSecondaryPixelHash));
    return pair && samples == 2 && depth_samples == 2 &&
           colors == 2 && depth32;
}

// Run48 frame9775: pixel history identifies event1337 as the skin draw rejecting
// both MSAA samples at the dots. Target VS replacement passed A/B/A offline.
// The old VS193/f5/18930 guess is excluded. The approved main gates stay original.
inline bool ScopedDrawMatches(std::uint64_t vs, std::uint64_t ps, bool secondary,
                              bool indexed, unsigned indices, unsigned instances,
                              bool depth_test, bool depth_write, bool less_equal) {
    if (vs == VertexHash)
        return DrawMatches(ps, indexed, indices, instances, depth_test, depth_write, less_equal);
    // Run53: skin events3527/3802, both 38166 indices. The PS39 draw still
    // requires 33522; never use the common count alone as a material identity.
    const unsigned expected = ps == SecondaryPixelHash ? SecondaryIndexCount :
                              ps == CutsceneSecondaryPixelHash ? CutsceneIndexCount : 0;
    return secondary && vs == SecondaryVertexHash && expected && indexed &&
           indices == expected && instances == 1 && depth_test &&
           !depth_write && less_equal;
}

// Adds exactly the same Z -= 1e-5 * W operations that passed the captured A/B/A test.
// Unsupported control flow/output layout is left untouched by the caller.
inline std::optional<std::vector<std::uint32_t>> Adjust(std::span<const std::uint32_t> input) {
    using U = std::uint32_t;
    if (input.size() < 5 || input.size() > 262144 || input[0] != 0x07230203 ||
        input[3] == 0 || input[3] > 1048576) return {};
    std::unordered_map<U, U> constants, pointers;
    U position = 0, float_type = 0, vertex_entries = 0;
    std::size_t first_function = 0;
    for (std::size_t i = 5; i < input.size();) {
        const U count = input[i] >> 16, op = input[i] & 65535;
        if (!count || count > input.size() - i) return {};
        if (op == 15) {
            if (count < 4 || input[i + 1] != 0) return {};
            ++vertex_entries;
        }
        if (op == 71 && count == 4 && input[i + 2] == 11 && input[i + 3] == 0)
            position = input[i + 1];
        if (op == 22 && count == 3 && input[i + 2] == 32) float_type = input[i + 1];
        if (op == 43 && count == 4) constants[input[i + 2]] = input[i + 3];
        if (op == 54 && !first_function) first_function = i;
        i += count;
    }
    if (!position || !float_type || !first_function || vertex_entries != 1) return {};
    struct Store { std::size_t after{}; U pointer{}, value{}, block{}; } z, w;
    U block = 0;
    for (std::size_t i = 5; i < input.size();) {
        const U count = input[i] >> 16, op = input[i] & 65535;
        if (op == 248 && count == 2) block = input[i + 1];
        if (op == 65 && count == 5 && input[i + 3] == position) {
            const auto c = constants.find(input[i + 4]);
            if (c != constants.end()) pointers[input[i + 2]] = c->second;
        }
        if (op == 62 && count >= 3) {
            const auto ptr = pointers.find(input[i + 1]);
            if (ptr != pointers.end()) {
                Store s{i + count, input[i + 1], input[i + 2], block};
                if (ptr->second == 2) z = s;
                if (ptr->second == 3) w = s;
            }
        }
        i += count;
    }
    if (!z.after || !w.after || !z.block || z.block != w.block) return {};
    const U constant = input[3], product = input[3] + 1, adjusted = input[3] + 2;
    std::vector<U> out(input.begin(), input.end());
    out[3] += 3;
    const std::size_t insertion = z.after > w.after ? z.after : w.after;
    const U operations[] = {(5u << 16) | 133, float_type, product, w.value, constant,
                            (5u << 16) | 131, float_type, adjusted, z.value, product,
                            (3u << 16) | 62, z.pointer, adjusted};
    out.insert(out.begin() + insertion, std::begin(operations), std::end(operations));
    const U declaration[] = {(4u << 16) | 43, float_type, constant, std::bit_cast<U>(Tolerance)};
    out.insert(out.begin() + first_function, std::begin(declaration), std::end(declaration));
    return out;
}
} // namespace BbFaceDepth
