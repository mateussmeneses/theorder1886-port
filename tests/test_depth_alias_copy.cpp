// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/texture_cache/depth_alias_copy.h"

int main() {
    using namespace VideoCore;
    ImageInfo depth{}, color{};
    depth.type = color.type = AmdGpu::ImageType::Color2D;
    depth.size = color.size = {1280, 720, 1};
    depth.pitch = color.pitch = 1280;
    depth.resources = color.resources = {1, 1};
    depth.props.is_depth = true;
    depth.pixel_format = vk::Format::eD32Sfloat;
    depth.num_bits = 32;
    for (u32 count : {2u, 4u}) {
        depth.num_samples = count;
        color.num_samples = 1;
        color.num_bits = 32 * count;
        color.pixel_format = count == 2 ? vk::Format::eR32G32Sfloat : vk::Format::eR32G32B32A32Sfloat;
        assert(PlanDepthAliasCopy(depth, color, false) == DepthAliasCopy::DepthToColor);
        assert(PlanDepthAliasCopy(color, depth, false) == DepthAliasCopy::ColorToDepth);
        assert(PlanDepthAliasCopy(depth, depth, false) == DepthAliasCopy::Direct);
        auto bad = color; bad.size.width++;
        assert(PlanDepthAliasCopy(depth, bad, false) == DepthAliasCopy::Unsupported);
        bad = color; bad.resources.layers = 2;
        assert(PlanDepthAliasCopy(depth, bad, false) == DepthAliasCopy::Unsupported);
        bad = color; bad.num_bits = 16;
        assert(PlanDepthAliasCopy(depth, bad, false) == DepthAliasCopy::Unsupported);
        bad = depth; bad.props.has_stencil = true;
        assert(PlanDepthAliasCopy(bad, color, false) == DepthAliasCopy::Unsupported);
    }
    depth.num_samples = 8;
    assert(PlanDepthAliasCopy(depth, color, false) == DepthAliasCopy::Unsupported);
    depth.num_samples = color.num_samples = 1;
    color.num_bits = 32; color.pixel_format = vk::Format::eR32Sfloat;
    assert(PlanDepthAliasCopy(depth, color, false) == DepthAliasCopy::Buffer);
    assert(PlanDepthAliasCopy(depth, color, true) == DepthAliasCopy::Buffer);
    auto stencil = depth; stencil.props.has_stencil = true;
    assert(PlanDepthAliasCopy(stencil, stencil, true) == DepthAliasCopy::Unsupported);
    color.pixel_format = vk::Format::eR8G8B8A8Unorm;
    assert(PlanDepthAliasCopy(depth, color, true) == DepthAliasCopy::Unsupported);
    std::puts("depth-alias-copy-test: PASS");
    return 0;
}
