// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

static void Check(bool condition) {
    if (condition) return;
    std::fputs("FAIL: identical compute SPIR-V must reuse a compatible pipeline key\n", stderr);
    std::exit(1);
}

int main() {
    Vulkan::Program program;
    program.info.hw_stage = Shader::HwStage::Compute;
    program.info.sw_stage = Shader::SwStage::Compute;
    Shader::StageSpecialization first;
    first.info = &program.info;
    first.runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    first.buffers.emplace_back();
    first.images.emplace_back();
    first.images[0].num_bindings = 1;
    auto duplicate = first;
    duplicate.buffers[0].stride = 16; // distinct specialization, identical emitted binary
    program.AddPermut({}, std::move(first), 123);
    program.AddPermut({}, std::move(duplicate), 123);
    std::printf("identical binary pipeline index: %zu\n", program.ComputePipelinePermutation(1));
    Check(program.ComputePipelinePermutation(1) == 0);

    program.modules[1].spv_hash = 124;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].spv_hash = 0;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].spv_hash = 123;
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.images[0].num_bindings = 2;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.images[0].num_bindings = 1;
    program.modules[1].spec.images[0].is_storage = true;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.images[0].is_storage = false;
    program.modules[1].spec.images[0].is_srgb = true;
    Check(program.ComputePipelinePermutation(1) == 1); // retained fallback masks must agree
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.images[0].is_srgb = false;
    program.modules[1].spec.start.buffer = 1;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.start.buffer = 0;
    program.modules[1].spec.runtime_info.props.dx10_clamp = 1;
    Check(program.ComputePipelinePermutation(1) == 1);
    program.modules[1].compute_pipeline_permutation.reset();
    program.modules[1].spec.runtime_info.props.dx10_clamp = 0;
    program.info.sw_stage = Shader::SwStage::Vertex;
    Check(program.ComputePipelinePermutation(1) == 1);
    Check(program.StagePipelinePermutation(1) == 0);
    Check(std::as_const(program).StagePipelinePermutation(1) == 0);

    program.modules[1].spec.vs_attribs.push_back({.num_class = AmdGpu::NumberClass::Sint});
    program.modules[1].compute_pipeline_permutation.reset();
    Check(program.StagePipelinePermutation(1) == 1);
    Check(std::as_const(program).StagePipelinePermutation(1) == 1);
    std::puts("PASS: content-identical compute and stage pipeline reuse");
}
