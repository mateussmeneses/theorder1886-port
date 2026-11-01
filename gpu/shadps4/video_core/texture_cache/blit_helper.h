// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

class Image;
class ImageView;
struct ImageInfo;

class BlitHelper {
    static constexpr size_t MaxMsPipelines = 6;

public:
    explicit BlitHelper(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~BlitHelper();

    void ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples, bool src_msaa,
                                   vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                   vk::Image source, vk::Image dest);

    void CopyBetweenMsImages(u32 width, u32 height, u32 num_samples, vk::Format pixel_format,
                             bool src_msaa, vk::Image source, vk::Image dest,
                             bool pack_depth_samples = false);

private:
    void CreateShaders();
    void CreatePipelineLayouts();

    struct MsPipelineKey {
        u32 num_samples;
        vk::Format attachment_format;
        bool src_msaa;
        bool pack_depth_samples{};

        auto operator<=>(const MsPipelineKey&) const noexcept = default;
    };
    void CreateColorToMSDepthPipeline(const MsPipelineKey& key);
    void CreateMsCopyPipeline(const MsPipelineKey& key);
    /// port: the copy shader matching the attachment format's numeric class.
    vk::ShaderModule CopyShader(const MsPipelineKey& key) const;

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    vk::UniqueDescriptorSetLayout single_texture_descriptor_set_layout;
    vk::UniquePipelineLayout single_texture_pl_layout;
    vk::ShaderModule fs_tri_vertex;
    vk::ShaderModule color_to_ms_depth_frag;
    vk::ShaderModule color_to_ms_depth_msaa_frag;
    vk::ShaderModule src_msaa_copy_frag;
    vk::ShaderModule src_non_msaa_copy_frag;
    vk::ShaderModule ms_depth_to_color_frag;
    /// port: [0] unsigned, [1] signed integer formats; index 1 of each pair reads MSAA.
    std::array<std::array<vk::ShaderModule, 2>, 2> int_copy_frag{};

    using MsPipeline = std::pair<MsPipelineKey, vk::UniquePipeline>;
    std::vector<MsPipeline> color_to_ms_depth_pl;
    std::vector<MsPipeline> ms_image_copy_pl;
};

} // namespace VideoCore
