// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "video_core/mario_kart_upscale.h"
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
namespace Vulkan {
class DescriptorPool;
class StagingBufferPool;
class MarioKartMotion {
public:
    MarioKartMotion(const Device&, Scheduler&, MemoryAllocator&, DescriptorPool&,
                    ComputePassDescriptorQueue&, StagingBufferPool&);
    ~MarioKartMotion();
    NvidiaDlssUpscaler::Image Record(VkImageView depth, VkExtent2D size,
                                     const VideoCommon::MarioKartUpscale::Camera& camera,
                                     const VideoCommon::MarioKartUpscale::Curvature& bend,
                                     u64 frame, bool& reset);
    void ResetHistory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
