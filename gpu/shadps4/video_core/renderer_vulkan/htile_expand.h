// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>

#include "common/alignment.h"
#include "common/types.h"

namespace VideoCore {

// The byte count of the HTILE Zmask that covers a depth surface: one u32 per 8x8-pixel tile,
// the tile grid padded to whole 64-tile rows, times the array layers. BeginRenderingFull
// fills this range with the uncompressed mask when a pass writes depth, so later compute
// reads (the EVSM conversion 0x8ea0ef5b) see caster geometry instead of the cleared state.
// Values verified live on RX 9070 XT: the sun cascade maps (1024x1024, 2 layers) report
// "HTILE: ... marked uncompressed, 131072 bytes".
[[nodiscard]] inline u64 HtileUncompressedSize(u32 width, u32 height, u32 layers) {
    const u32 tiles_x = Common::AlignUp((width + 7) / 8, 64u);
    const u32 tiles_y = Common::AlignUp((height + 7) / 8, 64u);
    return u64(tiles_x) * tiles_y * 4 * std::max<u32>(layers, 1u);
}

} // namespace VideoCore
