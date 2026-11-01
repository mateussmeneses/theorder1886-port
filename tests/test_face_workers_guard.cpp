// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

int main() {
    using Vulkan::PipelineCache;
    // Verify all 9 facial shaders identified in TOport commit a5327ef
    constexpr u64 expected_face_shaders[] = {
        0x193274a4, 0x0e4fee1c, 0xf049dbf2, 0x418001f4,
        0xf5c17eec, 0x39e98e7c, 0xdc16510f, 0x02108015,
        0x0a54a188
    };
    for (u64 hash : expected_face_shaders) {
        assert(PipelineCache::IsFaceShader(hash));
    }
    // Verify non-facial shaders are not falsely identified
    assert(!PipelineCache::IsFaceShader(0x0));
    assert(!PipelineCache::IsFaceShader(0x7550d841));
    assert(!PipelineCache::IsFaceShader(0x7b1b18c9));
    assert(!PipelineCache::IsFaceShader(0x12345678));

    // Default: face workers are disabled (specialized on GPU thread only)
    assert(!PipelineCache::FaceWorkersAllowed());

    std::puts("face-workers-guard-test: PASS");
    return 0;
}
