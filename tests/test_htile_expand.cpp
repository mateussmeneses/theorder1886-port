// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>

#include "video_core/renderer_vulkan/htile_expand.h"

int main() {
    using VideoCore::HtileUncompressedSize;
    // Sun cascade maps: 1024x1024, two array layers. Live RX 9070 XT boot log reports
    // "HTILE: ... marked uncompressed, 131072 bytes" for these surfaces.
    assert(HtileUncompressedSize(1024, 1024, 2) == 131072);
    assert(HtileUncompressedSize(1024, 1024, 1) == 65536);
    // Scene depth at the retail cinema scope 1920x800, single layer:
    // tiles_x = AlignUp(240, 64) = 256, tiles_y = AlignUp(101, 64) = 128 -> 131072.
    assert(HtileUncompressedSize(1920, 800, 1) == 131072);
    // Half-res shadow/blur target 960x400: tiles_x = AlignUp(120, 64) = 128,
    // tiles_y = AlignUp(50, 64) = 64 -> 32768.
    assert(HtileUncompressedSize(960, 400, 1) == 32768);
    // Sub-tile surfaces still reserve one padded 64x64 tile block: 8x8 -> 16384.
    assert(HtileUncompressedSize(8, 8, 1) == 64 * 64 * 4);
    assert(HtileUncompressedSize(1, 1, 1) == 64 * 64 * 4);
    // Zero layers behave as one (the product clamps, never divides).
    assert(HtileUncompressedSize(1024, 1024, 0) == 65536);
    std::puts("htile-expand ok");
    return 0;
}
