// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <functional>
#include <optional>
#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
struct AsyncShaderCompilation;
struct AsyncGraphicsCompilation;
struct AsyncComputeCompilation;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        u64 spv_hash{}; ///< checksum of the persisted SPIR-V, zero if unknown
        /// port: made this session, at the next free index: a stored pipeline key with that
        /// index may be another session's permutation there (not "known", see GetGraphicsPipeline).
        bool fresh = false;
        std::optional<size_t> compute_pipeline_permutation;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< port: permutation of the previous lookup, compared first
    /// port: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;
    std::shared_ptr<AsyncShaderCompilation> pending;
    bool compilation_failed = false;
    std::vector<Shader::StageSpecialization> failed_permutations;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec, u64 spv_hash) {
        modules.emplace_back(module, std::move(spec), spv_hash);
        modules.back().fresh = true;
    }

    size_t ComputePipelinePermutation(size_t index) {
        auto& selected = modules[index];
        if (info.sw_stage != Shader::SwStage::Compute || !selected.spv_hash ||
            !selected.spec.Valid()) {
            return index;
        }
        // The same emitted binary can result from different resource specializations.
        // Keep their metadata, but reuse a pipeline only with the same descriptor ABI.
        // Image specializations must also match: a pipeline may retain a fallback-image
        // mask used by BindTextures even when it is later selected for an exact dispatch.
        const auto compatible = [&](size_t candidate) {
            const auto& other = modules[candidate];
            const auto& a = selected.spec;
            const auto& b = other.spec;
            return other.spv_hash == selected.spv_hash && b.Valid() &&
                   a.runtime_info == b.runtime_info && a.start == b.start &&
                   a.buffers.size() == b.buffers.size() && a.samplers.size() == b.samplers.size() &&
                   a.fmasks.size() == b.fmasks.size() && a.images == b.images;
        };
        if (selected.compute_pipeline_permutation &&
            compatible(*selected.compute_pipeline_permutation)) {
            return *selected.compute_pipeline_permutation;
        }
        for (size_t candidate = 0; candidate < index; ++candidate) {
            if (compatible(candidate)) {
                selected.compute_pipeline_permutation = candidate;
                return candidate;
            }
        }
        selected.compute_pipeline_permutation = index;
        return index;
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx, u64 spv_hash = 0) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec), spv_hash};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    bool is_indirect;

    static constexpr DrawIndirectParams Indirect(u16 vertex_sgpr_offset,
                                                 u32 instance_sgpr_offset) {
        return {.vertex_sgpr_offset = vertex_sgpr_offset,
                .instance_sgpr_offset = instance_sgpr_offset,
                .is_indirect = true};
    }
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// port: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::array<u64, MaxShaderStages> spv_hashes{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    bool compilation_pending = false;
    bool fresh_permutation = false; ///< port: a stage uses a Program::Module::fresh permutation
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// port: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// Loads the pipelines of earlier sessions. port: called by the presenter once the window
    /// can show a loading screen; `progress(done, total)` follows the pipelines read.
    void WarmUp(const std::function<void(u32 done, u32 total)>& progress = {});
    /// port: the same load on low-priority threads while the game runs; the GPU thread takes
    /// the results a few at a time (PumpWarmUp). False when it cannot (zip archive store).
    bool StartBackgroundWarmUp();
    void StartOnDemand();
    void Sync();
    void ReportCompilerStats();

    /// port: driver pipeline cache persistence (BP08 from iss_pc)
    void StartDriverCacheSaver();
    void StopDriverCacheSaver();
    bool SaveDriverCache();
    bool SaveDriverCacheUnchecked();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);
    /// port: the cached permutations of a program seen for the first time this session.
    bool LoadCachedProgram(u64 pgm_hash, Shader::HwStage stage);
    /// port: the module of a cached permutation, read when its specialization is first used.
    vk::ShaderModule LoadDeferredModule(Program& program, size_t perm_idx);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// port (FEAT-004 Class C): wait on and retire exactly ONE in-flight graphics build, the one
    /// keyed by `key`. Returns the finished pipeline, or nullptr if `key` is not pending or its
    /// build failed. Reuses the PublishGraphics retire steps; never waits on more than one key.
    const GraphicsPipeline* WaitGraphics(const GraphicsPipelineKey& key,
                                         const PreparedDraw* prepared = nullptr,
                                         const char* source = "unknown");

    /// port (FEAT-004 Class C): whether the just-returned null from GetGraphicsPipeline came from
    /// a still-compiling (pending) key rather than a failed/absent one. Mirrors UsedPrepared().
    [[nodiscard]] bool LastLookupWasPending() const noexcept {
        return last_lookup_pending;
    }
    /// port (FEAT-004): the key GetGraphicsPipeline last found pending, for WaitGraphics().
    [[nodiscard]] const GraphicsPipelineKey& LastPendingGraphicsKey() const noexcept {
        return last_pending_key;
    }

    /// port: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// port: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// port: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool QueueShader(Program& program, Shader::HwStage stage, Shader::SwStage l_stage,
                     const Shader::ShaderParams& params, Shader::RuntimeInfo runtime_info,
                     Shader::Backend::Bindings binding, size_t permutation, bool initial,
                     std::optional<Shader::StageSpecialization> specialization = {});
    void PublishShaders(bool wait = false);
    void PublishGraphics(bool wait = false);
    void PublishCompute(bool wait = false);
    void FinishCompilations();
    void SeedWorkerCaches();
    bool CheckCacheProfile();
    struct BackgroundWarmup;
    struct WarmStage;
    void ReadWarmEntry(BackgroundWarmup& warm, std::vector<u8>&& data);
    /// port: the pipelines recent sessions used (hot_pipelines.txt beside the cache), restored
    /// in the background at start: on demand, each one met in the game skipped its draws for a
    /// frame or more while it came back from the binary cache (objects blinking).
    void StartHotWarmUp();
    void NoteHot(const Pipeline& pipeline, char kind, u64 hash);
    std::FILE* hot_list = nullptr;
    bool MergeWarmStage(WarmStage& stage);
    void PumpWarmUp();
    void StopWarmUp();
    struct DriverCacheSaver;
    DriverCacheSaver* driver_saver = nullptr;
    /// port: the stored shader metas, read on a background thread at start so that
    /// LoadCachedProgram does not open a file per permutation on the GPU thread (~1 ms each,
    /// a quarter of a second for a material shader with 256 cached permutations).
    struct MetaPrefetch {
        std::atomic<bool> ready{false};
        std::unordered_map<u64, std::vector<u8>> metas; ///< by permutation hash
        std::jthread thread;
    };
    std::unique_ptr<MetaPrefetch> meta_prefetch;
    bool sync_compile = false;
    bool AsyncCompilation() const;
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding, u64& spv_hash);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    CompilerPool<Shader::Pools> compiler_pools{8};
    std::unique_ptr<AsyncCompiler> compiler, capture_compiler;
    std::vector<vk::UniquePipelineCache> worker_pipeline_caches;
    bool worker_caches_seeded = false;
    /// port: THEORDER_PARALLEL_WARMUP: WarmUp builds the cached pipelines on these threads.
    struct WarmupPool;
    WarmupPool* warmup_pool = nullptr;
    std::mutex warmup_results_mutex;
    std::vector<std::pair<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>>> warmup_graphics;
    std::vector<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> warmup_compute;
    /// port: StartBackgroundWarmUp state; owned here, deleted by StopWarmUp.
    BackgroundWarmup* background = nullptr;
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<AsyncGraphicsCompilation>> pending_graphics;
    /// port: exceptional graphics builds are terminal for this session; retain no snapshots.
    std::unordered_set<GraphicsPipelineKey> failed_graphics;
    tsl::robin_map<ComputePipelineKey, std::shared_ptr<AsyncComputeCompilation>> pending_compute;
    /// port: compute pipelines whose asynchronous build failed: built on the GPU thread instead.
    std::unordered_set<u64> sync_compute_keys;
    /// port: frames each compute program ran in, to tell work redone every frame (which a skipped
    /// dispatch only delays) from one-shot work (SkippableDispatch).
    struct ComputeRuns {
        u64 last_frame = 0;
        u32 frames = 0;
    };
    tsl::robin_map<u64, ComputeRuns> compute_runs;
    bool SkippableDispatch(const Shader::Info& info) const;
    const ComputePipeline* FallbackComputePipeline(u64 pgm_hash);
    void CountComputeRun(u64 pgm_hash);
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    std::vector<Program*> pending_programs;
    /// port: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    /// port (FEAT-004): side-channel from the last GetGraphicsPipeline lookup that returned null,
    /// recording whether that null was a pending (still-compiling) key and which key it was, so
    /// the rasterizer can stall on exactly that one build (WaitGraphics).
    bool last_lookup_pending = false;
    GraphicsPipelineKey last_pending_key{};
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
