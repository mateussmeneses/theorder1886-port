// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 56;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

using SharpLocation = u16;

constexpr SharpLocation UNKNOWN_LOCATION = std::numeric_limits<u16>::max();

template <typename T>
struct SharpFetch {
    static constexpr std::size_t N = sizeof(T) / sizeof(u32);
    static_assert(N <= 8);

    enum class Summary : u8 {
        SingleLoad,
        MultiLoad,
        Invalid,
    };

    Summary summary{Summary::MultiLoad};
    std::array<u32, N> immediates;
    std::array<SharpLocation, N> offsets;
    u8 load_mask;

    bool operator==(const SharpFetch&) const = default;

    template <u32 num_dwords = N>
        requires(num_dwords <= N)
    constexpr bool Fetch(const u32* flatbuf, T* out) const {
        if (summary == Summary::SingleLoad) [[likely]] {
            std::memcpy(out, flatbuf + offsets[0], num_dwords * sizeof(u32));
            return true;
        } else if (summary == Summary::Invalid) [[unlikely]] {
            return false;
        }
        u8 mask = load_mask;
        std::array<u32, num_dwords> out_dw;
        for (u32 i = 0; i < num_dwords; i++) {
            out_dw[i] = (mask & 1) ? flatbuf[offsets[i]] : immediates[i];
            mask >>= 1;
        }
        std::memcpy(out, out_dw.data(), sizeof(out_dw));
        return true;
    }
};

enum class SharpFetchPostOp : u8 {
    None,
    // For buffers
    BitwiseOrDw1WithImm,
    OffsetByProgramBase,
    ClearStrideAndBitshiftDw2WithImm,
    // For images,
    ConvertCubeTo2DArray,
    // For samplers
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};

enum class BufferType : u8 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
};

struct BufferResource {
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types{};
    BufferType buffer_type{};
    bool is_written{};
    bool is_formatted{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};
    u32 post_op_dw2_shift{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer;
        if (!sharp_fetch.Fetch(info.FlatUserData().data(), &buffer)) {
            return AmdGpu::Buffer::Null();
        }
        if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            reinterpret_cast<u32*>(&buffer)[1] |= post_op_dw1_mask;
        } else if (post_op == SharpFetchPostOp::ClearStrideAndBitshiftDw2WithImm) {
            buffer.stride = 0;
            buffer.num_records <<= post_op_dw2_shift;
        } else if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.ProgramBase();
        }
        if (!buffer.Valid()) {
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

// Adapted from TOport v0.1.5 (Igor Augusto and contributors), GPL-2.0-or-later:
// https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/19779e291dda6545961469046430b93b5505e801
// The Order's particle/point-light compute shader has 16 unrolled lights. Descriptor
// words for unused lights can contain stale addresses and create unique specializations.
// Experimental until a matched CUSA00035 frame/compile A/B: enable with
// THEORDER_POINT_LIGHT_GUARD=1 (unset / 0 leaves the existing behavior unchanged).
inline bool g_point_light_guard = [] {
    const char* value = std::getenv("THEORDER_POINT_LIGHT_GUARD");
    return value && value[0] == '1' && value[1] == '\0';
}();

template <typename InfoT>
bool PointLightImageUnread(const InfoT& info, u32 sharp_offset) {
    if (!g_point_light_guard || info.pgm_hash != 0x804aeead) {
        return false;
    }
    const auto flat = info.FlatUserData();
    if (flat.size() != 708) {
        return false; // Guard table only validated for this layout.
    }
    const u32 count = flat[657];
    const u32 mask = flat[655];
    if (sharp_offset >= 68 && sharp_offset < 196 && (sharp_offset - 68) % 8 == 0) {
        return (sharp_offset - 68) / 8 >= count;
    }
    if (sharp_offset >= 196 && sharp_offset < 324 && (sharp_offset - 196) % 8 == 0) {
        const u32 light = (sharp_offset - 196) / 8;
        return light >= count || ((mask >> light) & 1u) == 0;
    }
    return false;
}

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

/// port: the GPU backend's guest memory check (MemoryManager::IsValidGpuMapping); unset in
/// tools without a guest.
inline bool (*g_valid_image_address)(VAddr address) = nullptr;

struct ImageResource {
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    /// port: read other than by normalized sampling without offsets (texel loads, size
    /// queries, offsets): needs the native-size image, not a reduced scene proxy.
    bool needs_native{};
    u8 constant_mip_index{};
    MipStorageFallbackMode mip_fallback_mode{};
    SharpFetchPostOp post_op{};

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (!Fetch(info.FlatUserData().data(), &image)) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (PointLightImageUnread(info, sharp_fetch.offsets[0])) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        // port: stale T#s left in slots a dispatch does not sample (The Order's material and
        // lighting compute shaders) carry garbage: no format, an empty mip range, a null page.
        // The rasterizer binds them as null anyway; reading them as Null here too keeps their
        // random view types out of the shader specialization, where each combination cost a
        // new permutation and a driver compile.
        // GCN GPU virtual addresses are 40-bit: a higher base cannot be memory.
        if (!image.Valid() || image.Address() < 0x10000 || image.Address() >= (1ULL << 40) ||
            image.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid ||
            image.base_level > image.last_level || !Plausible(image) ||
            (g_valid_image_address && !g_valid_image_address(image.Address()))) {
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    /// port: what a stale T# shows that a texture the dispatch samples cannot.
    constexpr bool Plausible(const AmdGpu::Image& image) const {
        const u64 width = image.width, height = image.height;
        const u64 layers = image.NumLayers();
        const bool is_3d = image.GetType() == AmdGpu::ImageType::Color3D;
        // The limits the rasterizer binds a texture under (BindTextures).
        if (width >= 8192 || height >= 8192 || layers > 2048 ||
            (is_3d && image.depth >= 2048) ||
            (layers > 64 && (width >= 256 || height >= 256)) ||
            width * height * layers > (256u << 20) ||
            (layers > 16 && (width >= 2048 || height >= 2048))) {
            return false;
        }
        if (!image.IsGuestMsaa()) {
            const u64 depth = is_3d ? u64(image.depth) + 1 : 1;
            const u32 levels = std::bit_width(std::max({width + 1, height + 1, depth}));
            if (image.base_level >= levels) {
                return false;
            }
        }
        // A cube is sampled as an array (MIMG DA bit), a 3D image never is. Only for plain
        // sampling: a size query need not set the bit.
        if (!is_written && !is_atomic && !needs_native) {
            return image.GetType() == AmdGpu::ImageType::Cube ? is_array : !(is_3d && is_array);
        }
        return true;
    }

    constexpr bool Fetch(const u32* flatbuf, AmdGpu::Image* out) const {
        if (!is_r128) {
            // Fetch full 8 byte T#
            return sharp_fetch.Fetch(flatbuf, out);
        }
        // Fetch r128 T# and fix pitch
        if (!sharp_fetch.Fetch<4>(flatbuf, out)) {
            return false;
        }
        out->pitch = out->width;
        return true;
    }

    u32 NumBindings(const auto& info) const {
        if (mip_fallback_mode != MipStorageFallbackMode::DynamicIndex) {
            return 1;
        }
        const AmdGpu::Image tsharp = GetSharp(info);
        return tsharp.last_level >= tsharp.base_level ? (tsharp.last_level - tsharp.base_level + 1)
                                                      : 1;
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{};
    bool is_depth{};
    u64 associated_images{};

    constexpr AmdGpu::Sampler GetSharp(const auto& info,
                                       bool clamp_integer_filters = true) const noexcept {
        AmdGpu::Sampler sampler{};
        sharp_fetch.Fetch(info.FlatUserData().data(), &sampler);
        if (post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const u32 tsharp_dw3 = info.FlatUserData()[post_op_tsharp_dw3_off];
            if (((tsharp_dw3 >> 12) & 0xff) == 0) {
                sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        } else if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        // port: Vulkan disallows linear/anisotropic filtering when sampling an integer image
        // (VUID-vkCmdDispatchIndirect-magFilter-04553); GCN forces point filtering on integer T#s.
        if (clamp_integer_filters && associated_images != 0 &&
            (sampler.xy_mag_filter != AmdGpu::Filter::Point ||
             sampler.xy_min_filter != AmdGpu::Filter::Point ||
             sampler.z_filter != 0 ||
             sampler.mip_filter == AmdGpu::MipFilter::Linear ||
             sampler.max_aniso != AmdGpu::AnisoRatio::One)) {
            u64 mask = associated_images;
            while (mask != 0) {
                const u32 idx = std::countr_zero(mask);
                mask &= mask - 1;
                if (idx < info.images.size() &&
                    AmdGpu::IsInteger(info.images[idx].GetSharp(info).GetNumberFmt())) {
                    sampler.xy_mag_filter.Assign(AmdGpu::Filter::Point);
                    sampler.xy_min_filter.Assign(AmdGpu::Filter::Point);
                    sampler.z_filter.Assign(0);
                    if (sampler.mip_filter == AmdGpu::MipFilter::Linear) {
                        sampler.mip_filter.Assign(AmdGpu::MipFilter::Point);
                    }
                    sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
                    break;
                }
            }
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpLocation sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 BufOffsetIndex = 4;
    /// port: members after the three buf_offsets vectors (see spirv_emit_context.cpp)
    static constexpr u32 MotionParamIndex = BufOffsetIndex + 3;
    static constexpr u32 SceneSizeIndex = MotionParamIndex + 1;
    static constexpr u32 MotionParamsAddressIndex = SceneSizeIndex + 1;
    static constexpr u32 MotionPositionsAddressIndex = MotionParamsAddressIndex + 1;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u8, NUM_BUFFERS> buf_offsets;
    u32 motion_param; ///< port: object motion parameter index (0: off)
    u32 scene_size; ///< reduced width | height << 16; zero means native pixel coordinates
    u64 motion_params_address;
    u64 motion_positions_address;

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
inline constexpr u32 PushDataBaseSize = offsetof(PushData, motion_params_address);
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");
static_assert(offsetof(PushData, motion_positions_address) ==
              offsetof(PushData, motion_params_address) + sizeof(u64));

} // namespace Shader
