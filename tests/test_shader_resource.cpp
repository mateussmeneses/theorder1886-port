// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port_toggles.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    using namespace Shader;

    // 1. SamplerResource::GetSharp integer-image filter clamping (VUID-04553).
    {
        Info info{};
        std::array<u32, 16> user_data{};
        info.user_data = user_data;

        AmdGpu::Image float_tsharp{};
        float_tsharp.base_address = 0x1000u; // 0x100000 valid GPU VA
        float_tsharp.width = 63;
        float_tsharp.height = 63;
        float_tsharp.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
        float_tsharp.data_format = static_cast<u64>(AmdGpu::DataFormat::Format8_8_8_8);
        float_tsharp.num_format = static_cast<u64>(AmdGpu::NumberFormat::Unorm);
        float_tsharp.base_level = 0;
        float_tsharp.last_level = 3;

        AmdGpu::Image uint_tsharp = float_tsharp;
        uint_tsharp.data_format = static_cast<u64>(AmdGpu::DataFormat::Format32_32);
        uint_tsharp.num_format = static_cast<u64>(AmdGpu::NumberFormat::Uint);

        AmdGpu::Sampler linear_ssharp{};
        linear_ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
        linear_ssharp.xy_min_filter.Assign(AmdGpu::Filter::AnisoLinear);
        linear_ssharp.z_filter.Assign(1);
        linear_ssharp.mip_filter.Assign(AmdGpu::MipFilter::Linear);
        linear_ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::Sixteen);

        ImageResource img_float{};
        std::memcpy(img_float.sharp_fetch.immediates.data(), &float_tsharp, sizeof(float_tsharp));
        info.images.push_back(img_float);

        ImageResource img_uint{};
        std::memcpy(img_uint.sharp_fetch.immediates.data(), &uint_tsharp, sizeof(uint_tsharp));
        info.images.push_back(img_uint);

        SamplerResource samp{};
        std::memcpy(samp.sharp_fetch.immediates.data(), &linear_ssharp, sizeof(linear_ssharp));
        samp.associated_images = (1ULL << 0); // float image only

        const auto s_float = samp.GetSharp(info);
        assert(s_float.xy_mag_filter == AmdGpu::Filter::Bilinear);
        assert(s_float.xy_min_filter == AmdGpu::Filter::AnisoLinear);
        assert(s_float.z_filter == 1);
        assert(s_float.mip_filter == AmdGpu::MipFilter::Linear);
        assert(s_float.max_aniso == AmdGpu::AnisoRatio::Sixteen);

        // Associate integer image at index 1 as well.
        samp.associated_images = (1ULL << 0) | (1ULL << 1);
        const auto s_clamped = samp.GetSharp(info);
        assert(s_clamped.xy_mag_filter == AmdGpu::Filter::Point);
        assert(s_clamped.xy_min_filter == AmdGpu::Filter::Point);
        assert(s_clamped.z_filter == 0);
        assert(s_clamped.mip_filter == AmdGpu::MipFilter::Point);
        assert(s_clamped.max_aniso == AmdGpu::AnisoRatio::One);

        // Specialization hot path skips integer-image clamping when clamp_integer_filters == false.
        const auto s_raw = samp.GetSharp(info, false);
        assert(s_raw.xy_mag_filter == AmdGpu::Filter::Bilinear);
        assert(s_raw.mip_filter == AmdGpu::MipFilter::Linear);
    }

    // 2. ImageResource::NumBindings fast path and wraparound guard.
    {
        Info info{};
        std::array<u32, 16> user_data{};
        info.user_data = user_data;

        AmdGpu::Image tsharp{};
        tsharp.base_address = 0x1000u;
        tsharp.width = 63;
        tsharp.height = 63;
        tsharp.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
        tsharp.data_format = static_cast<u64>(AmdGpu::DataFormat::Format8_8_8_8);
        tsharp.num_format = static_cast<u64>(AmdGpu::NumberFormat::Unorm);
        tsharp.base_level = 1;
        tsharp.last_level = 4;

        ImageResource img{};
        std::memcpy(img.sharp_fetch.immediates.data(), &tsharp, sizeof(tsharp));
        img.is_written = true;
        img.mip_fallback_mode = MipStorageFallbackMode::ConstantIndex;
        assert(img.NumBindings(info) == 1);

        img.mip_fallback_mode = MipStorageFallbackMode::DynamicIndex;
        assert(img.NumBindings(info) == 4);

        // Corrupted/invalid T# where last_level < base_level must not wrap around unsigned math.
        tsharp.base_level = 5;
        tsharp.last_level = 2;
        std::memcpy(img.sharp_fetch.immediates.data(), &tsharp, sizeof(tsharp));
        assert(img.NumBindings(info) == 1);
    }

    // 3. ResourcePatchingPass populates SamplerResource::associated_images.
    {
        Info info{};
        Profile profile{};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        IR::TextureInstInfo flags{};
        const auto vec4_zero =
            ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f), ir.Imm32(0.0f), ir.Imm32(0.0f));
        const auto sample = ir.ImageSampleRaw(ir.Imm32(0), ir.Imm32(0), vec4_zero, vec4_zero,
                                              vec4_zero, ir.Imm32(0.0f), flags);
        Optimization::ResourceDiscoveryList resources;
        auto& usage = resources.emplace_back(sample.Inst());
        usage.sharps[0].num_dwords = 8;
        usage.sharps[0].dwords = {ir.Imm32(1u), ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(0u),
                                  ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(0u)};
        usage.sharps[1].num_dwords = 4;
        usage.sharps[1].dwords = {ir.Imm32(1u), ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(0u)};
        Optimization::ResourcePatchingPass(info, resources, profile);
        assert(info.images.size() == 1);
        assert(info.samplers.size() == 1);
        assert(info.samplers[0].associated_images == 1ULL);
    }

    // 4. Empty instruction stream guard in TranslateProgram substitutes s_endpgm without crashing.
    {
        Info info{};
        RuntimeInfo runtime{};
        runtime.Initialize(HwStage::Compute, SwStage::Compute);
        Profile profile{};
        Pools pools{};
        std::span<const u32> empty_code{};
        auto program = TranslateProgram(empty_code, pools, info, runtime, profile);
        assert(!program.blocks.empty());
        assert(program.ins_list.size() == 1);
    }

    // 5. PartialHeightViews toggle default.
    assert(!PortToggle::Disabled(PortToggle::PartialHeightViews));

    // 6. TOport point-light guard: enabled by default, only descriptor slots proven unread are nulled.
    assert(g_point_light_guard);
    {
        const bool previous = g_point_light_guard;
        g_point_light_guard = true;
        Info info{};
        info.pgm_hash = 0x804aeead;
        info.flattened_ud_buf.resize(708);
        auto& flat = info.flattened_ud_buf;
        flat[657] = 2; // two live lights
        flat[655] = 1; // only light 0 has a live secondary image

        AmdGpu::Image valid{};
        valid.base_address = 0x1000u;
        valid.width = 63;
        valid.height = 63;
        valid.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
        valid.data_format = static_cast<u64>(AmdGpu::DataFormat::Format8_8_8_8);
        valid.num_format = static_cast<u64>(AmdGpu::NumberFormat::Unorm);
        valid.base_level = 0;
        valid.last_level = 0;

        auto at = [&](u32 offset) {
            ImageResource resource{};
            resource.sharp_fetch.summary = SharpFetch<AmdGpu::Image>::Summary::SingleLoad;
            resource.sharp_fetch.offsets[0] = offset;
            std::memcpy(flat.data() + offset, &valid, sizeof(valid));
            return resource;
        };
        const auto first0 = at(68);
        const auto first1 = at(76);
        const auto first2 = at(84);
        const auto second0 = at(196);
        const auto second1 = at(204);
        const auto second2 = at(212);

        assert(first0.GetSharp(info).Address() == valid.Address());
        assert(first1.GetSharp(info).Address() == valid.Address());
        assert(first2.GetSharp(info).Address() == 0);
        assert(second0.GetSharp(info).Address() == valid.Address());
        assert(second1.GetSharp(info).Address() == 0);
        assert(second2.GetSharp(info).Address() == 0);

        flat[655] = 3; // light 1 secondary image becomes live
        assert(second1.GetSharp(info).Address() == valid.Address());
        flat[657] = 3; // light 2 first image becomes live
        assert(first2.GetSharp(info).Address() == valid.Address());

        info.pgm_hash = 0x1234; // all other shaders remain unchanged
        assert(second2.GetSharp(info).Address() == valid.Address());
        info.pgm_hash = 0x804aeead;
        flat.resize(707); // unfamiliar SRT layout stays unchanged
        assert(second2.GetSharp(info).Address() == valid.Address());
        flat.resize(708);
        g_point_light_guard = false; // rollback restores original descriptor
        assert(second2.GetSharp(info).Address() == valid.Address());
        g_point_light_guard = previous;
    }

    std::puts("shader resource / sampler clamping / mip binding / empty program guard / point-light guard OK");
    return EXIT_SUCCESS;
}
