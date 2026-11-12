// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <memory>
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"

namespace Vulkan {
class DescriptorPool;

// Scene upscaling for explicitly identified passes. The title adapter validates the
// resource chain; the native pass always runs before a successful evaluation writes its result.
class SceneUpscaler {
public:
    struct Camera {
        std::array<f32, 16> view_projection;
        std::array<f32, 16> projection;
        std::array<f32, 12> view;
        std::array<f32, 4> pixel_to_clip; // normalized pixel XY scale, then bias
        std::array<f32, 2> depth_to_clip;
        bool focal_scale_animation{}; // Opt-in only for a proven constant-aspect native zoom.
    };
    // A validated native scene transfer applied after reconstruction. No transform means
    // identity; this does not change the linear inputs or exposure supplied to DLSS.
    struct OutputTransform {
        std::array<f32, 4> luminance; // base scale, extra scale, luminance cap, enabled
        std::array<f32, 4> shoulder;  // threshold, exp2 slope, exp2 offset, final scale
    };
    // Explicit subregions of shared scene attachments. Extents describe actual image views;
    // render/upscale_size describe the active tile. Scaled sRGB is decoded before DLSS and
    // encoded after reconstruction. Each independently moving camera needs its own instance.
    struct Region {
        VkOffset2D color_origin{}, depth_origin{}, target_origin{};
        VkExtent2D target_extent{};
        bool scaled_srgb{};
        f32 srgb_scale = 1.0f;
    };
    SceneUpscaler(const Device&, Scheduler&, MemoryAllocator&, DescriptorPool&,
                  ComputePassDescriptorQueue&);
    ~SceneUpscaler();
    bool Record(NvidiaDlssUpscaler::Inputs inputs, VkImageView target, const Camera& camera,
                u64 frame, VkFormat target_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                const Region* region = nullptr);
    // Resample the entire unjittered source crop into render_size before reconstructing motion.
    // Input extents describe the actual views; source_extent excludes any backing padding.
    bool RecordResampled(NvidiaDlssUpscaler::Inputs inputs, VkImageView target,
                         const Camera& camera, u64 frame, VkExtent2D source_extent,
                         VkFormat target_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    // Caller-prepared RG16F motion in previous-minus-current render pixels, scale (1,1).
    // Its image/view must remain GENERAL and alive through GPU completion; extent is the
    // actual allocation, with an origin-zero active render crop. The title supplies camera
    // cut/jitter/discontinuity resets in inputs.reset; frame gaps and toggles are also reset here.
    bool RecordWithMotion(NvidiaDlssUpscaler::Inputs inputs, VkImageView target, u64 frame,
                          VkFormat target_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                          const OutputTransform* transform = nullptr);
    void ResetHistory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
