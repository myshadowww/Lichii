// SPDX-License-Identifier: GPL-3.0-or-later

// Engine declarations must precede Xlib's Success/None macros from the Vulkan headers.
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_starfox_dlss.h"

#include "common/logging.h"
#include "video_core/host_shaders/starfox_dlss_color_comp_spv.h"
#include "video_core/host_shaders/starfox_dlss_output_comp_spv.h"
#include "video_core/host_shaders/starfox_dlss_present_comp_spv.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr VkImageSubresourceRange COLOR_RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::array<VkDescriptorSetLayoutBinding, 2> BINDINGS{{
    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
}};
constexpr std::array<VkDescriptorUpdateTemplateEntry, 2> TEMPLATES{{
    {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, sizeof(DescriptorUpdateEntry)},
    {1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(DescriptorUpdateEntry),
     sizeof(DescriptorUpdateEntry)},
}};
constexpr DescriptorBankInfo BANK{.textures = 1, .images = 1, .score = 2};
} // namespace

class StarFoxDlss::ColorPass final : public ComputePass {
public:
    ColorPass(const Device& device_, Scheduler& scheduler, DescriptorPool& pool,
              std::span<const u32> code)
        : ComputePass(device_, scheduler, pool, BINDINGS, TEMPLATES, BANK, {}, code) {}

    void Record(Scheduler& scheduler, const void* descriptors, VkExtent2D size,
                const NvidiaDlssUpscaler* require_success = nullptr) {
        scheduler.Record([this, descriptors, size, require_success](vk::CommandBuffer cmd) {
            if (require_success && require_success->Failed()) {
                return;
            }
            const VkMemoryBarrier before{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
            const VkMemoryBarrier after{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, after);
        });
    }
};

StarFoxDlss::StarFoxDlss(const Device& device_, Scheduler& scheduler_, MemoryAllocator& allocator_,
                         DescriptorPool& pool, ComputePassDescriptorQueue& queue_)
    : device{device_}, scheduler{scheduler_}, allocator{allocator_}, queue{queue_},
      color_pass{std::make_unique<ColorPass>(device, scheduler, pool, STARFOX_DLSS_COLOR_COMP_SPV)},
      output_pass{
          std::make_unique<ColorPass>(device, scheduler, pool, STARFOX_DLSS_OUTPUT_COMP_SPV)},
      present_pass{
          std::make_unique<ColorPass>(device, scheduler, pool, STARFOX_DLSS_PRESENT_COMP_SPV)},
      upscaler{device, scheduler, allocator} {}

StarFoxDlss::~StarFoxDlss() = default;

void StarFoxDlss::ResetHistory() {
    reset = true;
}

void StarFoxDlss::TickFrame() {
    if (!recorded) {
        reset = true;
    }
    recorded = false;
}

bool StarFoxDlss::Record(NvidiaDlssUpscaler::Inputs inputs, VkImageView target) {
    if (upscaler.Failed()) {
        return false;
    }
    if (!color || extent.width != inputs.render_size.width ||
        extent.height != inputs.render_size.height) {
        if (color) {
            scheduler.Finish();
        }
        extent = inputs.render_size;
        color_view.reset();
        color = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = COLOR_FORMAT,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        });
        color_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = *color,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = COLOR_FORMAT,
            .subresourceRange = COLOR_RANGE,
        });
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([image = *color](vk::CommandBuffer cmd) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = COLOR_RANGE,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
        });
        reset = true;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    queue.Acquire(scheduler, 2);
    queue.AddSampledImage(inputs.color.view, upscaler.PointSampler());
    queue.AddImage(*color_view);
    color_pass->Record(scheduler, queue.UpdateData(), extent);

    const auto now = std::chrono::steady_clock::now();
    inputs.reset |= reset || now - last_evaluation > std::chrono::milliseconds{250};
    inputs.color = {*color_view, *color, COLOR_FORMAT, COLOR_RANGE, VK_IMAGE_ASPECT_COLOR_BIT};
    if (!upscaler.Upscale(inputs)) {
        reset = true;
        scheduler.InvalidateState();
        return false;
    }
    queue.Acquire(scheduler, 2);
    queue.AddSampledImage(upscaler.OutputView(), upscaler.LinearSampler());
    queue.AddImage(target);
    output_pass->Record(scheduler, queue.UpdateData(), extent, &upscaler);
    scheduler.InvalidateState();
    if (evaluations++ % 120 == 0 || inputs.reset) {
        LOG_INFO(
            Render_Vulkan,
            "Star Fox NVIDIA DLSS queued: {}x{} -> {}x{}, jitter={},{} motion_scale={},{} reset={}",
            extent.width, extent.height, inputs.upscale_size.width, inputs.upscale_size.height,
            inputs.jitter[0], inputs.jitter[1], inputs.motion_vector_scale[0],
            inputs.motion_vector_scale[1], inputs.reset);
    }
    last_evaluation = now;
    output_extent = inputs.upscale_size;
    reset = false;
    recorded = true;
    return true;
}

bool StarFoxDlss::WriteUpscaledOutput(VkImageView target, VkExtent2D target_extent) {
    if (!recorded || upscaler.Failed() || output_extent.width != target_extent.width ||
        output_extent.height != target_extent.height || output_extent.width <= extent.width ||
        output_extent.height <= extent.height) {
        return false;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    queue.Acquire(scheduler, 2);
    queue.AddSampledImage(upscaler.OutputView(), upscaler.PointSampler());
    queue.AddImage(target);
    present_pass->Record(scheduler, queue.UpdateData(), output_extent, &upscaler);
    scheduler.InvalidateState();
    if (evaluations % 120 == 1) {
        LOG_INFO(Render_Vulkan, "Star Fox NVIDIA DLSS scene output queued: {}x{} before guest UI",
                 output_extent.width, output_extent.height);
    }
    return true;
}

} // namespace Vulkan
