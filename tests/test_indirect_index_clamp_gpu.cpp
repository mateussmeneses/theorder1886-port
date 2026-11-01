// SPDX-License-Identifier: GPL-2.0-or-later
// Executes the production indirect_index_clamp.comp against known arguments on a real device.
//
// This is the stronger half of the clamp validation: test_indirect_index_clamp.cpp proves the
// planning arithmetic, but only the shader can prove that a VALID draw's arguments are copied
// through unchanged (the requirement behind "valid draws produce the same result"). The same
// dispatch is also checked against a record that exceeds the bound, so "clamps" and "leaves
// alone" are both observed rather than argued.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/indirect_index_clamp_comp.h"
#include <vk_mem_alloc.h>

// Match the renderer's Vulkan-Hpp dispatcher ABI before enabling test assertions.
#undef NDEBUG
#include <cassert>

namespace {

// VkDrawIndexedIndirectCommand, five dwords.
struct Record {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    int32_t vertex_offset; // signed in VkDrawIndexedIndirectCommand
    u32 first_instance;
};

bool SameRecord(const Record& a, const Record& b) {
    return std::memcmp(&a, &b, sizeof(Record)) == 0;
}

const char* Describe(const Record& r) {
    static char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "{count=%u, inst=%u, first=%u, vtxoff=%d, inst0=%u}",
                  r.index_count, r.instance_count, r.first_index, int(r.vertex_offset),
                  r.first_instance);
    return buffer;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // a crash mid-run must not hide the checks that ran
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();

    // Push constants of the production shader: count, first dword, stride in dwords, max indices.
    struct Push {
        u32 count, first, stride, max_indices;
    };
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    for (u32 i = 0; i < 2; ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    auto set_layout = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = 2,
         .pBindings = bindings.data()},
        nullptr, d).value;
    auto layout = device.createPipelineLayoutUnique(
        {.setLayoutCount = 1,
         .pSetLayouts = &*set_layout,
         .pushConstantRangeCount = 1,
         .pPushConstantRanges = &push_range},
        nullptr, d).value;
    auto module = device.createShaderModuleUnique(
        {.codeSize = sizeof(INDIRECT_INDEX_CLAMP_COMP), .pCode = INDIRECT_INDEX_CLAMP_COMP},
        nullptr, d).value;
    auto pipeline = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
         .layout = *layout},
        nullptr, d).value;

    // Storage buffers for source records and clamped output, plus a host-visible readback.
    constexpr u32 MaxRecords = 8;
    constexpr u64 RecordsBytes = MaxRecords * sizeof(Record);
    const auto make_buffer = [&](u64 size, bool storage) {
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        const VkBufferCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        (storage ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0)),
        };
        const VmaAllocationCreateInfo ac{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };
        assert(vmaCreateBuffer(instance.GetAllocator(), &ci, &ac, &buffer, &allocation, &info) ==
               VK_SUCCESS);
        return std::tuple{buffer, allocation, info};
    };
    auto [src_buf, src_alloc, src_info] = make_buffer(RecordsBytes, true);
    auto [dst_buf, dst_alloc, dst_info] = make_buffer(RecordsBytes, true);
    auto [read_buf, read_alloc, read_info] = make_buffer(RecordsBytes, false);

    Vulkan::Scheduler scheduler(instance);

    // Runs one clamp dispatch over `records` and returns what the shader wrote.
    const auto run = [&](const std::vector<Record>& records, u32 max_indices) {
        std::memset(src_info.pMappedData, 0, RecordsBytes);
        std::memcpy(src_info.pMappedData, records.data(), records.size() * sizeof(Record));
        std::memset(dst_info.pMappedData, 0xCD, RecordsBytes); // poison: prove every dword is written

        const Push push{u32(records.size()), 0, 5, max_indices};
        scheduler.Record([&](vk::CommandBuffer cmd) {
            // Host writes visible to the dispatch.
            const vk::MemoryBarrier2 to_compute{
                .srcStageMask = vk::PipelineStageFlagBits2::eHost,
                .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_compute});

            const std::array<vk::DescriptorBufferInfo, 2> infos{{
                {src_buf, 0, RecordsBytes},
                {dst_buf, 0, RecordsBytes},
            }};
            std::array<vk::WriteDescriptorSet, 2> writes{};
            for (u32 i = 0; i < 2; ++i) {
                writes[i] = {.dstBinding = i,
                             .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &infos[i]};
            }
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
            cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmd.dispatch((push.count + 63) / 64, 1, 1);

            const vk::MemoryBarrier2 to_transfer{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_transfer});
            cmd.copyBuffer(dst_buf, read_buf,
                           vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = RecordsBytes});
            const vk::MemoryBarrier2 to_host{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        });
        scheduler.Finish();

        std::vector<Record> out(records.size());
        std::memcpy(out.data(), read_info.pMappedData, records.size() * sizeof(Record));
        return out;
    };

    unsigned failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };

    // --- The requirement: a valid draw is copied through byte-for-byte unchanged. ------------
    {
        const std::vector<Record> in{
            {.index_count = 300, .instance_count = 1, .first_index = 0, .vertex_offset = 0, .first_instance = 0},
            {.index_count = 1, .instance_count = 7, .first_index = 999, .vertex_offset = -5, .first_instance = 3},
            {.index_count = 0, .instance_count = 0, .first_index = 0, .vertex_offset = 0, .first_instance = 0},
            {.index_count = 1000, .instance_count = 2, .first_index = 0, .vertex_offset = 12345, .first_instance = 9},
        };
        const auto out = run(in, /*max_indices=*/1000);
        for (size_t i = 0; i < in.size(); ++i) {
            if (!SameRecord(in[i], out[i])) {
                std::printf("FAIL: valid record %zu changed\n  in  %s\n  out %s\n", i, Describe(in[i]),
                            Describe(out[i]));
                ++failures;
            }
        }
        std::printf("Valid draw with 4 records (last exactly reaching the bound): unchanged\n");
    }

    // A count that fits but a firstIndex that lands at the very end must also survive.
    {
        const std::vector<Record> in{
            {.index_count = 1, .instance_count = 1, .first_index = 999, .vertex_offset = 0, .first_instance = 0},
        };
        const auto out = run(in, 1000);
        check(SameRecord(in[0], out[0]), "record ending exactly at the bound was modified");
        std::printf("Edge record (first=999, count=1, bound=1000): unchanged\n");
    }

    // --- A count past the bound is clamped, and everything else is preserved. ----------------
    {
        const Record in{.index_count = 5000, .instance_count = 4, .first_index = 900,
                        .vertex_offset = -7, .first_instance = 2};
        const auto out = run({in}, /*max_indices=*/1000);
        check(out[0].index_count == 100, "count was not clamped to max_indices - first_index");
        check(out[0].instance_count == in.instance_count, "instance_count changed");
        check(out[0].first_index == in.first_index, "first_index changed");
        check(out[0].vertex_offset == in.vertex_offset, "vertex_offset changed");
        check(out[0].first_instance == in.first_instance, "first_instance changed");
        std::printf("Oversized record (5000 @ first=900, bound=1000): count -> %u, rest preserved\n",
                    out[0].index_count);
    }

    // The reported case: 0xFFFFFFFA indices from a zero-count source record.
    {
        const Record in{.index_count = 0xFFFFFFFAu, .instance_count = 1, .first_index = 0,
                        .vertex_offset = 0, .first_instance = 0};
        const auto out = run({in}, /*max_indices=*/1000);
        check(out[0].index_count == 1000, "0xFFFFFFFA was not clamped to the bound");
        std::printf("Reported case (0xFFFFFFFA, bound=1000): count -> %u\n", out[0].index_count);
    }

    // firstIndex beyond the bound: nothing is readable, so the count must become zero.
    {
        const Record in{.index_count = 100, .instance_count = 1, .first_index = 5000,
                        .vertex_offset = 0, .first_instance = 0};
        const auto out = run({in}, /*max_indices=*/1000);
        check(out[0].index_count == 0, "firstIndex past the bound did not zero the count");
        std::printf("firstIndex past the bound (5000 > 1000): count -> %u\n", out[0].index_count);
    }

    // A mixed batch: one valid, one oversized. The valid one must be untouched.
    {
        const std::vector<Record> in{
            {.index_count = 60, .instance_count = 1, .first_index = 10, .vertex_offset = 1, .first_instance = 0},
            {.index_count = 9000, .instance_count = 1, .first_index = 0, .vertex_offset = 2, .first_instance = 0},
        };
        const auto out = run(in, /*max_indices=*/500);
        check(SameRecord(in[0], out[0]), "valid record in a mixed batch was modified");
        check(out[1].index_count == 500, "oversized record in a mixed batch was not clamped");
        std::printf("Mixed batch: valid record unchanged, oversized clamped to %u\n",
                    out[1].index_count);
    }

    vmaDestroyBuffer(instance.GetAllocator(), src_buf, src_alloc);
    vmaDestroyBuffer(instance.GetAllocator(), dst_buf, dst_alloc);
    vmaDestroyBuffer(instance.GetAllocator(), read_buf, read_alloc);

    if (failures) {
        std::printf("indirect-index-clamp-gpu-test: %u FAILED\n", failures);
        return 1;
    }
    std::puts("indirect-index-clamp-gpu-test: passed");
    return 0;
}
