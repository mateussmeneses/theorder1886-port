// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// BbParticleSrt::Normalize: the four billboard VS programs keep their live GI probes and get the
// unused tail of both tables zeroed, with this port's buffer count (one fewer than TOport's);
// any other layout is left untouched.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bb_particle_srt.h"

namespace {
enum class Stage { Vertex, Fragment };
enum class PostOp { None, Other };
struct Fetch {
    std::uint32_t load_mask = 255;
    std::array<std::uint32_t, 8> offsets{};
};
struct Image {
    bool is_depth = false, is_array = false, is_written = false, is_r128 = false;
    PostOp post_op = PostOp::None;
    Fetch sharp_fetch;
};
struct Buffer {
    Fetch sharp_fetch;
};
struct Info {
    std::uint64_t pgm_hash = 0xa8792bbb;
    Stage sw_stage = Stage::Vertex;
    std::vector<std::uint32_t> flattened_ud_buf;
    std::vector<Image> images;
    std::vector<Buffer> buffers;
    std::vector<int> samplers, fmasks;
};

// Schema of 0xa8792bbb: 748 words, count at 649, 35 images (first 0), 10 buffers in TOport,
// tables at 64 and 192.
Info MakeInfo(std::size_t buffers, std::uint32_t active) {
    Info info;
    info.flattened_ud_buf.assign(748, 0xdeadbeef);
    info.flattened_ud_buf[649] = active;
    info.images.resize(35);
    for (std::uint32_t probe = 0; probe < 16; ++probe) {
        for (std::uint32_t pair = 0; pair < 2; ++pair) {
            auto& image = info.images[probe * 2 + pair];
            const std::uint32_t base = (pair ? 192 : 64) + probe * 8;
            for (std::uint32_t w = 0; w < 8; ++w) image.sharp_fetch.offsets[w] = base + w;
        }
    }
    info.buffers.resize(buffers);
    for (std::size_t i = 0; i < buffers; ++i) info.buffers[i].sharp_fetch.offsets.fill(8 + std::uint32_t(i));
    info.samplers.resize(1);
    return info;
}
} // namespace

int main() {
    // This port's layout (9 buffers): probes 7..15 of both tables are zeroed, 0..6 kept.
    auto info = MakeInfo(9, 7);
    const auto result = BbParticleSrt::Normalize(info);
    assert(result.applied && result.active == 7 && result.inactive_images == 18);
    for (std::uint32_t probe = 0; probe < 16; ++probe) {
        for (const std::uint32_t table : {64u, 192u}) {
            for (std::uint32_t w = 0; w < 8; ++w) {
                const auto value = info.flattened_ud_buf[table + probe * 8 + w];
                assert(probe < 7 ? value == 0xdeadbeef : value == 0);
            }
        }
    }
    assert(info.flattened_ud_buf[649] == 7 && info.flattened_ud_buf[0] == 0xdeadbeef);
    // TOport's count (10) still applies.
    auto toport = MakeInfo(10, 7);
    assert(BbParticleSrt::Normalize(toport).applied);
    // Other layouts are untouched: two buffers fewer, a moved descriptor, too many probes.
    auto fewer = MakeInfo(8, 7);
    assert(!BbParticleSrt::Normalize(fewer).applied && fewer.flattened_ud_buf[64 + 7 * 8] == 0xdeadbeef);
    auto moved = MakeInfo(9, 7);
    moved.images[5].sharp_fetch.offsets[3] += 1;
    assert(!BbParticleSrt::Normalize(moved).applied);
    auto aliased = MakeInfo(9, 7); // a buffer that reads a word about to be zeroed
    aliased.buffers[2].sharp_fetch.offsets[0] = 192 + 15 * 8;
    assert(!BbParticleSrt::Normalize(aliased).applied);
    auto many = MakeInfo(9, 17);
    assert(!BbParticleSrt::Normalize(many).applied);
    auto other = MakeInfo(9, 7);
    other.pgm_hash = 0x12345678;
    assert(!BbParticleSrt::Normalize(other).applied);
    std::puts("particle-srt ok");
    return 0;
}
