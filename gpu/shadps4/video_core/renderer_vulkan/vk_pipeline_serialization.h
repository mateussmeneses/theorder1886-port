// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace PipelineBinaryCache {
using Key = std::vector<u8>;
struct Binary {
    Key key;
    std::vector<u8> data;
};
constexpr size_t MaxFileSize = 64 * 1024 * 1024;
constexpr u32 MaxBinaries = 32;

std::vector<u8> Encode(const Key& global, const Key& pipeline, u64 graphics_hash,
                       const std::vector<Binary>& binaries);
std::optional<std::vector<Binary>> Decode(std::span<const u8> bytes, const Key& global,
                                          const Key& pipeline, u64 graphics_hash);
bool Save(const std::filesystem::path& file, std::span<const u8> bytes);
std::optional<std::vector<Binary>> Load(const std::filesystem::path& file, const Key& global,
                                        const Key& pipeline, u64 graphics_hash);
} // namespace PipelineBinaryCache

/// port: set around a build that must only restore a stored binary: a miss returns
/// ePipelineCompileRequired instead of compiling (the caller builds it in the background).
extern thread_local bool g_pipeline_restore_only;

vk::ResultValue<vk::UniquePipeline> CreateCachedGraphicsPipeline(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    const GraphicsPipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory);
vk::ResultValue<vk::UniquePipeline> CreateCachedComputePipeline(
    vk::Device device, vk::PipelineCache cache, const vk::ComputePipelineCreateInfo& info,
    const ComputePipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory);
size_t TrimPipelineBinaries(const std::filesystem::path& directory, u64 budget_bytes = 0);
void ReportPipelineBinaryCache();

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx,
                        u64 spv_hash, bool needs_ssbo_barriers);
/// Returns the SPIR-V hash its meta records (SpvHash).
u64 RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);
/// port: a meta and its SPIR-V are separate files, written by a queue that is not drained at
/// exit, and permutation indices are reused across sessions: the hash pairs them.
u64 SpvHash(std::span<const u32> spv);

} // namespace Vulkan
