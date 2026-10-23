// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>
#include "port_threads.h"

#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/elf_info.h"
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "port_settings.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_lighting_test.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bb_face_depth_fix.h"
#include "bb_particle_srt.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

struct AsyncShaderCompilation {
    AsyncJob<std::shared_ptr<AsyncShaderCompilation>> job;
    Shader::Info info;
    Shader::RuntimeInfo runtime{};
    std::vector<u32> code, user_data, geometry_code, binary;
    std::unique_ptr<Shader::Pools> pools;
    CompilerPool<Shader::Pools>* pool_cache{};
    std::optional<Shader::IR::Program> ir;
    Shader::StageSpecialization spec;
    Shader::Gcn::FetchShaderSnapshot fetch;
    Shader::Backend::Bindings binding;
    vk::Device device;
    vk::ShaderModule module{};
    size_t permutation{};
    bool initial{};
    ~AsyncShaderCompilation() {
        ir.reset();
        if (pools) {
            pool_cache->Recycle(std::move(pools));
        }
        if (module) {
            device.destroyShaderModule(module);
        }
    }
};

struct AsyncGraphicsCompilation {
    AsyncJob<std::shared_ptr<AsyncGraphicsCompilation>> job;
    std::chrono::steady_clock::time_point queued_at{};
    u64 queue_wait_ns{}, pipeline_create_ns{};
    PipelineSelection selection;
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<Shader::Gcn::FetchShaderSnapshot, MaxShaderStages> fetch;
    std::array<const Shader::Info*, MaxShaderStages> live_infos;
    GraphicsPipeline::SerializationSupport data;
    std::unique_ptr<GraphicsPipeline> pipeline;
};

// port: a compute pipeline built on a compiler thread. Its driver compile took ~180 ms on the GPU
// thread per new permutation of The Order's material shader (0x804aeead); the dispatches that
// need it are skipped until it is ready, as draws are while their graphics pipeline compiles.
struct AsyncComputeCompilation {
    AsyncJob<std::shared_ptr<AsyncComputeCompilation>> job;
    ComputePipelineKey key{};
    std::unique_ptr<Shader::Info> info;
    std::vector<u32> user_data;
    const Shader::Info* live_info{};
    vk::ShaderModule module{};
    ComputePipeline::SerializationSupport data{};
    std::unique_ptr<ComputePipeline> pipeline;
};

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

/// port: selections keep their own copy (workers fill them while other threads add
/// permutations, which moves the specializations the upstream pointer referred to).
static std::optional<Shader::Gcn::FetchShaderData> ToFetchShader(
    const Shader::Gcn::FetchShaderData& data) {
    if (data.Empty()) {
        return std::nullopt;
    }
    return data;
}

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.dx10_clamp = program.settings.dx10_clamp;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.depth_before_shader = regs.depth_shader_control.depth_before_shader;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u16(ps_inputs[i].input_offset),
                .is_default = u16(ps_inputs[i].use_default),
                .is_flat = u16(ps_inputs[i].flat_shade),
                .default_value = u16(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        // port: the per-vertex geometry shader of GPUs without barycentrics needs a vertex shader
        // straight before and triangle lists; left 0 elsewhere so permutations stay as they were.
        info.hw.fs.vs_direct_triangles =
            !profile.supports_amd_shader_explicit_vertex_parameter &&
            !profile.supports_fragment_shader_barycentric &&
            regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs &&
            sel.graphics_key.prim_type == AmdGpu::PrimitiveType::TriangleList;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.props.dx10_clamp = cs_pgm.settings.dx10_clamp;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.is_indirect = sel.draw_indirect_params.is_indirect;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

namespace {
constexpr u32 DriverCacheMagic = 0x4B435649; // "IVCK"
constexpr u32 DriverCacheVersion = 1;
struct DriverCacheHeader {
    u32 magic;
    u32 version;
    u64 payload_size;
    u64 payload_hash;
};
static_assert(sizeof(DriverCacheHeader) == 24);

std::filesystem::path DriverCacheFile(const Instance& instance) {
    const std::string serial{Common::ElfInfo::Instance().GameSerial()};
    std::string uuid;
    for (const u8 byte : instance.GetPipelineCacheUUID()) {
        uuid += fmt::format("{:02x}", byte);
    }
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           ("driver-" + (serial.empty() ? std::string{"unknown"} : serial)) /
           fmt::format("vk_pipeline_cache_{:04x}_{:04x}_{}.vkc", instance.GetVendorID(),
                       instance.GetDeviceID(), uuid);
}

std::vector<u8> LoadDriverCacheBlob(const Instance& instance) {
    const auto path = DriverCacheFile(instance);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        LOG_INFO(Render_Vulkan, "Driver pipeline cache: none saved for this GPU/driver");
        return {};
    }
    std::vector<u8> file(std::filesystem::file_size(path, ec));
    if (ec || file.size() < sizeof(DriverCacheHeader) + sizeof(VkPipelineCacheHeaderVersionOne)) {
        return {};
    }
    {
        const Common::FS::IOFile io{path, Common::FS::FileAccessMode::Read};
        if (!io.IsOpen() || io.Read(file) != file.size()) {
            return {};
        }
    }
    DriverCacheHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    const u8* payload = file.data() + sizeof(header);
    if (header.magic != DriverCacheMagic || header.version != DriverCacheVersion ||
        header.payload_size != file.size() - sizeof(header) ||
        XXH3_64bits(payload, header.payload_size) != header.payload_hash) {
        LOG_WARNING(Render_Vulkan, "Driver pipeline cache ignored (damaged file)");
        return {};
    }
    VkPipelineCacheHeaderVersionOne driver;
    std::memcpy(&driver, payload, sizeof(driver));
    const auto uuid = instance.GetPipelineCacheUUID();
    if (driver.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        driver.vendorID != instance.GetVendorID() || driver.deviceID != instance.GetDeviceID() ||
        std::memcmp(driver.pipelineCacheUUID, uuid.data(), VK_UUID_SIZE) != 0) {
        LOG_WARNING(Render_Vulkan, "Driver pipeline cache ignored (different GPU/driver)");
        return {};
    }
    LOG_INFO(Render_Vulkan, "Driver pipeline cache loaded ({} bytes)", header.payload_size);
    return {payload, payload + header.payload_size};
}
} // namespace

std::atomic<u64> g_port_driver_pipelines_created{0};

struct PipelineCache::DriverCacheSaver {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    u64 saved_hash = 0;
    u64 saved_created = ~0ull;
    bool over_limit_logged = false;
};

bool PipelineCache::SaveDriverCache() {
    try {
        return SaveDriverCacheUnchecked();
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "Driver pipeline cache save skipped: {}", e.what());
        return false;
    }
}

bool PipelineCache::SaveDriverCacheUnchecked() {
    const u64 created = g_port_driver_pipelines_created.load(std::memory_order_relaxed);
    if (driver_saver && created == driver_saver->saved_created) {
        return true;
    }
    const auto device = instance.GetDevice();
    auto [scratch_result, scratch] = device.createPipelineCacheUnique({});
    if (scratch_result != vk::Result::eSuccess) {
        return false;
    }
    std::vector<vk::PipelineCache> sources{*pipeline_cache};
    for (const auto& cache : worker_pipeline_caches) {
        sources.push_back(*cache);
    }
    if (device.mergePipelineCaches(*scratch, sources) != vk::Result::eSuccess) {
        return false;
    }
    auto [result, blob] = device.getPipelineCacheData(*scratch);
    if (result != vk::Result::eSuccess || blob.size() < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return false;
    }
    static const u64 max_bytes = [] {
        const char* v = std::getenv("THEORDER_DRIVER_CACHE_MAX_MB");
        return u64(v && *v ? std::max(0, std::atoi(v)) : 1024) << 20;
    }();
    if (max_bytes && blob.size() > max_bytes) {
        if (driver_saver && !driver_saver->over_limit_logged) {
            driver_saver->over_limit_logged = true;
            LOG_WARNING(Render_Vulkan, "Driver pipeline cache over limit ({} MB)", blob.size() >> 20);
        }
        if (driver_saver) {
            driver_saver->saved_created = created;
        }
        return false;
    }
    const u64 hash = XXH3_64bits(blob.data(), blob.size());
    if (driver_saver && hash == driver_saver->saved_hash) {
        driver_saver->saved_created = created;
        return true;
    }
    const auto path = DriverCacheFile(instance);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += ".tmp";
    const DriverCacheHeader header{DriverCacheMagic, DriverCacheVersion, blob.size(), hash};
    bool written = false;
    {
        const Common::FS::IOFile io{temp, Common::FS::FileAccessMode::Create};
        written = io.IsOpen() && io.WriteRaw<u8>(&header, sizeof(header)) == sizeof(header) &&
                  io.Write(blob) == blob.size();
    }
    if (written) {
        std::filesystem::rename(temp, path, ec);
    }
    if (!written || ec) {
        std::filesystem::remove(temp, ec);
        LOG_ERROR(Render_Vulkan, "Driver pipeline cache not written to {}", path.string());
        return false;
    }
    if (driver_saver) {
        driver_saver->saved_hash = hash;
        driver_saver->saved_created = created;
    }
    LOG_INFO(Render_Vulkan, "Driver pipeline cache saved ({} bytes)", blob.size());
    return true;
}

void PipelineCache::StartDriverCacheSaver() {
    driver_saver = new DriverCacheSaver;
    driver_saver->thread = std::thread([this] {
        Common::SetCurrentThreadName("port:DriverCacheSaver");
        auto* saver = driver_saver;
        std::unique_lock lock{saver->mutex};
        while (!saver->stop) {
            static const int save_seconds = [] {
                const char* v = std::getenv("THEORDER_DRIVER_CACHE_SAVE_S");
                return v && *v ? std::clamp(std::atoi(v), 10, 3600) : 60;
            }();
            if (saver->cv.wait_for(lock, std::chrono::seconds(save_seconds), [&] { return saver->stop; })) {
                break;
            }
            lock.unlock();
            SaveDriverCache();
            lock.lock();
        }
    });
}

void PipelineCache::StopDriverCacheSaver() {
    if (!driver_saver) {
        return;
    }
    {
        std::scoped_lock lock{driver_saver->mutex};
        driver_saver->stop = true;
    }
    driver_saver->cv.notify_all();
    if (driver_saver->thread.joinable()) {
        driver_saver->thread.join();
    }
    delete driver_saver;
    driver_saver = nullptr;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // port: older NVIDIA (Pascal) has no barycentrics; THEORDER_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("THEORDER_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        // FEAT-005 Class D (HYPOTHESIS): reconstruct the signed 2_10_10_10 decode in-shader when
        // the host forced the Snorm surface onto the unsigned Uint view (see
        // Instance::IsSnorm2101010Supported / GetSupportedFormat). This is active on ANY host that
        // lacks the Snorm pack format, including the NVIDIA RTX 5060 repro machine - so "no NVIDIA
        // regression" must be verified at runtime (capture), not assumed from the gate.
        .needs_snorm_2101010_expand = !instance.IsSnorm2101010Supported(),
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_ssbo_store_load_barrier = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .dref_broadcast = LightingTest::Current().dref,
    };

    std::vector<u8> driver_blob;
    static const bool driver_cache_enabled = [] {
        const char* v = std::getenv("THEORDER_DRIVER_CACHE");
        return !v || (std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0);
    }();
    if (driver_cache_enabled) {
        driver_blob = LoadDriverCacheBlob(instance);
    }
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(
        vk::PipelineCacheCreateInfo{
            .initialDataSize = driver_blob.size(),
            .pInitialData = driver_blob.data(),
        });
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    // port: the compiler threads always exist; the in-game menu switches async compilation live
    // (PortSettings::async_shaders, THEORDER_ASYNC_SHADERS at start).
    {
        // Low priority threads: on a 16+ thread CPU 8 drivers compile while the game runs, so a
        // scene's new pipelines (and the whole cache after a translation change) arrive sooner.
        const auto count = std::clamp(PortThreads::Available() / 2, 1u, 8u);
        const auto capture_count = std::min(4u, std::max(1u, PortThreads::Available() / 4));
        for (unsigned i = 0; i < count; ++i) {
            auto [result, worker_cache] = instance.GetDevice().createPipelineCacheUnique({});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compiler pipeline cache");
            worker_pipeline_caches.push_back(std::move(worker_cache));
        }
        capture_compiler = std::make_unique<AsyncCompiler>(capture_count, 32, [](unsigned index) {
            Common::SetCurrentThreadName(("port:ShaderCapture" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        });
        compiler = std::make_unique<AsyncCompiler>(count, 256, [](unsigned index) {
            Common::SetCurrentThreadName(("port:ShaderCompile" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
        LOG_INFO(Render_Vulkan, "port: async graphics compilation ready ({} capture, {} compile workers)",
                 capture_count, count);
        if (driver_cache_enabled) {
            StartDriverCacheSaver();
        }
    }
}

PipelineCache::~PipelineCache() {
    StopWarmUp();
    FinishCompilations();
    StopDriverCacheSaver();
    SaveDriverCache();
}

// port: shader/pipeline compile time on the GPU thread, reported by THEORDER_FRAME_STATS.
std::atomic<u64> g_port_compile_ns;
std::atomic<u32> g_port_compiles;
// port: the synchronous compile split: GCN translation and SPIR-V emission (CompileModule).
std::atomic<u64> g_port_translate_ns, g_port_emit_ns;
std::atomic<u64> g_port_capture_ns, g_port_async_ns, g_port_compile_wait_ns;
std::atomic<u64> g_port_snapshot_ns, g_port_guest_wait_ns;
std::atomic<u32> g_port_pool_reuses;
std::atomic<u32> g_port_pending_draws;
std::atomic<u32> g_port_fallback_dispatches;
namespace {
struct PhaseTimer {
    std::atomic<u64>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool active = true;
    void Stop() {
        if (!active) {
            return;
        }
        counter += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count());
        active = false;
    }
    ~PhaseTimer() { Stop(); }
};
// port: known pipelines (binaries an earlier session stored) are built on the GPU thread so
// their draws are not skipped; a scene start restored ~100 one after another, a 0.8-1.4 s
// freeze. Past 8 ms of such builds in a 33 ms window the rest go to the compiler workers, in
// parallel, and their draws wait a frame or two (PortToggle::SyncBuildBudget turns it off).
// Builds on the compiler workers do not count.
std::atomic<u64> g_sync_build_ns{0};

bool SyncBuildBudgetLeft() {
    static auto window = std::chrono::steady_clock::now();
    static u64 window_start_ns = 0;
    const auto now = std::chrono::steady_clock::now();
    const u64 spent = g_sync_build_ns.load(std::memory_order_relaxed);
    if (now - window > std::chrono::milliseconds(33)) {
        window = now;
        window_start_ns = spent;
    }
    return PortToggle::Disabled(PortToggle::SyncBuildBudget) || spent - window_start_ns < 8'000'000;
}

struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_port_compile_ns += ns;
        if (!AsyncCompiler::OnWorker()) {
            g_sync_build_ns.fetch_add(ns, std::memory_order_relaxed);
        }
        ++g_port_compiles;
    }
};

// port: THEORDER_PERMUTATION_LOG names the specialization field that forced a new permutation.
std::string DescribeSpecDifference(const Shader::StageSpecialization& a,
                                   const Shader::StageSpecialization& b) {
    std::string out;
    const auto add = [&out](std::string what) {
        out += out.empty() ? "" : ", ";
        out += std::move(what);
    };
    if (a.vs_attribs != b.vs_attribs) add("vs_attribs");
    if (a.runtime_info.props != b.runtime_info.props) add("props");
    if (!a.runtime_info.HasSameHwInfo(b.runtime_info.hw)) {
        if (a.runtime_info.hw_stage == Shader::HwStage::Compute) {
            const auto& x = a.runtime_info.hw.cs;
            const auto& y = b.runtime_info.hw.cs;
            add(fmt::format("cs workgroup {}x{}x{} vs {}x{}x{}", x.workgroup_size[0],
                            x.workgroup_size[1], x.workgroup_size[2], y.workgroup_size[0],
                            y.workgroup_size[1], y.workgroup_size[2]));
        } else {
            add("hw runtime info");
        }
    }
    if (!a.runtime_info.HasSameSwInfo(b.runtime_info.sw)) add("sw runtime info");
    if (a.fetch_shader_data != b.fetch_shader_data) add("fetch shader");
    for (u32 i = 0; i < std::min(a.fmasks.size(), b.fmasks.size()); ++i) {
        if (a.fmasks[i] != b.fmasks[i]) {
            add(fmt::format("fmask {} {}x{} vs {}x{}", i, a.fmasks[i].width, a.fmasks[i].height,
                            b.fmasks[i].width, b.fmasks[i].height));
        }
    }
    if (a.start != b.start) add("start bindings");
    // Only resources bound in `a` (the new draw) break the match (StageSpecialization ==).
    u32 binding = 0;
    for (u32 i = 0; i < std::min(a.buffers.size(), b.buffers.size()); ++i) {
        const auto& x = a.buffers[i];
        const auto& y = b.buffers[i];
        if (a.bitset[binding++] && x != y) {
            add(fmt::format("buffer {} stride {} vs {}, formatted {}/{}, fmt {}/{} vs {}/{}", i,
                            u32(x.stride), u32(y.stride), u32(x.is_formatted),
                            u32(y.is_formatted), u32(x.data_format), u32(x.num_format),
                            u32(y.data_format), u32(y.num_format)));
        }
    }
    for (u32 i = 0; i < std::min(a.images.size(), b.images.size()); ++i) {
        const auto& x = a.images[i];
        const auto& y = b.images[i];
        if (a.bitset[binding++] && x != y) {
            const auto sharp = a.info->images[i].GetSharp(*a.info);
            add(fmt::format("image {} type {}/{} int {}/{} storage {}/{} fmask {}/{} srgb {}/{} "
                            "bindings {}/{} (T# {:#x} {}x{}x{} fmt {}/{} mips {}-{})",
                            i, u32(x.type), u32(y.type), x.is_integer, y.is_integer, x.is_storage,
                            y.is_storage, x.is_fmask, y.is_fmask, x.is_srgb, y.is_srgb,
                            x.num_bindings, y.num_bindings, sharp.Address(), u32(sharp.width + 1),
                            u32(sharp.height + 1), u32(sharp.depth + 1), u32(sharp.GetDataFmt()),
                            u32(sharp.GetNumberFmt()), u32(sharp.base_level),
                            u32(sharp.last_level)));
        }
    }
    for (u32 i = 0; i < std::min(a.samplers.size(), b.samplers.size()); ++i) {
        if (a.samplers[i] != b.samplers[i]) add(fmt::format("sampler {}", i));
    }
    return out.empty() ? "no field difference found" : out;
}

void LogNewPermutation(u64 hash, size_t perm_idx, const Shader::StageSpecialization& spec,
                       const Shader::StageSpecialization& closest) {
    static const bool enabled = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
    if (enabled) {
        LOG_WARNING(Render_Vulkan, "port: shader {:#x} permutation {}: {}", hash, perm_idx,
                    DescribeSpecDifference(spec, closest));
    }
}
} // namespace

bool PipelineCache::QueueShader(Program& program, HwStage stage, SwStage l_stage,
                                 const Shader::ShaderParams& params,
                                 Shader::RuntimeInfo runtime_info,
                                 Shader::Backend::Bindings binding, size_t permutation,
                                 bool initial,
                                 std::optional<Shader::StageSpecialization> specialization) {
    if (!compiler->HasCapacity()) {
        return false;
    }
    PhaseTimer snapshot{g_port_snapshot_ns};
    auto build = std::make_shared<AsyncShaderCompilation>();
    build->device = instance.GetDevice();
    build->code.assign(params.code.begin(), params.code.end());
    build->user_data.assign(params.user_data.begin(), params.user_data.end());
    build->info = Shader::Info(stage, l_stage, params);
    build->info.user_data = build->user_data;
    build->runtime = runtime_info;
    if (stage == HwStage::Geometry && !runtime_info.hw.gs.vs_copy.empty()) {
        build->geometry_code.assign(runtime_info.hw.gs.vs_copy.begin(), runtime_info.hw.gs.vs_copy.end());
        build->runtime.hw.gs.vs_copy = build->geometry_code;
    }
    build->binding = binding;
    build->permutation = permutation;
    build->initial = initial;
    if (specialization) {
        build->spec = *specialization;
    }
    build->pool_cache = &compiler_pools;
    snapshot.Stop();
    AsyncJob<bool> capture_job;
    if (!capture_job.Start(*capture_compiler, [this, build, binding] {
        PhaseTimer capture{g_port_capture_ns};
        auto [pools, reused] = compiler_pools.Acquire();
        build->pools = std::move(pools);
        if (reused) {
            ++g_port_pool_reuses;
        }
        // port: the GPU thread waits until all guest reads and specialization are captured.
        build->ir.emplace(Shader::TranslateProgram(build->code, *build->pools, build->info,
                                                   build->runtime, profile));
        build->spec = Shader::StageSpecialization(build->info, build->runtime, profile, binding);
        build->fetch.Capture(build->info);
        return true;
    })) {
        return false;
    }
    {
        PhaseTimer waiting{g_port_guest_wait_ns};
        capture_job.Wait();
    }
    if (!capture_job.Poll()) {
        if (initial) {
            program.compilation_failed = true;
        } else {
            specialization->info = &program.info;
            program.failed_permutations.push_back(std::move(*specialization));
        }
        LOG_ERROR(Render_Vulkan, "port: shader {:#x}/{} capture failed: {}", params.hash,
                  permutation, capture_job.Error());
        return false;
    }
    const bool queued = build->job.Start(*compiler, [this, build] {
        PhaseTimer compile{g_port_async_ns};
        Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir,
                                                    build->binding);
        DumpShader(build->code, build->info.pgm_hash, build->info.hw_stage, build->permutation, "bin");
        DumpShader(spv, build->info.pgm_hash, build->info.hw_stage, build->permutation, "spv");
        auto patch = GetShaderPatch(build->info.pgm_hash, build->info.hw_stage,
                                    build->permutation, "spv");
        build->module = CompileSPV(patch && EmulatorSettings.IsPatchShaders() ? *patch : spv,
                                    instance.GetDevice());
        build->binary = std::move(spv);
        build->ir.reset();
        compiler_pools.Recycle(std::move(build->pools));
        return build;
    }, true);
    if (queued) {
        program.pending = std::move(build);
        pending_programs.push_back(&program);
    }
    return queued;
}

void PipelineCache::PublishShaders(bool wait) {
    for (size_t i = 0; i < pending_programs.size();) {
        auto* program = pending_programs[i];
        const auto hash = program->info.pgm_hash;
        const auto retire = [&] {
            pending_programs[i] = pending_programs.back();
            pending_programs.pop_back();
        };
        auto build = program->pending;
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed()) {
                if (build->initial) {
                    program->compilation_failed = true;
                } else {
                    build->spec.info = &program->info;
                    program->failed_permutations.push_back(std::move(build->spec));
                }
                program->pending.reset();
                LOG_ERROR(Render_Vulkan, "port: shader {:#x}/{} compilation failed: {}", hash,
                          build->permutation, build->job.Error());
                retire();
            } else {
                ++i;
            }
            continue;
        }
        const auto perm_hash = HashCombine(hash, build->permutation);
        Vulkan::SetObjectName(instance.GetDevice(), build->module,
                             GetShaderName(build->info.hw_stage, hash, build->permutation));
        const u64 spv_hash =
            RegisterShaderBinary(std::move(build->binary), hash, build->permutation);
        {
            std::unique_lock lock{programs_mutex};
            if (build->initial) {
                program->info = std::move(build->info);
                // user_data is refreshed on the next lookup; no pointer into a completed job survives.
                program->info.user_data = {};
                program->info_template = std::make_unique<Shader::Info>(program->info);
            }
            build->spec.info = &program->info;
            RegisterShaderMeta(program->info, build->spec.fetch_shader_data, build->spec,
                               perm_hash, build->permutation, spv_hash);
            program->AddPermut(build->module, std::move(build->spec), spv_hash);
            build->module = nullptr;
            program->pending.reset();
        }
        retire();
    }
}

void PipelineCache::PublishGraphics(bool wait) {
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto build = it.value();
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed()) {
                // A warm-up result may have won this key while the job was in flight.
                const auto ready = graphics_pipelines.find(it.key());
                if (ready == graphics_pipelines.end() || !ready->second || ready->second->Failed()) {
                    failed_graphics.insert(it.key());
                }
                LOG_ERROR(Render_Vulkan, "port: pipeline {:#x} compilation failed: {}",
                          std::hash<GraphicsPipelineKey>{}(it.key()), build->job.Error());
                if (last_lookup_pending && last_pending_key == it.key()) {
                    last_lookup_pending = false;
                }
                it = pending_graphics.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        const auto key = it.key();
        build->pipeline->PublishStages(build->live_infos);
        RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
        graphics_pipelines.emplace(key, std::move(build->pipeline));
        ++num_new_pipelines;
        it = pending_graphics.erase(it);
    }
}

const GraphicsPipeline* PipelineCache::WaitGraphics(const GraphicsPipelineKey& key,
                                                     const PreparedDraw* prepared,
                                                     const char* source) {
    // port (FEAT-004 Class C): a visible draw whose pipeline is still compiling would otherwise be
    // dropped (object pops in/out). Stall this one draw on exactly that one build instead of
    // skipping it. We wait on a single key's in-flight job and retire it with the same steps the
    // PublishGraphics loop body uses for a completed build; never the whole pending_graphics set.
    const auto it = pending_graphics.find(key);
    if (it == pending_graphics.end()) {
        return nullptr;
    }
    auto build = it.value();
    const auto wait_start = std::chrono::steady_clock::now();
    {
        PhaseTimer waiting{g_port_compile_wait_ns};
        build->job.Wait();
    }
    const double wait_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - wait_start)
                               .count();
    static const bool log_waits = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
    const auto state = prepared ? prepared->state.load(std::memory_order_acquire)
                                : PreparedDraw::Unavailable;
    const auto ready_at = prepared && state == PreparedDraw::Ready && prepared->key == key
                              ? prepared->ready_at
                              : std::chrono::steady_clock::time_point{};
    const char* prep_status = !prepared ? "no_draw"
                              : state == PreparedDraw::Pending ? "not_ready"
                              : state != PreparedDraw::Ready  ? "unavailable"
                              : prepared->key != key           ? "key_mismatch"
                              : ready_at == std::chrono::steady_clock::time_point{}
                                  ? "no_timestamp"
                                  : "matched";
    const double prep_ms = ready_at == std::chrono::steady_clock::time_point{}
                               ? -1.0
                               : std::chrono::duration<double, std::milli>(
                                     wait_start - ready_at)
                                     .count();
    const auto log_wait = [&](const char* outcome) {
        if (log_waits && wait_ms >= 0.1) {
            LOG_WARNING(Render_Vulkan,
                     "Graphics pipeline wait: flip {} key 0x{:016x} stage0 0x{:016x} "
                     "stage4 0x{:016x} spv0 0x{:016x} spv4 0x{:016x} "
                     "wait {:.3f} ms prep {:.3f} ms prep_status {} source {} queue {:.3f} ms create {:.3f} ms all_stages [{}] "
                     "all_spvs [{}] outcome {}",
                     PortStats::flips.load(std::memory_order_relaxed),
                     std::hash<GraphicsPipelineKey>{}(key), key.stage_hashes[0], key.stage_hashes[4],
                     sel.spv_hashes[0], sel.spv_hashes[4], wait_ms, prep_ms, prep_status, source,
                     build->queue_wait_ns / 1e6,
                     build->pipeline_create_ns / 1e6, fmt::join(key.stage_hashes, ","),
                     fmt::join(sel.spv_hashes, ","), outcome);
        }
    };
    if (last_lookup_pending && last_pending_key == key) {
        last_lookup_pending = false;
    }
    const auto result = build->job.Poll();
    if (!result) {
        const auto ready = graphics_pipelines.find(key);
        const auto* pipeline = ready != graphics_pipelines.end() && ready->second &&
                                       !ready->second->Failed()
                                   ? ready->second.get() : nullptr;
        if (build->job.Failed()) {
            if (!pipeline) {
                failed_graphics.insert(key);
            }
            LOG_ERROR(Render_Vulkan, "port: pipeline {:#x} compilation failed: {}",
                      std::hash<GraphicsPipelineKey>{}(key), build->job.Error());
        }
        log_wait(build->job.Failed() ? "failed" : "published");
        pending_graphics.erase(it);
        return pipeline;
    }
    build->pipeline->PublishStages(build->live_infos);
    RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
    // Warm-up may already have published this key. Return the cache's winner, since
    // emplace can discard the pipeline owned by this completed on-demand build.
    const auto published = graphics_pipelines.emplace(key, std::move(build->pipeline)).first;
    ++num_new_pipelines;
    log_wait("ready");
    pending_graphics.erase(it);
    return published->second && !published->second->Failed() ? published->second.get() : nullptr;
}

void PipelineCache::PublishCompute(bool wait) {
    for (auto it = pending_compute.begin(); it != pending_compute.end();) {
        auto build = it.value();
        if (wait) {
            build->job.Wait();
        }
        if (build->job.Failed()) {
            LOG_ERROR(Render_Vulkan, "port: compute pipeline {:#x} compilation failed: {}",
                      build->key.value, build->job.Error());
            sync_compute_keys.insert(build->key.value);
            it = pending_compute.erase(it);
            continue;
        }
        if (!build->job.Poll()) {
            ++it;
            continue;
        }
        build->pipeline->PublishStage(build->live_info);
        RegisterPipelineData(build->key, build->data);
        compute_pipelines.emplace(build->key, std::move(build->pipeline));
        ++num_new_pipelines;
        it = pending_compute.erase(it);
    }
}

void PipelineCache::FinishCompilations() {
    if (compiler) {
        capture_compiler->Stop();
        compiler->Stop();
        PublishShaders(true);
        PublishGraphics(true);
        PublishCompute(true);
        std::vector<vk::PipelineCache> sources;
        for (const auto& cache : worker_pipeline_caches) {
            sources.push_back(*cache);
        }
        const auto result = instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "port: compiler cache merge failed: {}", vk::to_string(result));
        }
    }
}

namespace {
// port: a draw whose color targets are all small (lookup tables, 1x1 luminance, the end of
// downsample chains, light ramps). Such draws are often made once at scene initialization;
// compiling these at once costs a short stall the first time only, preventing unlit or broken materials.
bool SmallTargetDraw(const AmdGpu::Regs& regs) {
    bool any = false;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }
        if (u64(col_buf.Pitch()) * col_buf.Height() > 256 * 256) {
            return false;
        }
        any = true;
    }
    return any;
}
} // namespace

bool PipelineCache::AsyncCompilation() const {
    return compiler && !sync_compile && PortSettings::Get().async_shaders.load(std::memory_order_relaxed);
}

void PipelineCache::SeedWorkerCaches() {
    if (!compiler || worker_caches_seeded) {
        return;
    }
    for (const auto& cache : worker_pipeline_caches) {
        const auto result = instance.GetDevice().mergePipelineCaches(*cache, *pipeline_cache);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to seed compiler pipeline cache");
    }
    worker_caches_seeded = true;
}

void PipelineCache::ReportCompilerStats() {
    ReportPipelineBinaryCache();
    if (compiler) {
        std::printf("Compiler stats: capture queue %zu, compile queue %zu; queue waits: capture "
                    "%.1f ms, compile %.1f ms; pools retained %zu, reused %u\n",
                    capture_compiler->Depth(), compiler->Depth(),
                    capture_compiler->QueueWaitNs() / 1e6, compiler->QueueWaitNs() / 1e6,
                    compiler_pools.Retained(), g_port_pool_reuses.exchange(0));
    }
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum ||
        (!failed_graphics.empty() && failed_graphics.contains(prepared.key))) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() && it->second && !it->second->Failed()
               ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    SeedWorkerCaches();
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    used_prepared = nullptr;
    last_lookup_pending = false; // port (FEAT-004): reset the pending side-channel each lookup.
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            if (!pipeline->hot_noted) {
                NoteHot(*pipeline, 'g', std::hash<GraphicsPipelineKey>{}(prepared->key));
            }
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    sync_compile = SmallTargetDraw(liverpool->regs);
    struct SyncReset {
        bool& flag;
        ~SyncReset() {
            flag = false;
        }
    } sync_reset{sync_compile};
    if (!RefreshGraphicsKey(sel)) {
        if (sel.compilation_pending) {
            ++g_port_pending_draws;
        }
        return nullptr;
    }
    // Keep only the key after an exceptional build: its snapshots can be freed, but neither
    // an async retry nor a synchronous/known-binary restore may recreate it this session.
    if (!failed_graphics.empty() && failed_graphics.contains(sel.graphics_key)) {
        return nullptr;
    }
    if (sync_compile && pending_graphics.contains(sel.graphics_key)) {
        // Refresh first: the previous selection may belong to a different draw.
        return WaitGraphics(sel.graphics_key, prepared, "small_target_sync");
    }
    // port: a pipeline an earlier session built (its key is stored) comes back from the
    // pipeline binary cache in 1-2 ms: built here, its draws are not skipped meanwhile. Skipped
    // draws were the flicker of objects and effects every time a session met them again.
    const auto stored = [this] {
        return Storage::DataBase::Instance().Exists(
            Storage::BlobType::PipelineKey,
            fmt::format("g_{:#018x}", std::hash<GraphicsPipelineKey>{}(sel.graphics_key)));
    };
    const auto known = [&] {
        // A stage permutation made this session took the next free index: a stored key with
        // that index may be another session's permutation, whose binary does not fit (a full
        // driver compile here, 130-190 ms, every session).
        return !sel.fresh_permutation && SyncBuildBudgetLeft() && stored();
    };
    const bool can_async = AsyncCompilation() && !sel.regs->stage_enable.hs_en &&
                           !EmulatorSettings.IsShaderCollect();
    const auto build_in_background = [&](bool restore_expected) -> const GraphicsPipeline* {
        if (!pending_graphics.contains(sel.graphics_key) && compiler->HasCapacity()) {
            PhaseTimer capture{g_port_snapshot_ns};
            auto build = std::make_shared<AsyncGraphicsCompilation>();
            build->selection = sel;
            build->selection.regs = nullptr;
            build->live_infos = sel.infos;
            for (u32 i = 0; i < MaxShaderStages; ++i) {
                if (sel.infos[i]) {
                    build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i]);
                    build->user_data[i].assign(sel.infos[i]->user_data.begin(), sel.infos[i]->user_data.end());
                    build->infos[i]->user_data = build->user_data[i];
                    build->selection.infos[i] = build->infos[i].get();
                    build->fetch[i].Capture(*build->infos[i]);
                }
            }
            // A stored key past this frame's restore budget comes back from the binary cache in a
            // few ms: ahead of the driver compiles (~250 ms each), its draws are skipped for a
            // frame or two instead of seconds (objects blinking at scene starts).
            const bool urgent = restore_expected && !sel.fresh_permutation && stored();
            build->queued_at = std::chrono::steady_clock::now();
            const bool queued = build->job.Start(*compiler, [this, build] {
                const auto worker_started = std::chrono::steady_clock::now();
                build->queue_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           worker_started - build->queued_at).count();
                // Only the vertex stage uses fetch-shader guest pointers during pipeline construction.
                Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch[u32(SwStage::Vertex)]};
                PhaseTimer compile{g_port_async_ns};
                auto& s = build->selection;
                const auto create_started = std::chrono::steady_clock::now();
                const auto record_create_time = [&] {
                    build->pipeline_create_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - create_started)
                                                    .count();
                };
                try {
                    build->pipeline = std::make_unique<GraphicsPipeline>(
                        instance, scheduler, desc_heap, profile, s.graphics_key,
                        *worker_pipeline_caches[AsyncCompiler::WorkerIndex()],
                        s.infos, s.runtime_infos, s.fetch_shader ? &*s.fetch_shader : nullptr,
                        s.modules, build->data, false);
                } catch (...) {
                    record_create_time();
                    throw;
                }
                record_create_time();
                return build;
            }, urgent);
            if (queued) {
                static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
                if (log) {
                    LOG_WARNING(Render_Vulkan,
                                "port: graphics pipeline {:#x} built in the background from flip "
                                "{}: stored {}, fresh permutation {}, budget left {}, stages "
                                "{:#x} {:#x}",
                                std::hash<GraphicsPipelineKey>{}(sel.graphics_key),
                                PortStats::flips.load(std::memory_order_relaxed), stored(),
                                sel.fresh_permutation, SyncBuildBudgetLeft(),
                                sel.graphics_key.stage_hashes[0], sel.graphics_key.stage_hashes[4]);
                }
                pending_graphics.emplace(sel.graphics_key, std::move(build));
            }
        }
        ++g_port_pending_draws;
        // port (FEAT-004): record whether this null is a still-compiling key (vs a failed/absent
        // one) so the rasterizer can stall a visible draw on exactly that one build.
        if (pending_graphics.contains(sel.graphics_key)) {
            last_lookup_pending = true;
            last_pending_key = sel.graphics_key;
        }
        return nullptr;
    };
    if (can_async && (pending_graphics.contains(sel.graphics_key) ||
                      (!graphics_pipelines.contains(sel.graphics_key) && !known()))) {
        return build_in_background(true);
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        // A known key may name another session's permutation: only its stored binary is
        // restored here, and without one it is built in the background (no 150 ms compile).
        g_pipeline_restore_only = can_async;
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader ? &*sel.fetch_shader : nullptr, sel.modules,
            sdata, false);
        g_pipeline_restore_only = false;
        if (!it.value()->Handle()) {
            graphics_pipelines.erase(it);
            return build_in_background(false); // no stored binary: a full compile
        }

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    if (it->second && !it->second->hot_noted) {
        NoteHot(*it->second, 'g', std::hash<GraphicsPipelineKey>{}(sel.graphics_key));
    }
    return it->second && !it->second->Failed() ? it->second.get() : nullptr;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    PublishShaders();
    PublishGraphics();
    PublishCompute();
    PumpWarmUp();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const u64 pgm_hash = sel.infos[0]->pgm_hash;
    if (const auto found = compute_pipelines.find(compute_key); found != compute_pipelines.end()) {
        CountComputeRun(pgm_hash);
        if (found->second && !found->second->hot_noted) {
            NoteHot(*found->second, 'c', compute_key.value);
        }
        return found->second.get();
    }
    // A pipeline an earlier session built is restored in 1-2 ms: built here (GetGraphicsPipeline).
    const bool known = !sel.fresh_permutation && SyncBuildBudgetLeft() &&
                       !pending_compute.contains(compute_key) &&
                       Storage::DataBase::Instance().Exists(
                           Storage::BlobType::PipelineKey,
                           fmt::format("c_{:#018x}", compute_key.value));
    // A skippable dispatch is redone every frame: skipping one costs nothing, so even a known
    // pipeline builds in the background (a stored key may be another session's permutation at
    // that index, and the driver then compiled it here: 0x804aeead, the particle lighting, 177 ms).
    if ((!known || !PortToggle::Disabled(PortToggle::SyncBuildBudget)) &&
        SkippableDispatch(*sel.infos[0]) && AsyncCompilation() &&
        !EmulatorSettings.IsShaderCollect() && !sync_compute_keys.contains(compute_key.value)) {
        static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (log && !pending_compute.contains(compute_key)) {
            LOG_WARNING(Render_Vulkan, "port: compute pipeline of {:#x} built in the background "
                        "from flip {}: its dispatches are skipped until it is ready",
                        pgm_hash, PortStats::flips.load(std::memory_order_relaxed));
        }
        if (!pending_compute.contains(compute_key) && compiler->HasCapacity()) {
            auto build = std::make_shared<AsyncComputeCompilation>();
            build->key = compute_key;
            // The GPU thread rewrites the program's info (user data) on every dispatch.
            build->info = std::make_unique<Shader::Info>(*sel.infos[0]);
            build->user_data.assign(sel.infos[0]->user_data.begin(),
                                    sel.infos[0]->user_data.end());
            build->info->user_data = build->user_data;
            build->live_info = sel.infos[0];
            build->module = sel.modules[0];
            const bool queued = build->job.Start(*compiler, [this, build] {
                PhaseTimer compile{g_port_async_ns};
                build->pipeline = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile,
                    *worker_pipeline_caches[AsyncCompiler::WorkerIndex()], build->key,
                    *build->info, build->module, build->data, false);
                return build;
            });
            if (queued) {
                pending_compute.emplace(compute_key, std::move(build));
            }
        }
        if (const auto* fallback = FallbackComputePipeline(pgm_hash)) {
            ++g_port_fallback_dispatches;
            CountComputeRun(pgm_hash);
            return fallback;
        }
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (log) {
            const auto runs = compute_runs.find(pgm_hash);
            LOG_WARNING(Render_Vulkan,
                        "port: compute pipeline of {:#x} built on the GPU thread: {} frames, last "
                        "{} of {}, writes images {}",
                        pgm_hash, runs != compute_runs.end() ? runs->second.frames : 0,
                        runs != compute_runs.end() ? runs->second.last_frame : 0,
                        PortStats::flips.load(std::memory_order_relaxed),
                        std::ranges::any_of(sel.infos[0]->images,
                                            [](const auto& image) { return image.is_written; }));
        }
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    CountComputeRun(pgm_hash);
    if (it->second && !it->second->hot_noted) {
        NoteHot(*it->second, 'c', compute_key.value);
    }
    return it->second.get();
}

namespace {
/// Images of `a` (a new permutation) that differ from `b` in their T#-derived types only
/// (dimension, integer or sRGB format, cube, swizzle): their count, or none when anything else
/// differs (buffers, samplers, a storage image, the number of descriptors).
std::optional<u32> ImageOnlyDifference(const Shader::StageSpecialization& a,
                                       const Shader::StageSpecialization& b) {
    if (a.images.size() != b.images.size()) {
        return std::nullopt;
    }
    auto same = a;
    u32 count = 0;
    for (u32 i = 0; i < a.images.size(); ++i) {
        const auto& x = a.images[i];
        const auto& y = b.images[i];
        if (x == y) {
            continue;
        }
        if (x.is_storage || y.is_storage || x.num_bindings != y.num_bindings ||
            x.is_fmask != y.is_fmask) {
            return std::nullopt;
        }
        same.images[i] = y;
        ++count;
    }
    return count && same == b ? std::optional<u32>{count} : std::nullopt;
}
} // namespace

// port: while a new permutation of a skippable compute program builds, its dispatches run on a
// built permutation that differs only in the types of some images, with those images bound null
// (Rasterizer::BindTextures). The Order's particle lighting (0x804aeead) samples ~30 slots whose
// T#s change dimension and format with the lights in view: hundreds of permutations, ~40 new
// ones each session, and every skipped dispatch was smoke and particles blinking.
const ComputePipeline* PipelineCache::FallbackComputePipeline(u64 pgm_hash) {
    if (PortToggle::Disabled(PortToggle::ComputeFallback)) {
        return nullptr;
    }
    const auto program = program_cache.find(pgm_hash);
    if (program == program_cache.end()) {
        return nullptr;
    }
    const auto& modules = program->second->modules;
    const auto current = std::ranges::find(modules, sel.modules[0], &Program::Module::module);
    if (current == modules.end()) {
        return nullptr;
    }
    const ComputePipeline* best = nullptr;
    size_t best_index = 0;
    u32 best_count = ~0u;
    for (size_t i = 0; i < modules.size(); ++i) {
        const auto found = compute_pipelines.find(ComputePipelineKey{HashCombine(pgm_hash, i)});
        if (found == compute_pipelines.end() || !found->second) {
            continue;
        }
        if (const auto count = ImageOnlyDifference(current->spec, modules[i].spec);
            count && *count < best_count) {
            best = found->second.get();
            best_index = i;
            best_count = *count;
        }
    }
    static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
    static std::unordered_set<size_t> logged;
    if (log && logged.insert(compute_key.value).second) {
        u32 built = 0, same_shape = 0;
        for (size_t i = 0; i < modules.size(); ++i) {
            if (compute_pipelines.contains(ComputePipelineKey{HashCombine(pgm_hash, i)})) {
                ++built;
                same_shape += modules[i].spec.images.size() == current->spec.images.size() &&
                              modules[i].spec.buffers == current->spec.buffers &&
                              modules[i].spec.samplers == current->spec.samplers &&
                              modules[i].spec.bitset == current->spec.bitset;
            }
        }
        LOG_WARNING(Render_Vulkan, "port: compute {:#x} permutation {}: {} of {} permutations "
                    "built, {} with the same buffers/samplers/bound set, fallback {}",
                    pgm_hash, std::distance(modules.begin(), current), built, modules.size(),
                    same_shape, best ? fmt::format("{} ({} images)", best_index, best_count)
                                     : std::string{"none"});
    }
    if (best) {
        best->SetFallbackImages(modules[best_index].spec.images);
    }
    return best;
}

void PipelineCache::CountComputeRun(u64 pgm_hash) {
    const u64 frame = PortStats::flips.load(std::memory_order_relaxed);
    auto& runs = compute_runs[pgm_hash];
    if (runs.last_frame != frame) {
        runs.last_frame = frame;
        ++runs.frames;
    }
}

// port: a new compute permutation is built on the compiler threads, its dispatches skipped
// until it is ready, only when skipping loses nothing for good: the program ran in several
// frames up to the last few (work redone every frame, as The Order's particle lighting shader,
// whose permutations follow the light volumes in view: ~200 ms driver compiles on the GPU
// thread), and it writes no image (Iggy copies each glyph into its textures once: the menus
// lost their text). THEORDER_ASYNC_COMPUTE=0 never skips, 1 always does.
bool PipelineCache::SkippableDispatch(const Shader::Info& info) const {
    static const char mode = [] {
        const char* env = std::getenv("THEORDER_ASYNC_COMPUTE");
        return env ? env[0] : '\0';
    }();
    if (mode == '0' || mode == '1') {
        return mode == '1';
    }
    // A frame can go without the program (the guest flips one frame twice).
    constexpr u32 MinFrames = 8, MaxIdleFlips = 4;
    const auto runs = compute_runs.find(info.pgm_hash);
    const u64 frame = PortStats::flips.load(std::memory_order_relaxed);
    if (runs == compute_runs.end() || runs->second.frames < MinFrames ||
        runs->second.last_frame + MaxIdleFlips < frame) {
        return false;
    }
    return std::ranges::none_of(info.images, [](const auto& image) { return image.is_written; });
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    // port: a depth/stencil buffer that cannot affect the draw is not attached: on the PC a
    // smaller one would crop a full-screen pass (eltutz, vk_depth_stencil_state.h).
    const bool db_enabled = GetEffectiveDepthStencilState(regs).needs_attachment;

    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // THEORDER_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("THEORDER_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("THEORDER_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;
    sel.compilation_pending = false;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx], sel.spv_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (!sel.infos[stage_out_idx]) {
            sel.compilation_pending = true;
            return false;
        }
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.spv_hashes.fill(0);
    sel.fresh_permutation = false;

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.compilation_pending) {
        return false;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        if (sel.compilation_pending) {
            return false;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    sel.fresh_permutation = false;
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value,
             sel.spv_hashes[0]) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return sel.infos[0] != nullptr;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding, u64& spv_hash) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    PhaseTimer translate{g_port_translate_ns};
    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    translate.Stop();
    PhaseTimer emit{g_port_emit_ns};
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    emit.Stop();
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    if (info.sw_stage == SwStage::Vertex) {
        BbFaceDepth::Register(module, is_patched ? std::span<const u32>(*patch) :
                                               std::span<const u32>(spv), info.pgm_hash);
    }

    spv_hash = RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // port: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        if (BbParticleSrt::Enabled()) BbParticleSrt::Normalize(info);
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end() || !it->module) { // a deferred module loads on stage A
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        sel.fresh_permutation |= it->fresh;
        return std::make_tuple(&info, it->module, ToFetchShader(it->spec.fetch_shader_data),
                               HashCombine(params.hash, perm_idx), it->spv_hash);
    }

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    if (it_pgm == program_cache.end() && LoadCachedProgram(params.hash, hw_stage)) {
        it_pgm = program_cache.find(params.hash);
    }
    const bool compile_async = AsyncCompilation() && hw_stage != HwStage::Compute &&
                               !sel.regs->stage_enable.hs_en &&
                               !EmulatorSettings.IsShaderCollect();
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->pending) {
        const size_t permutation = it_pgm->second->pending->permutation;
        const auto wait_start = std::chrono::steady_clock::now();
        {
            PhaseTimer waiting{g_port_compile_wait_ns};
            it_pgm->second->pending->job.Wait();
        }
        const double wait_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - wait_start)
                                   .count();
        static const bool log_shader_waits =
            std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (log_shader_waits && wait_ms >= 0.1) {
            LOG_WARNING(Render_Vulkan,
                     "Shader compilation wait: flip {} shader 0x{:016x} stage {} permutation {} "
                     "wait {:.3f} ms",
                     PortStats::flips.load(std::memory_order_relaxed), params.hash,
                     static_cast<u32>(hw_stage), permutation, wait_ms);
        }
        PublishShaders();
    }
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->modules.empty() &&
        !it_pgm->second->compilation_failed) {
        std::unique_lock lock{programs_mutex};
        program_cache.erase(it_pgm);
        it_pgm = program_cache.end();
    }
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        if (compile_async) {
            QueueShader(*new_program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
            std::unique_lock lock{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
            return {};
        }
        auto start = binding;
        u64 spv_hash{};
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding, spv_hash);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, 0,
                           spv_hash);
        new_program->AddPermut(module, std::move(spec), spv_hash);
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        sel.fresh_permutation = true;
        return std::make_tuple(&program->info, module,
                               ToFetchShader(program->modules[0].spec.fetch_shader_data),
                               perm_hash, spv_hash);
    }

    auto& program = it_pgm.value();
    if (program->compilation_failed || (program->modules.empty() && program->pending)) {
        return {};
    }
    if (program->modules.empty() && compile_async) {
        QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
        return {};
    }
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    if (BbParticleSrt::Enabled()) BbParticleSrt::Normalize(info);
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // port: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end() && !it->module &&
        !LoadDeferredModule(*program, std::distance(program->modules.begin(), it))) {
        it = program->modules.end(); // its binary is gone: compile it again
    }
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        if (program->pending || std::ranges::find(program->failed_permutations, spec) !=
                                    program->failed_permutations.end()) {
            return {};
        }
        if (compile_async) {
            if (!program->modules.empty()) {
                LogNewPermutation(params.hash, perm_idx, spec,
                                  program->modules[std::min(program->last_used,
                                                            program->modules.size() - 1)].spec);
            }
            QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, perm_idx, false,
                        std::move(spec));
            return {};
        }
        if (!program->modules.empty()) {
            LogNewPermutation(params.hash, perm_idx, spec,
                              program->modules[std::min(program->last_used,
                                                        program->modules.size() - 1)].spec);
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        u64 spv_hash{};
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding, spv_hash);
        static const bool check_shape = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
        if (check_shape && (new_info.buffers.size() != info.buffers.size() ||
                            new_info.images.size() != info.images.size() ||
                            new_info.samplers.size() != info.samplers.size() ||
                            new_info.fmasks.size() != info.fmasks.size())) {
            LOG_WARNING(Render_Vulkan, "port: shader {:#x} permutation {} resources {}/{}/{}/{} "
                        "(buffers/images/samplers/fmasks), program {}/{}/{}/{}",
                        params.hash, perm_idx, new_info.buffers.size(), new_info.images.size(),
                        new_info.samplers.size(), new_info.fmasks.size(), info.buffers.size(),
                        info.images.size(), info.samplers.size(), info.fmasks.size());
        }
        for (u32 i = 0; check_shape && i < std::min(new_info.images.size(), info.images.size());
             ++i) {
            const auto& a = new_info.images[i];
            const auto& b = info.images[i];
            if (a.NumBindings(new_info) != b.NumBindings(info) ||
                a.mip_fallback_mode != b.mip_fallback_mode || a.is_written != b.is_written) {
                LOG_WARNING(Render_Vulkan, "port: shader {:#x} permutation {} image {}: bindings "
                            "{}/{}, mip mode {}/{}, written {}/{} (permutation/program)",
                            params.hash, perm_idx, i, a.NumBindings(new_info),
                            b.NumBindings(info), u32(a.mip_fallback_mode),
                            u32(b.mip_fallback_mode), a.is_written, b.is_written);
            }
        }

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx, spv_hash);
        std::unique_lock lk{programs_mutex};
        program->AddPermut(module, std::move(spec), spv_hash);
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    sel.fresh_permutation |= program->modules[perm_idx].fresh;
    static const bool reuse_compute = [] {
        const char* env = std::getenv("THEORDER_COMPUTE_PIPELINE_REUSE");
        return !env || *env != '0';
    }();
    if (hw_stage == HwStage::Compute && reuse_compute &&
        !EmulatorSettings.IsPatchShaders() && !EmulatorSettings.IsShaderCollect()) {
        const bool first_lookup = !program->modules[perm_idx].compute_pipeline_permutation;
        const auto canonical = program->ComputePipelinePermutation(perm_idx);
        if (canonical != perm_idx) {
            static const bool log = std::getenv("THEORDER_PERMUTATION_LOG") != nullptr;
            if (log && first_lookup) {
                std::printf("Compute pipeline reuse: shader %016llx permutation %zu -> %zu "
                            "(SPIR-V %016llx)\n",
                            static_cast<unsigned long long>(params.hash), perm_idx, canonical,
                            static_cast<unsigned long long>(program->modules[perm_idx].spv_hash));
            }
            perm_hash = HashCombine(params.hash, canonical);
        }
    }
    return std::make_tuple(&program->info, module,
                           ToFetchShader(program->modules[perm_idx].spec.fetch_shader_data),
                           perm_hash, program->modules[perm_idx].spv_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    PublishShaders(true);
    PublishGraphics(true);
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                if (program->info.sw_stage == SwStage::Vertex) {
                    BbFaceDepth::Register(m.module, spv_code, program->info.pgm_hash);
                }
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
