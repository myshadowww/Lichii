// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "common/scope_exit.h"
#include "video_core/host_shaders/yoshi_resample_motion_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_yoshi_motion.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
namespace Vulkan {
namespace {
constexpr VkImageSubresourceRange RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr std::array BINDINGS{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
};
constexpr std::array TEMPLATES{
    VkDescriptorUpdateTemplateEntry{0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
                                    sizeof(DescriptorUpdateEntry)},
    VkDescriptorUpdateTemplateEntry{1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                    sizeof(DescriptorUpdateEntry), sizeof(DescriptorUpdateEntry)},
};
struct Parameters {
    VkExtent2D render, output;
};
constexpr std::array PUSH{VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)}};
class Pass final : public ComputePass {
public:
    Pass(const Device& device, Scheduler& scheduler, DescriptorPool& pool)
        : ComputePass(device, scheduler, pool, BINDINGS, TEMPLATES,
                      DescriptorBankInfo{.textures = 1, .images = 1, .score = 2}, PUSH,
                      YOSHI_RESAMPLE_MOTION_COMP_SPV) {}
    void Record(Scheduler& scheduler, const void* descriptors, VkExtent2D size,
                Parameters parameters) {
        scheduler.Record([this, descriptors, size, parameters](vk::CommandBuffer cmd) {
            const VkMemoryBarrier before{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
            const auto set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, parameters);
            cmd.Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
            const VkMemoryBarrier after{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, after);
        });
    }
};
} // namespace
struct YoshiMotion::Impl {
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    ComputePassDescriptorQueue& queue;
    Pass pass;
    vk::Image image;
    vk::ImageView view;
    vk::Sampler sampler;
    VkExtent2D extent{};
    Impl(const Device& d, Scheduler& s, MemoryAllocator& a, DescriptorPool& p,
         ComputePassDescriptorQueue& q)
        : device{d}, scheduler{s}, allocator{a}, queue{q}, pass{d, s, p} {
        sampler = device.GetLogical().CreateSampler(
            VkSamplerCreateInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = 0});
    }
    NvidiaDlssUpscaler::Image Record(VkImageView source, VkExtent2D size, VkExtent2D output) {
        if (!source || !size.width || !size.height || !output.width || !output.height)
            return {};
        SCOPE_EXIT {
            scheduler.InvalidateState();
        };
        scheduler.RequestOutsideRenderPassOperationContext();
        if (!image || size.width > extent.width || size.height > extent.height) {
            if (image)
                scheduler.Finish();
            extent = {std::max(extent.width, size.width), std::max(extent.height, size.height)};
            view.reset();
            image = allocator.CreateImage(
                VkImageCreateInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = VK_FORMAT_R16G16_SFLOAT,
                                  .extent = {extent.width, extent.height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE});
            view = device.GetLogical().CreateImageView(
                VkImageViewCreateInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = *image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = VK_FORMAT_R16G16_SFLOAT,
                                      .subresourceRange = RANGE});
            scheduler.Record([handle = *image](vk::CommandBuffer cmd) {
                const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                   .image = handle,
                                                   .subresourceRange = RANGE};
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
            });
        }
        queue.Acquire(scheduler, 2);
        queue.AddSampledImage(source, *sampler);
        queue.AddImage(*view);
        pass.Record(scheduler, queue.UpdateData(), size, {size, output});
        return {*view, *image, VK_FORMAT_R16G16_SFLOAT, RANGE, VK_IMAGE_ASPECT_COLOR_BIT, extent};
    }
};
YoshiMotion::YoshiMotion(const Device& d, Scheduler& s, MemoryAllocator& a, DescriptorPool& p,
                         ComputePassDescriptorQueue& q)
    : impl{std::make_unique<Impl>(d, s, a, p, q)} {}
YoshiMotion::~YoshiMotion() {
    if (impl->image)
        impl->scheduler.Finish();
}
NvidiaDlssUpscaler::Image YoshiMotion::Record(VkImageView source, VkExtent2D render,
                                              VkExtent2D output) {
    return impl->Record(source, render, output);
}
} // namespace Vulkan
