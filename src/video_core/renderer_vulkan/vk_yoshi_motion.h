// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
namespace Vulkan {
class DescriptorPool;
// Converts the validated native temporal prepass's output-pixel motion to render pixels.
class YoshiMotion {
public:
    YoshiMotion(const Device&, Scheduler&, MemoryAllocator&, DescriptorPool&,
                ComputePassDescriptorQueue&);
    ~YoshiMotion();
    NvidiaDlssUpscaler::Image Record(VkImageView source, VkExtent2D render, VkExtent2D output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
