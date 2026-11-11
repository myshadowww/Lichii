// SPDX-License-Identifier: GPL-3.0-or-later
// Optional hardware regression for the native Yoshi temporal motion resampling pass.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_yoshi_motion.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace {
using namespace Vulkan;
constexpr VkImageSubresourceRange RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
struct Image {
    vk::Image image;
    vk::ImageView view;
};
Image MakeImage(const Device& device, MemoryAllocator& allocator, VkExtent2D size, VkFormat format,
                VkImageUsageFlags usage) {
    Image result;
    result.image = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    });
    result.view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = RANGE,
    });
    return result;
}
} // namespace

// Exercise the production pass that consumes the game's already-decoded full-resolution motion.
void YoshiTemporalMotionSmoke(const Vulkan::Device& device) {
    using namespace Vulkan;
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    constexpr VkExtent2D capacity{48, 32};
    auto source = MakeImage(device, allocator, capacity, VK_FORMAT_R16G16_SFLOAT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto upload = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          .size = capacity.width * capacity.height * 4,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                         MemoryUsage::Upload);
    auto readback = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .size = capacity.width * capacity.height * 4,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                           MemoryUsage::Download);
    YoshiMotion converter{device, scheduler, allocator, pool, queue};
    for (u32 step = 0; step < 4; ++step) {
        const VkExtent2D render = step % 2 ? VkExtent2D{11, 7} : VkExtent2D{17, 9};
        const VkExtent2D output{render.width * 2, render.height * 2};
        for (u32 y = 0; y < capacity.height; ++y) {
            for (u32 x = 0; x < capacity.width; ++x) {
                const bool active_pixel = x < output.width && y < output.height;
                const std::array<_Float16, 2> value{
                    static_cast<_Float16>(active_pixel ? float(x) - 20.f : 32000.f),
                    static_cast<_Float16>(active_pixel ? float(y) * 2.f - 9.f : -32000.f)};
                std::memcpy(upload.Mapped().data() + (y * capacity.width + x) * 4, value.data(), 4);
            }
        }
        upload.Flush();
        scheduler.Record([&, step](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .srcAccessMask = step ? VK_ACCESS_MEMORY_READ_BIT : 0u,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .oldLayout =
                                         step ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = *source.image,
                                     .subresourceRange = RANGE});
            cmd.CopyBufferToImage(
                *upload, *source.image, VK_IMAGE_LAYOUT_GENERAL,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {capacity.width, capacity.height, 1}});
        });
        const auto result = converter.Record(*source.view, render, output);
        scheduler.Record([&, result, render](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                result.image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {render.width, render.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        readback.Invalidate();
        for (u32 y = 0; y < render.height; ++y) {
            for (u32 x = 0; x < render.width; ++x) {
                std::array<_Float16, 2> value;
                std::memcpy(value.data(), readback.Mapped().data() + (y * render.width + x) * 4, 4);
                // For a 2x output, the native consumer chooses texel (2x+1,2y+1), then halves.
                if (float(value[0]) != float(x) - 9.5f || float(value[1]) != float(y) * 2.f - 3.5f)
                    throw std::runtime_error("Yoshi output-motion sampling/scale/crop regression");
            }
        }
        queue.TickFrame();
    }
    std::puts("Yoshi temporal motion smoke: 4 production resample cases passed");
}
