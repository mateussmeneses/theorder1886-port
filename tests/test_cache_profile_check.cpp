// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// T-024: a shader/pipeline cache written for another Shader::Profile (other GPU vendor workarounds,
// other supported features, an older translation revision, or a struct of another size) must be
// reported as not matching, so PipelineCache::CheckCacheProfile clears and rebuilds it.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shader_recompiler/profile_blob.h"

static std::vector<u8> Blob(const Shader::Profile& p) {
    std::vector<u8> bytes(sizeof(p));
    std::memcpy(bytes.data(), &p, sizeof(p));
    return bytes;
}

int main() {
    using Shader::Profile;
    using Shader::StoredProfileMatches;

    Profile host{};
    host.subgroup_size = 32;
    host.support_int64 = true;
    host.needs_clip_distance_emulation = true; // NVIDIA proprietary
    host.needs_ssbo_store_load_barrier = true;

    // The same profile loads.
    assert(StoredProfileMatches(Blob(host), host));

    // Each of these, written by another GPU/driver/build, is ignored.
    auto other = [&](auto&& edit) {
        Profile p = host;
        edit(p);
        return !StoredProfileMatches(Blob(p), host);
    };
    assert(other([](Profile& p) { p.needs_clip_distance_emulation = false; })); // AMD/Intel
    assert(other([](Profile& p) { p.needs_ssbo_store_load_barrier = false; }));
    assert(other([](Profile& p) { p.needs_lds_barriers = true; }));
    assert(other([](Profile& p) { p.needs_unorm_fixup = true; }));
    assert(other([](Profile& p) { p.subgroup_size = 64; }));
    assert(other([](Profile& p) { p.supports_fragment_shader_barycentric = true; }));
    assert(other([](Profile& p) { p.support_int64 = false; }));
    assert(other([](Profile& p) { p.supported_spirv = 0x00010300; }));
    assert(other([](Profile& p) { p.max_shared_memory_size = 1; }));
    assert(other([](Profile& p) { p.translation_revision = p.translation_revision - 1; }));
    assert(other([](Profile& p) { p.dref_broadcast = true; }));

    // Missing, truncated or longer blobs (the struct changed between builds) are ignored.
    assert(!StoredProfileMatches({}, host));
    auto shorter = Blob(host);
    shorter.pop_back();
    assert(!StoredProfileMatches(shorter, host));
    auto longer = Blob(host);
    longer.push_back(0);
    assert(!StoredProfileMatches(longer, host));

    std::puts("cache-profile-check: ok");
    return 0;
}
