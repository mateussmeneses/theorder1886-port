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
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    using namespace Shader;

    // Indirect draws preserve the PM4 SGPR offsets and select indirect vertex-ID semantics.
    {
        const auto params = Vulkan::DrawIndirectParams::Indirect(7, 11);
        assert(params.vertex_sgpr_offset == 7);
        assert(params.instance_sgpr_offset == 11);
        assert(params.is_indirect);

        Info info{};
        info.hw_stage = HwStage::Vertex;
        info.sw_stage = SwStage::Vertex;
        RuntimeInfo direct{};
        direct.Initialize(HwStage::Vertex, SwStage::Vertex);
        RuntimeInfo indirect = direct;
        indirect.sw.vs.is_indirect = params.is_indirect;
        Profile profile{};
        const Backend::Bindings bindings{};
        const StageSpecialization direct_spec{info, direct, profile, bindings};
        const StageSpecialization indirect_spec{info, indirect, profile, bindings};
        assert(!(direct_spec == indirect_spec));
    }

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

    // 5. Indirect vertex shaders undo Vulkan's base offsets when fetch shaders don't supply them.
    {
        Info info{};
        info.hw_stage = HwStage::Vertex;
        info.sw_stage = SwStage::Vertex;
        Profile profile{};
        const auto count_subtractions = [&](RuntimeInfo runtime) {
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            Gcn::Translator translator(info, runtime, profile);
            translator.EmitPrologue(&block);
            u32 count = 0;
            for (const auto& instruction : block.Instructions()) {
                count += instruction.GetOpcode() == IR::Opcode::ISub32;
            }
            return count;
        };
        RuntimeInfo direct{};
        direct.Initialize(HwStage::Vertex, SwStage::Vertex);
        RuntimeInfo indirect = direct;
        indirect.sw.vs.is_indirect = true;
        assert(count_subtractions(direct) == 0);
        assert(count_subtractions(indirect) == 2);
    }

    // 6. PartialHeightViews toggle default.
    assert(!PortToggle::Disabled(PortToggle::PartialHeightViews));

    std::puts("shader resource / indirect IDs / sampler clamping / mip binding / empty program guard OK");
    return EXIT_SUCCESS;
}
