// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {
enum class DepthAliasCopy { Unsupported, Direct, Buffer, ColorToDepth, DepthToColor };

// A depth sample and a packed color channel represent the same guest dword.
// This is a bit-preserving alias conversion, not a depth resolve/average.
inline DepthAliasCopy PlanDepthAliasCopy(const ImageInfo& src, const ImageInfo& dst,
                                        bool maintenance8) {
    if (src.size != dst.size || src.pitch != dst.pitch ||
        src.resources.levels != dst.resources.levels ||
        src.resources.layers != dst.resources.layers) return DepthAliasCopy::Unsupported;
    const auto is_2d = [](AmdGpu::ImageType type) {
        return type == AmdGpu::ImageType::Color2D || type == AmdGpu::ImageType::Color2DMsaa ||
               type == AmdGpu::ImageType::Color2DArray;
    };
    if (src.num_samples == dst.num_samples && src.pixel_format == dst.pixel_format &&
        !src.props.has_stencil && !dst.props.has_stencil) return DepthAliasCopy::Direct;
    if (src.num_samples == 1 && dst.num_samples == 1) {
        // Cross-aspect copies have an explicit Vulkan compatibility list;
        // maintenance8 alone does not make arbitrary formats compatible.
        const bool float_alias =
            (src.pixel_format == vk::Format::eD32Sfloat && dst.pixel_format == vk::Format::eR32Sfloat) ||
            (dst.pixel_format == vk::Format::eD32Sfloat && src.pixel_format == vk::Format::eR32Sfloat);
        if (float_alias && !src.props.has_stencil && !dst.props.has_stencil &&
            src.resources.levels == 1 && src.resources.layers == 1) {
            return maintenance8 ? DepthAliasCopy::Direct : DepthAliasCopy::Buffer;
        }
        return DepthAliasCopy::Unsupported;
    }
    if (src.resources.levels != 1 || src.resources.layers != 1 || src.size.depth != 1 ||
        !is_2d(src.type) || !is_2d(dst.type) ||
        src.props.has_stencil || dst.props.has_stencil ||
        src.num_bits * src.num_samples != dst.num_bits * dst.num_samples) {
        return DepthAliasCopy::Unsupported;
    }
    const auto packed_format = [](u32 samples) {
        switch (samples) {
        case 2: return vk::Format::eR32G32Sfloat;
        case 4: return vk::Format::eR32G32B32A32Sfloat;
        default: return vk::Format::eUndefined;
        }
    };
    if (src.num_samples == 1 && dst.num_samples > 1 && dst.props.is_depth &&
        dst.pixel_format == vk::Format::eD32Sfloat &&
        src.pixel_format == packed_format(dst.num_samples)) return DepthAliasCopy::ColorToDepth;
    if (dst.num_samples == 1 && src.num_samples > 1 && src.props.is_depth &&
        src.pixel_format == vk::Format::eD32Sfloat &&
        dst.pixel_format == packed_format(src.num_samples)) return DepthAliasCopy::DepthToColor;
    return DepthAliasCopy::Unsupported;
}
} // namespace VideoCore
