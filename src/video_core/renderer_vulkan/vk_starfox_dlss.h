// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"

namespace Vulkan {

class DescriptorPool;

/// Adapts Star Fox's sRGB scene and native motion/depth to the platform DLSS backend.
/// The caller records the spatial repair first; a failed evaluation leaves it intact.
class StarFoxDlss {
public:
    StarFoxDlss(const Device& device, Scheduler& scheduler, MemoryAllocator& allocator,
                DescriptorPool& pool, ComputePassDescriptorQueue& queue);
    ~StarFoxDlss();

    bool Record(NvidiaDlssUpscaler::Inputs inputs, VkImageView target);
    /// Replace the later scene upscale before the guest adds UI. Only this frame's result is used.
    bool WriteUpscaledOutput(VkImageView target, VkExtent2D target_extent);
    void ResetHistory();
    void TickFrame();

private:
    class ColorPass;
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    ComputePassDescriptorQueue& queue;
    std::unique_ptr<ColorPass> color_pass;
    std::unique_ptr<ColorPass> output_pass;
    std::unique_ptr<ColorPass> present_pass;
    vk::Image color;
    vk::ImageView color_view;
    VkExtent2D extent{};
    VkExtent2D output_extent{};
    bool reset = true;
    bool recorded{};
    std::chrono::steady_clock::time_point last_evaluation{};
    u64 evaluations{};
    NvidiaDlssUpscaler upscaler;
};

} // namespace Vulkan
