// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include "shader_recompiler/profile.h"

namespace Shader {

/// port: whether the Profile blob stored with a shader/pipeline cache was written for exactly
/// `current`. The stored blob is the raw struct; a different size (the struct changed) or any
/// differing field (GPU vendor workarounds, supported features, translation_revision) means the
/// cached SPIR-V, metas and pipeline keys were produced for another profile and must not be used.
inline bool StoredProfileMatches(const std::vector<u8>& blob, const Profile& current) {
    if (blob.size() != sizeof(Profile)) {
        return false;
    }
    Profile stored{};
    std::memcpy(&stored, blob.data(), sizeof(stored));
    return stored == current;
}

} // namespace Shader
