// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Optional hardware test, deliberately excluded from ctest. Does not boot the emulator.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/neural_rendering/runtime.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_starfox_dlss.h"
#include "video_core/renderer_vulkan/vk_scene_upscale.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

void NrCropSmoke(const Vulkan::Device& device);
void NrExposureSmoke(const Vulkan::Device& device);
void YoshiTemporalMotionSmoke(const Vulkan::Device& device);
void MarioKartMotionSmoke(const Vulkan::Device& device);

namespace {
using namespace Vulkan;

/// Fail the hardware smoke test with a diagnostic when an invariant is violated.
void Require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct InputImage {
    vk::Image image;
    vk::ImageView view;
    NvidiaDlssUpscaler::Image resource;
};

InputImage MakeInput(const Device& device, MemoryAllocator& allocator, VkFormat format,
                     VkImageAspectFlags aspect, VkExtent2D extent,
                     VkImageUsageFlags extra_usage = 0, VkImageAspectFlags barrier_aspect = 0) {
    InputImage result;
    result.image = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    result.view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = range,
    });
    result.resource = {*result.view, *result.image, format, range,
                       barrier_aspect ? barrier_aspect : aspect};
    return result;
}

/// Exercise the Star Fox adapter's captured formats, history lifecycle and same-frame fallback.
void StarFoxCases(const Device& device, bool super_resolution, bool exercise_gameplay = false) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    StarFoxDlss adapter{device, scheduler, allocator, pool, queue};
    Settings::values.nvidia_dlss_quality = super_resolution ? Settings::NvidiaDlssQuality::Automatic
                                                            : Settings::NvidiaDlssQuality::Native;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    Settings::RefreshNvidiaDlssRuntimeSettings();

    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    // Forward successful evaluations to the real GPU backend while observing its reset input.
    // The scheduler is drained before these test-only variables are read or changed.
    static decltype(api.evaluate) forward_evaluate;
    static bool inject_failure;
    static int observed_reset;
    static u32 evaluate_calls;
    static VkFormat observed_motion_format;
    forward_evaluate = evaluate;
    SCOPE_EXIT {
        api.evaluate = evaluate;
    };
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* parameters,
                      PFN_NVSDK_NGX_ProgressCallback progress) {
        ++evaluate_calls;
        parameters->Get(NVSDK_NGX_Parameter_Reset, &observed_reset);
        void* resource{};
        parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &resource);
        observed_motion_format =
            resource
                ? static_cast<const NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Format
                : VK_FORMAT_UNDEFINED;
        if (inject_failure) {
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        return forward_evaluate(cmd, feature, parameters, progress);
    };
    constexpr std::array<u8, 4> encoded{128, 64, 192, 255};
    constexpr std::array<float, 4> fallback{0.75f, 0.125f, 0.875f, 0.375f};
    constexpr std::array<float, 4> scene_fallback{0.0f, 1.0f, 0.0f, 0.4f};
    constexpr std::array<u8, 4> scene_fallback_bytes{0, 255, 0, 102};
    const auto linear = [](u8 byte) {
        const double value = byte / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto neural_before = VideoCore::NeuralRendering::GetStatus().submitted_frames;
    const u32 failure_frame = exercise_gameplay ? 8 : 6;
    for (u32 frame = 0; frame <= failure_frame; ++frame) {
        // A cinematic between two gameplay evaluations also exercises both mode transitions.
        const bool gameplay = exercise_gameplay && frame != 6;
        const auto mode =
            gameplay ? StarFoxDlss::InputMode::Gameplay : StarFoxDlss::InputMode::Cinematic;
        // Repeated frame, explicit reset, skipped evaluation, resize, guest reset, failure.
        if (frame == 2) {
            adapter.ResetHistory();
        }
        if (frame == 3) {
            adapter.TickFrame();
        }
        const VkExtent2D extent = frame < 4 ? VkExtent2D{320, 180} : VkExtent2D{384, 216};
        const VkExtent2D output_extent =
            super_resolution ? VkExtent2D{extent.width * 2, extent.height * 2} : extent;
        const VkExtent2D scene_extent =
            gameplay ? VkExtent2D{extent.width * 2, extent.height * 2} : output_extent;
        const bool writes_scene = gameplay || super_resolution;
        const size_t pixels = size_t{extent.width} * extent.height;
        const size_t scene_pixels = size_t{scene_extent.width} * scene_extent.height;
        auto color = MakeInput(device, allocator,
                               gameplay ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_ASPECT_COLOR_BIT, extent);
        auto depth =
            MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT,
                      extent, 0, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        auto motion = MakeInput(device, allocator,
                                gameplay ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SFLOAT,
                                VK_IMAGE_ASPECT_COLOR_BIT, extent);
        auto target =
            MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                      extent, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto scene =
            MakeInput(device, allocator, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                      scene_extent, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto scene_readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = scene_pixels * 4,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        const size_t color_pixel_bytes = gameplay ? 8 : 4;
        auto upload = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = pixels * color_pixel_bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Upload);
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            if (gameplay) {
                for (size_t channel = 0; channel < encoded.size(); ++channel) {
                    const _Float16 value = static_cast<_Float16>(encoded[channel] / 255.0f);
                    std::memcpy(upload.Mapped().data() + pixel * color_pixel_bytes + channel * 2,
                                &value, sizeof(value));
                }
            } else {
                std::memcpy(upload.Mapped().data() + pixel * color_pixel_bytes, encoded.data(),
                            encoded.size());
            }
        }
        upload.Flush();
        auto readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = pixels * 8,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        scheduler.Record([&](vk::CommandBuffer cmd) {
            for (const auto& image : {color.resource, depth.resource, motion.resource,
                                      target.resource, scene.resource}) {
                auto range = image.range;
                range.aspectMask = image.barrier_aspect;
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                    VkImageMemoryBarrier{
                                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = image.image,
                                        .subresourceRange = range,
                                    });
            }
            // Upload encoded bytes: a Vulkan sRGB clear would encode the clear value itself.
            cmd.CopyBufferToImage(
                *upload, *color.image, VK_IMAGE_LAYOUT_GENERAL,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            VkClearColorValue clear{};
            clear.float32[0] = gameplay ? 0.125f : 0.0f;
            clear.float32[1] = gameplay ? -0.125f : 0.0f;
            cmd.ClearColorImage(*motion.image, VK_IMAGE_LAYOUT_GENERAL, clear,
                                motion.resource.range);
            std::copy(fallback.begin(), fallback.end(), clear.float32);
            cmd.ClearColorImage(*target.image, VK_IMAGE_LAYOUT_GENERAL, clear,
                                target.resource.range);
            std::copy(scene_fallback.begin(), scene_fallback.end(), clear.float32);
            cmd.ClearColorImage(*scene.image, VK_IMAGE_LAYOUT_GENERAL, clear, scene.resource.range);
            const VkClearDepthStencilValue clear_depth{0.5f, 0};
            auto depth_range = depth.resource.range;
            depth_range.aspectMask = depth.resource.barrier_aspect;
            const auto clear_fn = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
                device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                               "vkCmdClearDepthStencilImage"));
            clear_fn(*cmd, *depth.image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1, &depth_range);
        });
        inject_failure = frame == failure_frame;
        observed_reset = -1;
        observed_motion_format = VK_FORMAT_UNDEFINED;
        evaluate_calls = 0;
        const auto before = NvidiaDlssUpscaler::Status().evaluations;
        const NvidiaDlssUpscaler::Inputs inputs{color.resource,
                                                depth.resource,
                                                motion.resource,
                                                extent,
                                                output_extent,
                                                {0.125f, 0.2777778f},
                                                {-0.5f * extent.width, 0.5f * extent.height},
                                                frame == 5,
                                                true,
                                                true};
        const auto lowres_target = gameplay ? VK_NULL_HANDLE : *target.view;
        Require(adapter.Record(inputs, lowres_target, mode),
                "Star Fox adapter refused a valid frame");
        Require(!adapter.WriteUpscaledOutput(*scene.view,
                                             {scene_extent.width + 8, scene_extent.height}),
                "Star Fox adapter accepted a mismatched scene extent");
        const bool scene_queued = adapter.WriteUpscaledOutput(*scene.view, scene_extent);
        if (!inject_failure) {
            Require(scene_queued == writes_scene,
                    "Star Fox scene output did not follow the native/SR extent contract");
        }
        // Failure may already have reached the worker here. Whether refused immediately or
        // queued and skipped there, the current native scene must remain intact.

        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                *target.image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            cmd.CopyImageToBuffer(
                *scene.image, VK_IMAGE_LAYOUT_GENERAL, *scene_readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {scene_extent.width, scene_extent.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        Require(evaluate_calls == 1, "Star Fox adapter did not invoke NGX exactly once");
        Require(NvidiaDlssUpscaler::Status().evaluations == before + !inject_failure,
                "Star Fox adapter failed a real evaluation or counted an injected failure");
        Require(observed_motion_format == VK_FORMAT_R16G16_SFLOAT,
                "Star Fox motion did not reach NGX as RG16F");
        if (frame == 0 || (frame >= 2 && frame <= 5) ||
            (exercise_gameplay && frame >= 6 && frame <= 7)) {
            Require(observed_reset == 1,
                    "Star Fox adapter lost a lifecycle or guest history reset");
        }
        // Slow first-time shader compilation may trigger the adapter's wall-clock reset on
        // an otherwise consecutive frame; do not assume that evaluation finishes within 250 ms.
        Require(observed_reset == 0 || observed_reset == 1, "NGX did not receive a reset value");
        readback.Invalidate();
        std::array<double, 3> sums{};
        const auto bytes = readback.Mapped();
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            for (size_t channel = 0; channel < 4; ++channel) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + (pixel * 4 + channel) * 2, sizeof(half));
                const float value = static_cast<float>(half);
                Require(std::isfinite(value), "Star Fox adapter produced a non-finite component");
                if (channel == 3 || inject_failure || gameplay) {
                    Require(value == fallback[channel],
                            "Star Fox adapter changed alpha or overwrote same-frame fallback");
                }
                if (channel < 3) {
                    sums[channel] += value;
                }
            }
        }
        if (!inject_failure && !gameplay) {
            for (size_t channel = 0; channel < sums.size(); ++channel) {
                Require(std::abs(sums[channel] / pixels - linear(encoded[channel])) < 0.025,
                        "Star Fox sRGB input was not decoded to linear DLSS output");
            }
        } else if (inject_failure) {
            Require(!adapter.Record(inputs, lowres_target, mode),
                    "Failed Star Fox adapter did not retain spatial fallback on following frames");
        }
        scene_readback.Invalidate();
        const auto scene_bytes = scene_readback.Mapped();
        std::array<double, 3> scene_sums{};
        for (size_t pixel = 0; pixel < scene_pixels; ++pixel) {
            for (size_t channel = 0; channel < 4; ++channel) {
                const auto value = scene_bytes[pixel * 4 + channel];
                if (!writes_scene || inject_failure) {
                    Require(value == scene_fallback_bytes[channel],
                            "Star Fox scene fallback RGB/alpha was overwritten");
                } else if (channel == 3) {
                    Require(value == 255, "Star Fox scene output did not write opaque alpha");
                }
                if (channel < 3) {
                    scene_sums[channel] += value;
                }
            }
        }
        if (writes_scene && !inject_failure) {
            for (size_t channel = 0; channel < scene_sums.size(); ++channel) {
                Require(std::abs(scene_sums[channel] / scene_pixels - encoded[channel]) < 3.0,
                        "Star Fox scene output did not apply exactly one sRGB encoding");
            }
        }
        Require(VideoCore::NeuralRendering::GetStatus().submitted_frames == neural_before,
                "Star Fox adapter smoke unexpectedly enabled neural rendering");
        adapter.TickFrame();
        Require(!adapter.WriteUpscaledOutput(*scene.view, scene_extent),
                "Star Fox adapter accepted a stale scene from the previous frame");
        queue.TickFrame();
        std::printf(
            "PASS Star Fox %s frame %u: %ux%u -> %ux%u, reset=%d, fallback=%d; RGB %.4f %.4f %.4f; "
            "alpha preserved\n",
            gameplay ? "gameplay" : "cinematic", frame, extent.width, extent.height,
            output_extent.width, output_extent.height, observed_reset, inject_failure,
            sums[0] / pixels, sums[1] / pixels, sums[2] / pixels);
    }
}

/// Exercise camera reprojection and writeback through the real scene adapter and NGX backend.
void SceneCases(const Device& device, bool super_resolution, bool dynamic_crop = false,
                bool return_to_native = false,
                VkFormat scene_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                bool prepared = false, bool native_transfer = false) {
    const bool rgba8 = scene_format == VK_FORMAT_R8G8B8A8_UNORM;
    const bool rgba16f = scene_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const size_t pixel_bytes = rgba16f ? 8 : 4;
    const bool unorm = rgba8 || scene_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    SceneUpscaler adapter{device, scheduler, allocator, pool, queue};
    Settings::values.nvidia_dlss_quality = super_resolution ? Settings::NvidiaDlssQuality::Automatic
                                                            : Settings::NvidiaDlssQuality::Native;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    if (dynamic_crop)
        Settings::values.nvidia_dlss_quality = Settings::NvidiaDlssQuality::Performance;
    Settings::RefreshNvidiaDlssRuntimeSettings();
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    const auto create = api.create;
    SCOPE_EXIT {
        api.create = create;
    };
    static decltype(api.create) forward_create;
    static u32 creates;
    static VkImage color_image, depth_image, output_image;
    static VkExtent2D actual_extent;
    forward_create = create;
    creates = 0;
    api.create = [](VkDevice device, VkCommandBuffer commands, NVSDK_NGX_Feature feature,
                    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
        ++creates;
        return forward_create(device, commands, feature, parameters, handle);
    };
    VkExtent2D large{}, small{};
    if (dynamic_crop) {
        NVSDK_NGX_Parameter* caps{};
        Require(NVSDK_NGX_SUCCEED(api.capabilities(&caps)) && caps,
                "Dynamic smoke capability query failed");
        SCOPE_EXIT {
            api.destroy_parameters(caps);
        };
        void* callback{};
        Require(NVSDK_NGX_SUCCEED(
                    caps->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback)) &&
                    callback,
                "Dynamic smoke optimal settings callback missing");
        caps->Set(NVSDK_NGX_Parameter_Width, 640u);
        caps->Set(NVSDK_NGX_Parameter_Height, 360u);
        caps->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(NVSDK_NGX_PerfQuality_Value_MaxPerf));
        caps->Set(NVSDK_NGX_Parameter_RTXValue, 0);
        Require(NVSDK_NGX_SUCCEED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(
                    callback)(caps)),
                "Dynamic smoke optimal settings query failed");
        VkExtent2D minimum{}, maximum{}, optimal{};
        caps->Get(NVSDK_NGX_Parameter_OutWidth, &optimal.width);
        caps->Get(NVSDK_NGX_Parameter_OutHeight, &optimal.height);
        caps->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &minimum.width);
        caps->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &minimum.height);
        caps->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &maximum.width);
        caps->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &maximum.height);
        Require(maximum.width > minimum.width && maximum.height > minimum.height,
                "Runtime has no dynamic Performance range to exercise");
        large = {std::min(maximum.width, optimal.width + 16),
                 std::min(maximum.height, optimal.height + 9)};
        small = {std::max(minimum.width, large.width - 8),
                 std::max(minimum.height, large.height - 5)};
        std::printf("Performance dynamic query: optimal=%ux%u min=%ux%u max=%ux%u\n",
                    optimal.width, optimal.height, minimum.width, minimum.height,
                    maximum.width, maximum.height);
    }
    static decltype(api.evaluate) forward;
    static bool fail;
    static int reset;
    static VkImage motion_image;
    static VkFormat motion_format;
    static float motion_scale_x, motion_scale_y;
    forward = evaluate;
    SCOPE_EXIT {
        api.evaluate = evaluate;
    };
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* parameters,
                      PFN_NVSDK_NGX_ProgressCallback progress) {
        parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
        void* resource{};
        parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &resource);
        const auto* motion = static_cast<const NVSDK_NGX_Resource_VK*>(resource);
        motion_image = motion->Resource.ImageViewInfo.Image;
        motion_format = motion->Resource.ImageViewInfo.Format;
        parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &motion_scale_x);
        parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &motion_scale_y);
        actual_extent = {motion->Resource.ImageViewInfo.Width,
                         motion->Resource.ImageViewInfo.Height};
        parameters->Get(NVSDK_NGX_Parameter_Color, &resource);
        color_image =
            static_cast<const NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        parameters->Get(NVSDK_NGX_Parameter_Depth, &resource);
        depth_image =
            static_cast<const NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        parameters->Get(NVSDK_NGX_Parameter_Output, &resource);
        output_image =
            static_cast<const NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        return fail ? NVSDK_NGX_Result_FAIL_FeatureNotSupported
                    : forward(cmd, feature, parameters, progress);
    };
    const auto decode = [](u32 bits, u32 mantissa_bits) {
        const u32 exponent = bits >> mantissa_bits;
        const u32 fraction = bits & ((1U << mantissa_bits) - 1);
        return std::ldexp(exponent ? 1.f + float(fraction) / (1U << mantissa_bits)
                                   : float(fraction) / (1U << mantissa_bits),
                          exponent ? int(exponent) - 15 : -14);
    };
    const std::array<float, 3> normal_source = !native_transfer
        ? (rgba16f ? std::array{2.5f, 0.5f, 1.125f} : std::array{0.25f, 0.5f, 0.125f})
        : super_resolution ? std::array{0.f, 3.f, 8.f} : std::array{2.75f, 3.f, 4.f};
    // Independent golden values from native 0b7390fa8600a81c and captured CB3:
    // below the shoulder, at its threshold, black, and two highlight levels.
    const std::array<float, 3> normal_expected_color = !native_transfer ? normal_source
        : super_resolution ? std::array{0.f, 0.7499999793f, 0.9983155130f}
                           : std::array{0.6875f, 0.7499999793f, 0.9080301309f};
    constexpr SceneUpscaler::OutputTransform normal_curve{
        {0.25f, 0.f, 0.150000006f, 1.f},
        {0.75f, -5.7707800865f, 2.3280851841f, 1.f},
    };
    constexpr std::array<float, 3> fallback{0.75f, 0.125f, 0.875f};
    bool checked_translation = false;
    u64 frame = 1;
    float previous_x = 0;
    std::array<VkImage, 4> persistent_images{};
    for (u32 step = 0; step < (return_to_native ? 3u : 10u); ++step, ++frame) {
        const auto source = native_transfer && step == 8 ? std::array{0.f, 0.f, 0.f}
            : native_transfer && step == 6 ? std::array{.05f, .1f, .15f} : normal_source;
        const auto expected_color = native_transfer && step == 8 ? std::array{0.f, 0.f, 0.f}
            : native_transfer && step == 6 ? std::array{.0375f, .075f, .1125f}
            : native_transfer && step == 7
                ? (super_resolution ? std::array{0.f, .8203577330f, .9993022225f}
                                    : std::array{.7557490374f, .8144445083f, .9381949552f})
                : normal_expected_color;
        auto native_curve = normal_curve;
        if (native_transfer && step >= 6 && step <= 8) {
            native_curve.luminance[1] = .5f; // Saturated/unsaturated luminance weight, then black.
            adapter.ResetHistory();
        }
        if (step == 3)
            frame += 2; // Missing guest frames invalidate history.
        if (step == 4)
            adapter.ResetHistory(); // The title calls this on an enabled toggle.
        const VkExtent2D extent =
            return_to_native ? (step == 1 ? VkExtent2D{320, 180} : VkExtent2D{640, 360})
            : dynamic_crop   ? (step % 2 ? small : large)
                             : (step < 5 ? VkExtent2D{320, 180} : VkExtent2D{384, 216});
        const VkExtent2D output =
            (dynamic_crop || return_to_native)
                ? VkExtent2D{640, 360}
                : (super_resolution ? VkExtent2D{extent.width * 2, extent.height * 2} : extent);
        if (dynamic_crop && step == 7) {
            Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::K;
            Settings::RefreshNvidiaDlssRuntimeSettings();
        }
        const auto pixels = size_t{extent.width} * extent.height;
        const auto output_pixels = size_t{output.width} * output.height;

        auto color = MakeInput(device, allocator, scene_format,
                               VK_IMAGE_ASPECT_COLOR_BIT, {extent.width + 1, extent.height});
        auto depth =
            MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, extent);
        auto target = MakeInput(device, allocator, scene_format,
                                VK_IMAGE_ASPECT_COLOR_BIT, output,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // A caller-owned padded motion image must reach NGX unchanged, even on reset frames.
        const VkExtent2D supplied_extent{extent.width + 7, extent.height + 3};
        auto supplied_motion = prepared
            ? MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                        supplied_extent, VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            : InputImage{};
        supplied_motion.resource.extent = supplied_extent;
        auto padding = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                               .size = (size_t{extent.height} +
                                                        ((unorm || rgba16f) ? output_pixels : 0)) * pixel_bytes,
                                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                              MemoryUsage::Upload);
        for (u32 row = 0; row < extent.height; ++row) {
            const u32 poison = rgba8 ? 255u : unorm ? 1023u : 28u << 6; // Red in the unused final column.
            if (rgba16f) {
                const std::array<_Float16, 4> poison_hdr{8192, 0, 0, 1};
                std::memcpy(padding.Mapped().data() + row * pixel_bytes, poison_hdr.data(), 8);
            } else {
                std::memcpy(padding.Mapped().data() + row * pixel_bytes, &poison, sizeof(poison));
            }
        }
        u32 fallback_rgb{};
        if (unorm) {
            for (u32 channel = 0; channel < 3; ++channel)
                fallback_rgb |= u32(std::round(fallback[channel] * (rgba8 ? 255 : 1023)))
                                << (channel * (rgba8 ? 8 : 10));
            for (size_t pixel = 0; pixel < output_pixels; ++pixel) {
                const u32 packed = fallback_rgb | (rgba8 ? u32(pixel % 256) << 24
                                                        : u32(pixel % 4) << 30);
                std::memcpy(padding.Mapped().data() + (extent.height + pixel) * sizeof(u32),
                            &packed, sizeof(packed));
            }
        }
        if (rgba16f) {
            for (size_t pixel = 0; pixel < output_pixels; ++pixel) {
                const std::array<_Float16, 4> value{fallback[0], fallback[1], fallback[2],
                    static_cast<_Float16>(float(int(pixel % 65) - 32) / 8)};
                std::memcpy(padding.Mapped().data() + (extent.height + pixel) * pixel_bytes,
                            value.data(), 8);
            }
        }
        padding.Flush();
        auto readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = output_pixels * pixel_bytes + pixels * 4,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        scheduler.Record([&](vk::CommandBuffer cmd) {
            for (const auto& image : {color.resource, depth.resource, target.resource,
                                      supplied_motion.resource}) {
                if (!image.image) continue;
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                    VkImageMemoryBarrier{
                                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = image.image,
                                        .subresourceRange = image.range,
                                    });
            }
            VkClearColorValue clear{};
            if (prepared) {
                clear.float32[0] = 0.375f;
                clear.float32[1] = -0.625f;
                cmd.ClearColorImage(*supplied_motion.image, VK_IMAGE_LAYOUT_GENERAL, clear,
                                    supplied_motion.resource.range);
            }
            std::copy(source.begin(), source.end(), clear.float32);
            cmd.ClearColorImage(*color.image, VK_IMAGE_LAYOUT_GENERAL, clear, color.resource.range);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_TRANSFER_WRITE_BIT});
            cmd.CopyBufferToImage(
                *padding, *color.image, VK_IMAGE_LAYOUT_GENERAL,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageOffset = {static_cast<s32>(extent.width), 0, 0},
                                  .imageExtent = {1, extent.height, 1}});
            std::copy(fallback.begin(), fallback.end(), clear.float32);
            if (unorm || rgba16f) {
                cmd.CopyBufferToImage(
                    *padding, *target.image, VK_IMAGE_LAYOUT_GENERAL,
                    VkBufferImageCopy{.bufferOffset = size_t{extent.height} * pixel_bytes,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {output.width, output.height, 1}});
            } else {
                cmd.ClearColorImage(*target.image, VK_IMAGE_LAYOUT_GENERAL, clear,
                                    target.resource.range);
            }
            const VkClearDepthStencilValue clear_depth{0.5f, 0};
            const auto clear_fn = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
                device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                               "vkCmdClearDepthStencilImage"));
            clear_fn(*cmd, *depth.image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1,
                     &depth.resource.range);
        });
        // Perspective camera looking down -Z, translated right by 0.02 world units
        // each frame. At device depth 0.5, the surface lies at distance B/A. Its
        // motion under a rightward camera translation must point right toward the
        // prior frame, scaled by focal length.
        const float x = step >= 8 ? 60.f + float(step - 8) * 0.02f : float(step) * 0.02f;
        const float focal = step < 6 ? 1.f : 1.2f;
        constexpr float a = -1001.f / 999.f;
        constexpr float b = -2000.f / 999.f;
        SceneUpscaler::Camera camera{
            .view_projection = {focal, 0, 0, -focal * x, 0, 1, 0, 0, 0, 0, a, b, 0, 0, -1, 0},
            .projection = {focal, 0, 0, 0, 0, 1, 0, 0, 0, 0, a, b, 0, 0, -1, 0},
            .view = {1, 0, 0, -x, 0, 1, 0, 0, 0, 0, 1, 0},
            .pixel_to_clip = {2, -2, -1, 1},
            .depth_to_clip = {2, -1},
        };
        NvidiaDlssUpscaler::Inputs inputs{};
        inputs.color = color.resource;
        inputs.depth = depth.resource;
        inputs.render_size = extent;
        inputs.upscale_size = output;
        inputs.dynamic_resolution = dynamic_crop || return_to_native;
        inputs.depth_inverted = false;
        inputs.hdr = !rgba8;
        inputs.motion_vectors = supplied_motion.resource;
        inputs.reset = prepared && (step == 6 || step == 8); // Title-reported camera cuts.
        const auto record = [&](NvidiaDlssUpscaler::Inputs in, u64 at_frame) {
            return prepared ? adapter.RecordWithMotion(in, *target.view, at_frame, scene_format,
                                                        native_transfer ? &native_curve : nullptr)
                            : adapter.Record(in, *target.view, camera, at_frame, scene_format);
        };
        fail = step == 9;
        reset = -1;
        motion_image = VK_NULL_HANDLE;
        const auto before = NvidiaDlssUpscaler::Status().evaluations;
        if (step == 0 && prepared) {
            auto bad = inputs;
            bad.motion_vectors.view = VK_NULL_HANDLE;
            Require(!record(bad, frame), "Prepared scene accepted missing motion");
            bad = inputs;
            bad.motion_vectors.format = VK_FORMAT_R16G16B16A16_UNORM;
            Require(!record(bad, frame), "Prepared scene accepted undecoded motion");
            bad = inputs;
            bad.motion_vectors.extent = {extent.width - 1, extent.height};
            Require(!record(bad, frame), "Prepared scene accepted undersized motion");
            if (rgba16f) {
                Require(!adapter.RecordWithMotion(inputs, *target.view, frame, scene_format,
                                                   &normal_curve),
                        "HDR scene accepted an unvalidated output transform");
            }
            if (native_transfer) {
                auto invalid = native_curve;
                invalid.shoulder[1] = std::numeric_limits<float>::quiet_NaN();
                Require(!adapter.RecordWithMotion(inputs, *target.view, frame, scene_format,
                                                   &invalid),
                        "Scene accepted a non-finite output transfer");
                Require(!adapter.RecordWithMotion(inputs, *target.view, frame,
                                                   VK_FORMAT_R8G8B8A8_UNORM, &native_curve),
                        "Scene accepted a transfer for an unvalidated target format");
            }
        }
        if (step == 0 && !prepared) {
            auto singular = camera;
            singular.view_projection = {};
            Require(!adapter.Record(inputs, *target.view, singular, frame, scene_format),
                    "Scene adapter accepted a singular camera matrix");
        }
        Require(record(inputs, frame),
                "Scene adapter refused valid inputs");
        Require(!record(inputs, frame),
                "Scene adapter accepted duplicate frame");
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                *target.image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {output.width, output.height, 1}});
            if (motion_image) {
                cmd.CopyImageToBuffer(
                    motion_image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                    VkBufferImageCopy{.bufferOffset = output_pixels * pixel_bytes,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {extent.width, extent.height, 1}});
            }
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        Require(motion_image && motion_format == VK_FORMAT_R16G16_SFLOAT,
                "Scene camera motion did not reach NGX as RG16F");
        Require(NvidiaDlssUpscaler::Status().evaluations == before + !fail,
                "Scene adapter did not complete a real NGX evaluation");
        Require(reset == 0 || reset == 1, "Scene reset input missing");
        if (prepared) {
            Require(motion_image == *supplied_motion.image &&
                        actual_extent.width == supplied_extent.width &&
                        actual_extent.height == supplied_extent.height &&
                        motion_scale_x == 1.f && motion_scale_y == 1.f,
                    "Prepared motion resource, capacity or pixel scale was changed");
        }
        if (step == 0 || step == 3 || step == 4 || (!dynamic_crop && step == 5) || step == 6 ||
            (dynamic_crop && step == 7) || step == 8)
            Require(reset == 1, "Scene gap/toggle/resize/projection reset was lost");
        if (dynamic_crop || return_to_native) {
            const std::array images{color_image, depth_image, motion_image, output_image};
            if (step == 0)
                persistent_images = images;
            Require(images == persistent_images,
                    "Dynamic crop recreated Scene scratch or NGX output images");
            const VkExtent2D capacity = return_to_native ? VkExtent2D{640, 360} : large;
            Require(actual_extent.width == capacity.width &&
                        actual_extent.height == capacity.height,
                    "NGX resource metadata confused active crop with image allocation");
            Require(creates == (return_to_native ? step + 1 : (step < 7 ? 1u : 2u)),
                    "Dynamic crop recreated NGX feature or options did not recreate it");
            if (return_to_native) {
                Require(reset == 1, "Out-of-range or requested-mode return did not reset history");
                Require(NvidiaDlssUpscaler::Status().mode ==
                            (step == 1 ? Settings::NvidiaDlssQuality::Performance
                                       : Settings::NvidiaDlssQuality::Native),
                        "Dynamic fallback did not return to the requested Native mode");
            } else if (step == 1 || step == 2 || step == 5)
                Require(reset == 0, "Dynamic crop unnecessarily reset temporal history");
        }
        readback.Invalidate();
        const auto bytes = readback.Mapped();
        std::array<double, 3> sums{};
        for (size_t pixel = 0; pixel < output_pixels; ++pixel) {
            u32 packed;
            std::memcpy(&packed, bytes.data() + pixel * pixel_bytes, 4);
            std::array<_Float16, 4> hdr{};
            if (rgba16f) std::memcpy(hdr.data(), bytes.data() + pixel * pixel_bytes, 8);
            const auto values = rgba16f ? std::array{float(hdr[0]), float(hdr[1]), float(hdr[2])} : rgba8
                ? std::array{float(packed & 255) / 255, float((packed >> 8) & 255) / 255,
                             float((packed >> 16) & 255) / 255}
                : unorm
                ? std::array{float(packed & 1023) / 1023, float((packed >> 10) & 1023) / 1023,
                             float((packed >> 20) & 1023) / 1023}
                : std::array{decode(packed & 0x7ff, 6), decode((packed >> 11) & 0x7ff, 6),
                             decode(packed >> 22, 5)};
            if (rgba16f) {
                Require(float(hdr[3]) == float(int(pixel % 65) - 32) / 8,
                        "HDR scene writeback changed native signed or above-one alpha");
            }
            if (unorm) {
                Require((rgba8 ? packed >> 24 : packed >> 30) == pixel % (rgba8 ? 256 : 4),
                        "Scene writeback changed native alpha");
                if (fail)
                    Require((packed & (rgba8 ? 0xffffff : 0x3fffffff)) == fallback_rgb,
                            "Failed UNORM scene evaluation changed native fallback bits");
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                Require(std::isfinite(values[channel]), "Scene output contains non-finite color");
                if (fail && !unorm)
                    Require(values[channel] == fallback[channel],
                            "Failed scene evaluation overwrote native fallback");
                sums[channel] += values[channel];
            }
        }
        if (!fail)
            for (size_t channel = 0; channel < 3; ++channel)
                Require(std::abs(sums[channel] / output_pixels - expected_color[channel]) < 0.025,
                        "Scene output differs from its linear color or native transfer");
        // A backend feature reset can coexist with valid camera motion (DLSS
        // ignores it on that frame). Scene only zeros motion when its own camera
        // history was invalidated.
        const bool camera_reset =
            reset && !(dynamic_crop && step == 7) && !(return_to_native && step > 0);
        const float expected_x =
            prepared ? 0.375f
            : camera_reset ? 0.f : (x - previous_x) * focal * (a / b) * extent.width / 2;
        for (size_t pixel = 0; pixel < pixels; pixel += 17) {
            std::array<_Float16, 2> motion;
            std::memcpy(motion.data(), bytes.data() + output_pixels * pixel_bytes + pixel * 4, 4);
            Require(std::abs(float(motion[0]) - expected_x) < 0.005f &&
                        std::abs(float(motion[1]) - (prepared ? -0.625f : 0.f)) < 0.005f,
                    "Scene camera reprojection has wrong depth, sign, scale, or reset");
        }
        checked_translation |= !reset;
        if (fail)
            Require(!record(inputs, frame + 1),
                    "Failed scene adapter accepted a subsequent frame");
        previous_x = x;
        queue.TickFrame();
        std::printf("PASS scene %s %s step%u: %ux%u -> %ux%u reset=%d failure=%d "
                    "motionX=%.5f\n",
                    native_transfer ? "R11G11B10-transfer-prepared"
                        : rgba16f ? "RGBA16F-prepared" : rgba8 ? "RGBA8-prepared" : unorm ? "RGB10A2" : "R11G11B10",
                    return_to_native ? "mode-return"
                    : dynamic_crop   ? "dynamic"
                                     : (super_resolution ? "SR" : "native"),
                    step, extent.width, extent.height, output.width, output.height, reset, fail,
                    expected_x);
    }
    Require(checked_translation || return_to_native,
            "Scene smoke never exercised non-reset camera translation");
}

/// Check live neural controls, quality, history resets, rebinding and failures with concurrent submits.
void RenderCases(const Device& device, u32 scale, bool request_neural, bool expect_neural,
                 bool fail_creation) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    // Simulate presentation submissions during backend setup, resizing and teardown. Declaring
    // this before the upscaler keeps it running through upscaler destruction; use Vulkan validation
    // to detect queue calls that are not protected by the scheduler's submission lock.
    std::jthread present_thread{[&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                std::scoped_lock queue_lock{scheduler.submit_mutex};
                Require(device.GetGraphicsQueue().Submit(
                            VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO}) == VK_SUCCESS,
                        "Concurrent queue submission failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};
    NvidiaDlssUpscaler upscaler{device, scheduler, allocator};
    const VkExtent2D input_extent{320 * scale, 180 * scale};
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    auto depth =
        MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, input_extent);
    auto motion = MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                            input_extent);
    auto alternate_motion_view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *motion.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = motion.resource.format,
        .subresourceRange = motion.resource.range,
    });
    const auto original_motion_view = motion.resource.view;
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto create = api.create;
    const auto evaluate = api.evaluate;
    SCOPE_EXIT {
        api.create = create;
        api.evaluate = evaluate;
    };
    static decltype(api.create) forward_static_create;
    static VkExtent2D static_creation_extent;
    forward_static_create = create;
    static_creation_extent = {};
    api.create = [](VkDevice device, VkCommandBuffer commands, NVSDK_NGX_Feature feature,
                    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
        parameters->Get(NVSDK_NGX_Parameter_Width, &static_creation_extent.width);
        parameters->Get(NVSDK_NGX_Parameter_Height, &static_creation_extent.height);
        return forward_static_create(device, commands, feature, parameters, handle);
    };
    if (fail_creation) {
        api.create = [](VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                        NVSDK_NGX_Handle** handle) {
            *handle = nullptr;
            return NVSDK_NGX_Result_FAIL_PlatformError;
        };
    }
    using Quality = Settings::NvidiaDlssQuality;
    using Neural = Settings::NeuralRendering;
    constexpr std::array cases{
        // Match Ocarina's cold start; scale 8 exercises Native/M/Natural at 2560x1440.
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Natural, 100, 100, 100, true,
                                            Settings::NvidiaDlssPreset::M},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Standard},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Cinematic},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Cinematic},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Off},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Off},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 50},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 100, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 100, 100, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Quality, Neural::Off, 100, 100, 100, true,
                                            Settings::NvidiaDlssPreset::K},
        Settings::NvidiaDlssRuntimeSettings{Quality::Balanced, Neural::Off, 100, 100, 100, true,
                                            Settings::NvidiaDlssPreset::M},
        Settings::NvidiaDlssRuntimeSettings{Quality::Performance, Neural::Off, 100, 100, 100, true,
                                            Settings::NvidiaDlssPreset::L},
        Settings::NvidiaDlssRuntimeSettings{Quality::UltraPerformance, Neural::Off, 100, 100, 100,
                                            true, Settings::NvidiaDlssPreset::J},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Off, 100, 100, 100, true,
                                            Settings::NvidiaDlssPreset::NvidiaDefault},
        // Captured MKW pre-exposure sand: reference-white 4 -> 1 -> 4, warm, explicit reset.
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        // Native variable exposure versus independently exposed input, plus history/reset.
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural}};
    u64 neural_frames{};
    std::array<double, 3> natural_reset{}, reference_white_reset{}, exposure_reset{};
    std::vector<std::array<float, 3>> exposure_pair;
    const std::array<double, 3> exposure_weights{.2126, .7152, .0722};
    const auto expose = [&](std::array<float, 4> color) {
        double y{};
        for (size_t c = 0; c < 3; ++c)
            y += color[c] * exposure_weights[c];
        const auto scale = .25 + .5 * (y > 0 ? std::min(.15 / y, 1.) : 1.);
        for (size_t c = 0; c < 3; ++c)
            color[c] *= scale;
        return color;
    };
    for (size_t frame = 0; frame < cases.size(); ++frame) {
        Settings::values.nvidia_dlss_quality = cases[frame].quality;
        Settings::values.nvidia_dlss_preset = cases[frame].preset;
        Settings::values.neural_rendering =
            request_neural ? cases[frame].neural_rendering : Neural::Off;
        Settings::values.neural_rendering_strength = cases[frame].strength;
        Settings::values.neural_rendering_tone = cases[frame].tone;
        Settings::values.neural_rendering_structure = cases[frame].structure;
        Settings::RefreshNvidiaDlssRuntimeSettings();
        const auto options = Settings::GetNvidiaDlssRuntimeSettings();
        const bool neural_on = expect_neural && options.neural_rendering != Neural::Off;
        // Rebind while temporal history is live, without resizing the backend.
        motion.resource.view = frame == 1 ? *alternate_motion_view : original_motion_view;
        const VkExtent2D extent = options.quality == Quality::Native
                                      ? input_extent
                                      : VkExtent2D{480 * scale, 270 * scale};
        const bool inject_failure = frame == cases.size() - 1;
        if (inject_failure) {
            api.evaluate = [](VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                              PFN_NVSDK_NGX_ProgressCallback) {
                return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
            };
        }
        const bool white_case = frame >= 24 && frame <= 28;
        const bool exposure_case = frame >= 29 && frame <= 38;
        const bool exposure_enabled = exposure_case && frame != 30 && frame != 32 && frame != 34;
        const bool hdr = frame == 5 || frame == 10 || frame == 14 || white_case || exposure_case;
        std::array<float, 4> expected = white_case ? std::array{2.09375f, 1.140625f, 0.4375f, 1.0f}
                                        : frame == 14 ? std::array{2.0f, 0.25f, 0.5f, 1.0f}
                                        : hdr         ? std::array{0.75f, 0.25f, 0.5f, 1.0f}
                                                      : std::array{0.25f, 0.5f, 0.75f, 1.0f};
        if (exposure_case) {
            expected = frame == 31 || frame == 32   ? std::array{2.09375f, 1.140625f, .4375f, 1.f}
                       : frame == 33 || frame == 34 ? std::array{0.f, 0.f, 0.f, 1.f}
                                                    : std::array{.15625f, .15625f, .15625f, 1.f};
            if (!exposure_enabled)
                expected = expose(expected);
        }
        if (white_case || exposure_case) {
            // Isolate NR history from NGX's own temporal convergence/reset. Still evaluate real
            // NGX, then supply the identical captured HDR pixel to the production NR backend.
            static decltype(api.evaluate) forward_white;
            static const vk::DeviceDispatch* white_dispatch;
            static std::array<float, 4> white_color;
            forward_white = evaluate;
            white_dispatch = &device.GetDispatchLoader();
            white_color = expected;
            api.evaluate = [](VkCommandBuffer commands, const NVSDK_NGX_Handle* feature,
                              const NVSDK_NGX_Parameter* parameters,
                              PFN_NVSDK_NGX_ProgressCallback progress) {
                const auto result = forward_white(commands, feature, parameters, progress);
                if (NVSDK_NGX_FAILED(result))
                    return result;
                void* resource{};
                parameters->Get(NVSDK_NGX_Parameter_Output, &resource);
                const auto image = static_cast<const NVSDK_NGX_Resource_VK*>(resource)
                                       ->Resource.ImageViewInfo.Image;
                vk::CommandBuffer cmd{commands, *white_dispatch};
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT});
                VkClearColorValue clear{};
                std::copy(white_color.begin(), white_color.end(), clear.float32);
                cmd.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, clear,
                                    VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                    VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT});
                return result;
            };
        }
        scheduler.Record([&, frame, expected](vk::CommandBuffer cmd) {
            for (const auto& image : {color.resource, depth.resource, motion.resource}) {
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = frame == 0 ? 0u : VK_ACCESS_MEMORY_READ_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout =
                            frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.image,
                        .subresourceRange = image.range,
                    });
            }
            VkClearColorValue clear{};
            std::copy(expected.begin(), expected.end(), clear.float32);
            cmd.ClearColorImage(*color.image, VK_IMAGE_LAYOUT_GENERAL, clear, color.resource.range);
            cmd.ClearColorImage(*motion.image, VK_IMAGE_LAYOUT_GENERAL, {}, motion.resource.range);
            const VkClearDepthStencilValue clear_depth{0.5f, 0};
            const auto clear_fn = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
                device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                               "vkCmdClearDepthStencilImage"));
            clear_fn(*cmd, *depth.image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1,
                     &depth.resource.range);
        });
        const auto before_status = NvidiaDlssUpscaler::Status();
        NvidiaDlssUpscaler::Inputs inputs{color.resource,
                                          depth.resource,
                                          motion.resource,
                                          input_extent,
                                          extent,
                                          {0, 0},
                                          {1, 1},
                                          frame == 0 || frame == 28 || frame == 38,
                                          frame != 0 && !hdr,
                                          hdr};
        inputs.neural_reference_white = white_case && frame != 25 ? 4.0f : 1.0f;
        if (exposure_enabled)
            inputs.neural_reference_exposure =
                frame == 35 ? std::array{.5f, .25f, .15f} : std::array{.25f, .5f, .15f};
        if (frame == 0) {
            for (const float invalid : {0.f, -1.f, 0.01f, std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::quiet_NaN()}) {
                auto rejected = inputs;
                rejected.neural_reference_white = invalid;
                Require(!upscaler.Upscale(rejected) && !upscaler.Failed(),
                        "Invalid NR reference white was accepted or poisoned the upscaler");
            }
        }
        if (frame == 29) {
            for (const auto invalid : {std::array{0.f, 1.f, .15f}, std::array{.25f, -.5f, .15f},
                                       std::array{.25f, .5f, -1.f}}) {
                auto rejected = inputs;
                rejected.neural_reference_exposure = invalid;
                Require(!upscaler.Upscale(rejected) && !upscaler.Failed(),
                        "Invalid native exposure was accepted or poisoned the upscaler");
            }
            auto rejected = inputs;
            rejected.hdr = false;
            Require(!upscaler.Upscale(rejected) && !upscaler.Failed(),
                    "Native HDR exposure was accepted for LDR input");
        }
        Require(upscaler.Upscale(inputs), "Upscale refused a frame");
        auto readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = VkDeviceSize{extent.width} * extent.height * 8,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        scheduler.Record([&, extent](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                upscaler.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        api.evaluate = evaluate;
        if (frame == 0 && request_neural && !expect_neural && !fail_creation) {
            Require(!VideoCore::NeuralRendering::GetStatus().error.empty(),
                    "Missing neural model was not diagnosed");
        }
        if (fail_creation) {
            Require(upscaler.Failed() && !Ngx::GetStatus().error.empty(),
                    "Initial feature creation failure was not diagnosed");
            Require(NvidiaDlssUpscaler::Status().evaluations == before_status.evaluations,
                    "Failed creation counted a DLSS evaluation");
            const auto neural = VideoCore::NeuralRendering::GetStatus();
            Require(!neural.ready && neural.submitted_frames == 0,
                    "Neural rendering started despite failed DLSS creation");
            Require(!upscaler.Upscale({}), "Failed creation did not request fallback");
            std::printf("PASS cold-start creation failure: no neural initialization\n");
            return;
        }
        Require(static_creation_extent.width == input_extent.width &&
                    static_creation_extent.height == input_extent.height,
                "Static caller feature allocation changed from its actual input size");
        Require(upscaler.Failed() == inject_failure, "Unexpected NGX evaluation result");
        const auto after_status = NvidiaDlssUpscaler::Status();
        Require(after_status.evaluations == before_status.evaluations + !inject_failure,
                "DLSS status counted an unsuccessful evaluation");
        if (!inject_failure) {
            Require(after_status.requested == options.quality &&
                        after_status.preset == options.preset,
                    "DLSS status lost runtime quality or preset");
            Require(after_status.mode != Quality::Automatic, "No evaluated quality reported");
        }
        if (inject_failure) {
            Require(!Ngx::GetStatus().error.empty(), "Evaluation failure was not diagnosed");
            Require(!upscaler.Upscale({}), "Failed upscaler did not request fallback");
        }
        readback.Invalidate();
        const auto bytes = readback.Mapped();
        std::array<double, 3> sums{};
        const size_t pixels = size_t{extent.width} * extent.height;
        for (size_t i = 0; i < pixels; ++i) {
            for (size_t c = 0; c < sums.size(); ++c) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + (i * 4 + c) * 2, sizeof(half));
                const float value = static_cast<float>(half);
                Require(std::isfinite(value), "Non-finite output");
                sums[c] += value;
            }
        }
        if (expect_neural && frame >= 29 && frame <= 34) {
            const bool first = frame % 2 == 1;
            if (first)
                exposure_pair.resize(pixels);
            for (size_t i = 0; i < pixels; ++i) {
                std::array<float, 3> color{};
                for (size_t c = 0; c < 3; ++c) {
                    _Float16 h;
                    std::memcpy(&h, bytes.data() + (i * 4 + c) * 2, 2);
                    color[c] = float(h);
                }
                if (first) {
                    exposure_pair[i] = color;
                    continue;
                }
                double y{};
                for (size_t c = 0; c < 3; ++c)
                    y += color[c] * exposure_weights[c];
                const double original_y = y <= .75 * .15 ? y / .75 : (y - .5 * .15) / .25;
                for (size_t c = 0; c < 3; ++c) {
                    const double expected_hdr = y > 0 ? color[c] * original_y / y : 0;
                    Require(
                        std::abs(exposure_pair[i][c] - expected_hdr) < .006,
                        "Native NR exposure differs from independently normalized/inverted output");
                }
            }
        }
        double style_difference{};
        for (size_t c = 0; c < sums.size(); ++c) {
            if (!neural_on || options.strength == 0 || inject_failure) {
                Require(std::abs(sums[c] / pixels - (inject_failure ? 0.0f : expected[c])) < 0.025,
                        "Readback differs from input; stale or incorrect output");
            } else {
                Require(sums[c] / pixels >= (frame == 33 || frame == 34 ? 0. : 0.01) &&
                            sums[c] / pixels < 16.0,
                        "Neural output is empty or outside the expected HDR range");
            }
            if (expect_neural && frame == 2) {
                natural_reset[c] = sums[c] / pixels;
            }
            if (expect_neural && (frame == 6 || frame == 9 || frame == 16 || frame == 18)) {
                Require(
                    std::abs(sums[c] / pixels - natural_reset[c]) < 0.001,
                    "Neural adjustment retained old history or differs from a fresh activation");
            }
            if (expect_neural && frame == 12) {
                const auto halfway = (expected[c] + natural_reset[c]) / 2;
                Require(std::abs(sums[c] / pixels - halfway) < 0.003,
                        "50% strength did not blend the original and neural images");
            }
            if (expect_neural && frame == 24) {
                reference_white_reset[c] = sums[c] / pixels;
                constexpr std::array<double, 3> prior_disabled{1.5838, .9620, .4628};
                Require(std::abs(reference_white_reset[c] - prior_disabled[c]) < .001,
                        "Disabled native exposure changed established NR output");
            }
            if (expect_neural && (frame == 26 || frame == 28))
                Require(std::abs(sums[c] / pixels - reference_white_reset[c]) < 0.001,
                        "Reference-white change retained incorrectly normalized NR history");
            if (expect_neural && frame == 29)
                exposure_reset[c] = sums[c] / pixels;
            if (expect_neural && (frame == 36 || frame == 38))
                Require(std::abs(sums[c] / pixels - exposure_reset[c]) < .001,
                        "Native exposure change retained incompatible proxy history");
            style_difference += std::abs(sums[c] / pixels - natural_reset[c]);
        }
        if (expect_neural && frame == 30) {
            double normalized_y{};
            for (size_t c = 0; c < 3; ++c)
                normalized_y += sums[c] / pixels * exposure_weights[c];
            Require(normalized_y < .75 * .15,
                    "Actual NR case did not cross from above to below the native exposure knee");
        }
        if (expect_neural && frame == 25) {
            double difference{};
            for (size_t c = 0; c < sums.size(); ++c)
                difference += std::abs(sums[c] / pixels - reference_white_reset[c]);
            Require(difference > 0.05,
                    "Reference-white override did not reach NR's actual HDR proxy/output");
            Require(reference_white_reset[0] - reference_white_reset[2] >
                        (sums[0] - sums[2]) / pixels + 0.1,
                    "Captured sand still lost chroma after reference-white normalization");
        }
        if (expect_neural && frame == 3) {
            Require(style_difference > 0.01,
                    "Switching Natural to Cinematic did not change output");
        }
        if (expect_neural && (frame == 15 || frame == 17)) {
            Require(style_difference > 0.00001, "Tone/structure control did not change output");
        }
        neural_frames += neural_on && !inject_failure;
        const auto neural = VideoCore::NeuralRendering::GetStatus();
        Require(neural.submitted_frames == neural_frames,
                "Incorrect neural rendering submission count");
        Require(neural.ready == (neural_on && !inject_failure),
                "Neural readiness did not follow the live setting and DLSS result");
        std::printf("PASS frame %zu: %ux%u -> %ux%u; strength/tone/structure %u/%u/%u; "
                    "RGB %.4f %.4f %.4f\n",
                    frame, input_extent.width, input_extent.height, extent.width, extent.height,
                    options.strength, options.tone, options.structure, sums[0] / pixels,
                    sums[1] / pixels, sums[2] / pixels);
    }
    Require(Ngx::GetStatus().evaluations == cases.size() - 1, "Incorrect evaluation count");
}
// Two independent cameras share color/depth/output attachments. Poison borders and the other
// tile, then verify actual NGX inputs, tile-local motion, untouched alpha and failure fallback.
void SceneRegionCases(const Device& device) {
    constexpr VkExtent2D tile{128, 72}, backing{258, 74};
    constexpr size_t pixels = size_t{backing.width} * backing.height;
    constexpr size_t tile_pixels = size_t{tile.width} * tile.height;
    constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device, ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    std::array<std::unique_ptr<SceneUpscaler>, 2> adapters;
    for (auto& adapter : adapters)
        adapter = std::make_unique<SceneUpscaler>(device, scheduler, allocator, pool, queue);
    Settings::values.nvidia_dlss_quality = Settings::NvidiaDlssQuality::Native;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    Settings::RefreshNvidiaDlssRuntimeSettings();
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto depth = MakeInput(device, allocator, VK_FORMAT_R32_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto target = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_ASPECT_COLOR_BIT, backing,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    color.resource.extent = depth.resource.extent = backing;
    auto buffer = [&](size_t bytes, VkBufferUsageFlags usage, MemoryUsage memory) {
        return allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE}, memory);
    };
    auto upload = buffer(pixels * 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
    auto readback = buffer(std::max(pixels * 8, tile_pixels * 16),
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Download);
    const auto encode = [](float x) {
        return .7f * (x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f);
    };
    const std::array linear{std::array{.25f, .5f, .75f}, std::array{.0001f, .0031308f, .8f}};
    for (u32 y = 0; y < backing.height; ++y) {
        for (u32 x = 0; x < backing.width; ++x) {
            const size_t p = size_t{y} * backing.width + x;
            const bool interior = y > 0 && y <= tile.height && x > 0 && x <= tile.width * 2;
            const size_t player = x > tile.width;
            std::array<_Float16, 4> source{}, native{};
            for (size_t c = 0; c < 3; ++c) {
                source[c] = interior ? encode(linear[player][c]) : 100.f;
                native[c] = .1f * (c + 1);
            }
            source[3] = .125f;
            native[3] = interior ? (player ? .625f : .375f) : 1.125f;
            const float z = interior ? (player ? .75f : .25f) : -999.f;
            std::memcpy(upload.Mapped().data() + p * 8, source.data(), 8);
            std::memcpy(upload.Mapped().data() + pixels * 8 + p * 4, &z, 4);
            std::memcpy(upload.Mapped().data() + pixels * 12 + p * 8, native.data(), 8);
        }
    }
    upload.Flush();
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    SCOPE_EXIT { api.evaluate = evaluate; };
    static decltype(api.evaluate) forward;
    static bool fail;
    static int observed_reset;
    static VkImage copied_color, copied_depth, copied_motion;
    forward = evaluate;
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback progress) {
        void* resource{};
        params->Get(NVSDK_NGX_Parameter_Color, &resource);
        copied_color = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_Depth, &resource);
        copied_depth = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_MotionVectors, &resource);
        copied_motion = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_Reset, &observed_reset);
        return fail ? NVSDK_NGX_Result_FAIL_FeatureNotSupported
                    : forward(cmd, feature, params, progress);
    };
    const auto barrier = [](vk::CommandBuffer cmd, VkPipelineStageFlags dst,
                            VkAccessFlags access) {
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst, 0,
            VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, access});
    };
    const auto copy = [&](vk::CommandBuffer cmd, VkImage image, VkExtent2D size, size_t offset) {
        cmd.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_GENERAL, *readback,
            VkBufferImageCopy{.bufferOffset = offset,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {size.width, size.height, 1}});
    };
    for (u32 step = 0; step < 3; ++step) {
        scheduler.Record([&, step](vk::CommandBuffer cmd) {
            for (const auto image : {*color.image, *depth.image, *target.image}) {
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = step ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0u,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = step ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image, .subresourceRange = range});
            }
            const std::array images{*color.image, *depth.image, *target.image};
            const std::array<size_t, 3> offsets{0, pixels * 8, pixels * 12};
            for (size_t i = 0; i < images.size(); ++i)
                cmd.CopyBufferToImage(*upload, images[i], VK_IMAGE_LAYOUT_GENERAL,
                    VkBufferImageCopy{.bufferOffset = offsets[i],
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .imageExtent = {backing.width, backing.height, 1}});
            barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        });
        for (u32 player = 0; player < 2; ++player) {
            const s32 x = 1 + player * tile.width;
            SceneUpscaler::Region region{{x, 1}, {x, 1}, {x, 1}, backing, true, .7f};
            SceneUpscaler::Camera camera{};
            camera.view = {1,0,0,0, 0,1,0,0, 0,0,1,0};
            camera.projection = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            camera.view_projection = camera.projection;
            if (step && !player) camera.view[3] = camera.view_projection[3] = -.1f;
            camera.pixel_to_clip = {2,-2,-1,1};
            camera.depth_to_clip = {2,-1};
            NvidiaDlssUpscaler::Inputs inputs{};
            inputs.color = color.resource;
            inputs.depth = depth.resource;
            inputs.render_size = inputs.upscale_size = tile;
            inputs.depth_inverted = false;
            inputs.hdr = true;
            if (step == 0) {
                for (u32 bad = 0; bad < 5; ++bad) {
                    auto invalid = region;
                    if (bad == 0) invalid.color_origin.x = -1;
                    if (bad == 1) invalid.depth_origin.x = backing.width;
                    if (bad == 2) invalid.target_origin.y = backing.height;
                    if (bad == 3) invalid.srgb_scale = 0;
                    if (bad == 4) invalid.srgb_scale = std::numeric_limits<float>::quiet_NaN();
                    Require(!adapters[player]->Record(inputs, *target.view, camera, step + 1,
                                VK_FORMAT_R16G16B16A16_SFLOAT, &invalid),
                            "Scene accepted invalid tile bounds/transfer");
                }
            }
            fail = step == 2 && player == 0;
            Require(adapters[player]->Record(inputs, *target.view, camera, step + 1,
                        VK_FORMAT_R16G16B16A16_SFLOAT, &region), "Scene rejected valid tile");
            scheduler.WaitWorker();
            scheduler.Record([&](vk::CommandBuffer cmd) {
                barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                copy(cmd, copied_color, tile, 0);
                copy(cmd, copied_depth, tile, tile_pixels * 8);
                copy(cmd, copied_motion, tile, tile_pixels * 12);
                barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            });
            scheduler.Finish();
            readback.Invalidate();
            Require(observed_reset == int(step == 0), "Tile history crossed cameras or reset unnecessarily");
            for (size_t p = 0; p < tile_pixels; ++p) {
                std::array<_Float16, 4> c;
                float z;
                std::array<_Float16, 2> mv;
                std::memcpy(c.data(), readback.Mapped().data() + p * 8, 8);
                std::memcpy(&z, readback.Mapped().data() + tile_pixels * 8 + p * 4, 4);
                std::memcpy(mv.data(), readback.Mapped().data() + tile_pixels * 12 + p * 4, 4);
                for (size_t k = 0; k < 3; ++k)
                    Require(std::abs(float(c[k]) - linear[player][k]) < .002f,
                            "Scene tile decoded another player's color or wrong sRGB branch");
                Require(z == (player ? .75f : .25f), "Scene tile copied another player's depth");
                const float expected_motion = step == 1 && player == 0 ? .05f * tile.width : 0.f;
                Require(std::abs(float(mv[0]) - expected_motion) < .01f && std::abs(float(mv[1])) < .01f,
                        "Scene tile motion/history crossed player origins");
            }
        }
        scheduler.Record([&](vk::CommandBuffer cmd) {
            barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            copy(cmd, *target.image, backing, 0);
            barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        scheduler.Finish();
        readback.Invalidate();
        for (u32 y = 0; y < backing.height; ++y) for (u32 x = 0; x < backing.width; ++x) {
            const bool inside = y > 0 && y <= tile.height && x > 0 && x <= tile.width * 2;
            const size_t player = x > tile.width;
            const bool replaced = inside && !(step == 2 && player == 0);
            std::array<_Float16, 4> actual;
            std::memcpy(actual.data(), readback.Mapped().data() + (size_t{y} * backing.width + x) * 8, 8);
            for (size_t c = 0; c < 3; ++c) {
                const float expected = replaced ? encode(linear[player][c]) : .1f * (c + 1);
                Require(std::abs(float(actual[c]) - expected) < (replaced ? .025f : .0002f),
                        "Scene tile output crossed bounds, changed encoding or lost native fallback");
            }
            Require(float(actual[3]) == (inside ? (player ? .625f : .375f) : 1.125f),
                    "Scene tile changed native alpha");
        }
    }
    std::printf("PASS Scene region: 6 evaluations, two isolated sRGB/depth/motion tiles, alpha, bounds and fallback\n");
}

// Exercise production camera reprojection through real NGX inputs during focal animation.
void SceneZoomCases(const Device& device) {
    constexpr VkExtent2D extent{320, 180};
    constexpr size_t pixels = size_t{extent.width} * extent.height;
    constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    SceneUpscaler adapter{device, scheduler, allocator, pool, queue};
    Settings::values.nvidia_dlss_quality = Settings::NvidiaDlssQuality::Native;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    Settings::RefreshNvidiaDlssRuntimeSettings();
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, extent);
    auto depth =
        MakeInput(device, allocator, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, extent);
    auto target = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_ASPECT_COLOR_BIT, extent, VK_IMAGE_USAGE_STORAGE_BIT);
    auto readback = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .size = pixels * 4,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                           MemoryUsage::Download);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        for (const auto& resource : {color.resource, depth.resource, target.resource}) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = resource.image,
                                     .subresourceRange = range});
            VkClearColorValue clear{};
            clear.float32[0] = clear.float32[1] = clear.float32[2] = .5f;
            clear.float32[3] = .375f;
            cmd.ClearColorImage(resource.image, VK_IMAGE_LAYOUT_GENERAL, clear, range);
        }
        cmd.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT});
    });
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    SCOPE_EXIT {
        api.evaluate = evaluate;
    };
    static decltype(api.evaluate) forward;
    static VkImage observed_motion;
    static int observed_reset;
    forward = evaluate;
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback progress) {
        void* resource{};
        params->Get(NVSDK_NGX_Parameter_MotionVectors, &resource);
        observed_motion =
            static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_Reset, &observed_reset);
        return forward(cmd, feature, params, progress);
    };
    struct Case {
        float fx, fy;
        bool animate, reset;
    };
    constexpr std::array cases{
        Case{1, 2, false, true},
        Case{1, 2, false, false},        // Cold feature then stationary warmup.
        Case{1.1f, 2.2f, false, true},   // Existing default zoom reset.
        Case{1.1f, 2.2f, true, true},    // Policy transition resets.
        Case{1.21f, 2.42f, true, false}, // Matched focal increase/decrease.
        Case{1.1f, 2.2f, true, false},
        Case{1.1f, 2.3f, true, true},  // Aspect change.
        Case{1.1f, 2.3f, true, true},  // Depth mapping change.
        Case{1.1f, 2.3f, true, true},  // Explicit caller reset.
        Case{1.1f, 2.3f, true, true},  // Frame gap.
        Case{1.1f, 2.3f, false, true}, // Return to default policy.
        Case{1.2f, 2.3f * 1.2f / 1.1f, false, true},
        Case{1.2f, 2.3f * 1.2f / 1.1f, true, true}, // Policy transition.
        Case{1.2f, 2.3f * 1.2f / 1.1f, true, true}, // Position cut.
        Case{1.2f, 2.3f * 1.2f / 1.1f, true, true}, // Rotation cut at same world position.
        Case{1.2f, 2.3f * 1.2f / 1.1f, true, true}, // Clip convention change.
    };
    const auto before = Ngx::GetStatus().evaluations;
    for (size_t step = 0; step < cases.size(); ++step) {
        const auto& test = cases[step];
        SceneUpscaler::Camera camera{};
        camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (step == 13)
            camera.view[3] = -100;
        if (step >= 14)
            camera.view = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 100};
        camera.projection = {
            test.fx, 0, 0, 0, 0, test.fy, 0, 0, 0, 0, .001f, step >= 7 ? .11f : .1f, 0, 0, -1, 0};
        for (size_t row = 0; row < 4; ++row)
            for (size_t col = 0; col < 4; ++col) {
                double value = col == 3 ? camera.projection[row * 4 + 3] : 0;
                for (size_t k = 0; k < 3; ++k)
                    value += double(camera.projection[row * 4 + k]) * camera.view[k * 4 + col];
                camera.view_projection[row * 4 + col] = static_cast<float>(value);
            }
        camera.pixel_to_clip =
            step == 15 ? std::array<f32, 4>{2, 2, -1, -1} : std::array<f32, 4>{2, -2, -1, 1};
        camera.depth_to_clip = {1, 0};
        camera.focal_scale_animation = test.animate;
        NvidiaDlssUpscaler::Inputs inputs{};
        inputs.color = color.resource;
        inputs.depth = depth.resource;
        inputs.render_size = inputs.upscale_size = extent;
        inputs.hdr = true;
        inputs.depth_inverted = true;
        inputs.reset = step == 8;
        observed_motion = VK_NULL_HANDLE;
        Require(adapter.Record(inputs, *target.view, camera, step + 1 + (step >= 9),
                               VK_FORMAT_R16G16B16A16_SFLOAT),
                "Focal-scale frame declined");
        scheduler.WaitWorker();
        Require(observed_motion != VK_NULL_HANDLE, "Focal-scale frame did not evaluate NGX");
        if (step != 1)
            Require(observed_reset == int(test.reset), "Focal-scale history reset policy changed");
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                observed_motion, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        readback.Invalidate();
        for (u32 y = 0; y < extent.height; ++y)
            for (u32 x = 0; x < extent.width; ++x) {
                std::array<_Float16, 2> motion;
                std::memcpy(motion.data(),
                            readback.Mapped().data() + (size_t{y} * extent.width + x) * 4, 4);
                const bool zoom = step == 4 || step == 5;
                const double expected_x =
                    zoom ? (double(cases[step - 1].fx) / test.fx - 1) * (x + .5 - extent.width * .5)
                         : 0;
                const double expected_y = zoom ? (double(cases[step - 1].fy) / test.fy - 1) *
                                                     (y + .5 - extent.height * .5)
                                               : 0;
                Require(std::abs(float(motion[0]) - expected_x) < .02 &&
                            std::abs(float(motion[1]) - expected_y) < .02,
                        "Focal-scale pixel motion does not reproduce analytic zoom");
            }
        Require(Ngx::GetStatus().evaluations == before + step + 1,
                "Focal-scale NGX successful evaluation count changed");
        std::printf("PASS scene focal step %zu: opt-in %d reset %d\n", step, test.animate,
                    observed_reset);
    }
}

// Exercise the production copy, reprojection and asynchronous fallback paths with the same
// one-column UltraPerformance rounding mismatch as Kirby, at a smaller hardware-test size.
void SceneResampleCases(const Device& device) {
    constexpr VkExtent2D source{134, 80}, render{133, 80}, backing{137, 82}, output{400, 240};
    constexpr size_t source_pixels = size_t{backing.width} * backing.height;
    constexpr size_t render_pixels = size_t{render.width} * render.height;
    constexpr size_t output_pixels = size_t{output.width} * output.height;
    constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    SceneUpscaler adapter{device, scheduler, allocator, pool, queue};
    Settings::values.nvidia_dlss_quality = Settings::NvidiaDlssQuality::Automatic;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    Settings::RefreshNvidiaDlssRuntimeSettings();
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto depth =
        MakeInput(device, allocator, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto target =
        MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                  output, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    color.resource.extent = depth.resource.extent = backing;
    auto buffer = [&](size_t bytes, VkBufferUsageFlags usage, MemoryUsage memory) {
        return allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = bytes,
                                       .usage = usage,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                      memory);
    };
    auto upload = buffer(source_pixels * 12 + output_pixels * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         MemoryUsage::Upload);
    auto readback = buffer(render_pixels * 16 + output_pixels * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           MemoryUsage::Download);
    std::vector<std::array<_Float16, 4>> colors(source_pixels);
    std::vector<float> depths(source_pixels);
    for (u32 y = 0; y < backing.height; ++y) {
        for (u32 x = 0; x < backing.width; ++x) {
            const size_t p = size_t{y} * backing.width + x;
            const bool active = x < source.width && y < source.height;
            colors[p] = {active ? _Float16(x == source.width - 1 ? .875f : .1f + .4f * (x % 3))
                                : _Float16(100),
                         active ? _Float16(.1f + .5f * y / source.height) : _Float16(100),
                         active ? _Float16(.2f + .3f * x / source.width) : _Float16(100), 1};
            depths[p] = active ? .1f + .6f * x / source.width + .1f * y / source.height : -999.f;
        }
    }
    const std::array<_Float16, 4> native{.875f, .125f, .25f, .375f};
    std::memcpy(upload.Mapped().data(), colors.data(), source_pixels * 8);
    std::memcpy(upload.Mapped().data() + source_pixels * 8, depths.data(), source_pixels * 4);
    for (size_t p = 0; p < output_pixels; ++p)
        std::memcpy(upload.Mapped().data() + source_pixels * 12 + p * 8, native.data(), 8);
    upload.Flush();
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    SCOPE_EXIT {
        api.evaluate = evaluate;
    };
    static decltype(api.evaluate) forward;
    static VkImage copied_color, copied_depth, copied_motion;
    static int observed_reset;
    forward = evaluate;
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback progress) {
        void* resource{};
        params->Get(NVSDK_NGX_Parameter_Color, &resource);
        copied_color = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_Depth, &resource);
        copied_depth = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_MotionVectors, &resource);
        copied_motion = static_cast<NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo.Image;
        params->Get(NVSDK_NGX_Parameter_Reset, &observed_reset);
        return forward(cmd, feature, params, progress);
    };
    const auto evaluations_before = Ngx::GetStatus().evaluations;
    u64 evaluated{};
    for (u32 step = 0; step < 6; ++step) {
        const bool unsupported = step == 0 || step == 4;
        const bool resampled = step == 1 || step == 2 || step == 5;
        const u64 frame = step + 1 + (step >= 2 ? 1 : 0);
        copied_color = copied_depth = copied_motion = VK_NULL_HANDLE;
        scheduler.Record([&, step](vk::CommandBuffer cmd) {
            const std::array images{*color.image, *depth.image, *target.image};
            for (size_t i = 0; i < images.size(); ++i) {
                if (step && i < 2)
                    continue;
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask =
                            step ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0u,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = step ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range});
                const std::array<size_t, 3> offsets{0, source_pixels * 8, source_pixels * 12};
                const auto size = i == 2 ? output : backing;
                cmd.CopyBufferToImage(
                    *upload, images[i], VK_IMAGE_LAYOUT_GENERAL,
                    VkBufferImageCopy{.bufferOffset = offsets[i],
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {size.width, size.height, 1}});
            }
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT});
        });
        SceneUpscaler::Camera camera{};
        camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        camera.view_projection = camera.projection;
        if (step >= 2)
            camera.view[3] = camera.view_projection[3] = -.1f;
        camera.pixel_to_clip = {2, -2, -1, 1};
        camera.depth_to_clip = {1, 0};
        NvidiaDlssUpscaler::Inputs inputs{};
        inputs.color = color.resource;
        inputs.depth = depth.resource;
        inputs.render_size = unsupported ? source : render;
        inputs.upscale_size = output;
        inputs.depth_inverted = false;
        inputs.hdr = true;
        if (step == 1) {
            auto invalid = inputs;
            invalid.jitter[0] = .25f;
            Require(!adapter.RecordResampled(invalid, *target.view, camera, frame, source,
                                             VK_FORMAT_R16G16B16A16_SFLOAT),
                    "Resample accepted unsupported native jitter");
            Require(!adapter.RecordResampled(inputs, *target.view, camera, frame, {138, 80},
                                             VK_FORMAT_R16G16B16A16_SFLOAT),
                    "Resample accepted source outside backing");
        }
        const bool recorded = resampled
                                  ? adapter.RecordResampled(inputs, *target.view, camera, frame,
                                                            source, VK_FORMAT_R16G16B16A16_SFLOAT)
                                  : adapter.Record(inputs, *target.view, camera, frame,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
        Require(recorded != unsupported, "Unsupported crop did not preserve recoverable fallback");
        scheduler.WaitWorker();
        Require(NvidiaDlss::IsAvailable(), "Unsupported crop permanently poisoned NGX runtime");
        if (!unsupported) {
            Require(copied_color && copied_depth && copied_motion,
                    "Resampled frame did not evaluate NGX");
            Require(observed_reset == int(step != 2),
                    "Resampling transition/reset contract changed");
            ++evaluated;
        } else {
            Require(!copied_color, "Unsupported crop unexpectedly evaluated NGX");
        }
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT});
            const auto copy = [&](VkImage image, VkExtent2D size, size_t offset) {
                cmd.CopyImageToBuffer(
                    image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                    VkBufferImageCopy{.bufferOffset = offset,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {size.width, size.height, 1}});
            };
            if (!unsupported) {
                copy(copied_color, render, 0);
                copy(copied_depth, render, render_pixels * 8);
                copy(copied_motion, render, render_pixels * 12);
            }
            copy(*target.image, output, render_pixels * 16);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        readback.Invalidate();
        if (!unsupported) {
            for (u32 y = 0; y < render.height; ++y) {
                for (u32 x = 0; x < render.width; ++x) {
                    const size_t p = size_t{y} * render.width + x;
                    const double position =
                        resampled ? (x + .5) * source.width / render.width - .5 : x;
                    const u32 lo = static_cast<u32>(std::floor(position));
                    const u32 hi = std::min(lo + 1, source.width - 1);
                    const double weight = position - lo;
                    std::array<_Float16, 4> c;
                    float z;
                    std::array<_Float16, 2> mv;
                    std::memcpy(c.data(), readback.Mapped().data() + p * 8, 8);
                    std::memcpy(&z, readback.Mapped().data() + render_pixels * 8 + p * 4, 4);
                    std::memcpy(mv.data(), readback.Mapped().data() + render_pixels * 12 + p * 4,
                                4);
                    for (size_t k = 0; k < 3; ++k) {
                        const double expected =
                            float(colors[y * backing.width + lo][k]) * (1 - weight) +
                            float(colors[y * backing.width + hi][k]) * weight;
                        Require(std::abs(float(c[k]) - expected) < .001,
                                "Odd-width resample cropped a column or sampled backing padding");
                    }
                    const u32 nearest =
                        resampled ? static_cast<u32>((x + .5) * source.width / render.width) : x;
                    Require(std::abs(z - depths[y * backing.width + nearest]) < 1e-6,
                            "Resampled depth did not use the matching pixel center");
                    Require(std::abs(float(mv[0]) - (step == 2 ? .05f * render.width : 0.f)) <
                                    .01f &&
                                std::abs(float(mv[1])) < .01f,
                            "Camera motion was not expressed in the resampled pixel extent");
                }
            }
        }
        for (size_t p = 0; p < output_pixels; ++p) {
            std::array<_Float16, 4> value;
            std::memcpy(value.data(), readback.Mapped().data() + render_pixels * 16 + p * 8, 8);
            Require(value[3] == native[3], "Resampling changed native output alpha");
            if (unsupported)
                Require(value == native, "Unsupported NGX input overwrote native fallback");
        }
        if (step == 1) {
            // First model creation exceeded the production 250 ms gap threshold on hardware.
            // Re-establish a warm stationary history before testing the moving frame's vectors.
            Require(adapter.RecordResampled(inputs, *target.view, camera, frame + 1, source,
                                            VK_FORMAT_R16G16B16A16_SFLOAT),
                    "Warm resampling frame was declined");
            scheduler.Finish();
            ++evaluated;
        }
        Require(Ngx::GetStatus().evaluations == evaluations_before + evaluated,
                "Unsupported/resampled evaluation count is incorrect");
        std::printf("PASS scene resample step %u: %s; successful evaluations %llu\n", step,
                    unsupported ? "native fallback"
                    : resampled ? "full-image resample"
                                : "exact copy",
                    static_cast<unsigned long long>(evaluated));
    }
}

// Direct backend callers must reset after a rejected native frame even when the next crop
// reuses the existing feature. Scene's separate frame-gap reset cannot hide this regression.
void NvidiaInputRecoveryCases(const Device& device) {
    constexpr VkExtent2D backing{134, 80}, supported{133, 80}, output{400, 240};
    constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    NvidiaDlssUpscaler upscaler{device, scheduler, allocator};
    Settings::values.nvidia_dlss_quality = Settings::NvidiaDlssQuality::Automatic;
    Settings::values.nvidia_dlss_preset = Settings::NvidiaDlssPreset::NvidiaDefault;
    Settings::values.neural_rendering = Settings::NeuralRendering::Off;
    Settings::RefreshNvidiaDlssRuntimeSettings();
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto depth =
        MakeInput(device, allocator, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, backing);
    auto motion =
        MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, backing);
    color.resource.extent = depth.resource.extent = motion.resource.extent = backing;
    scheduler.Record([&](vk::CommandBuffer cmd) {
        const std::array images{*color.image, *depth.image, *motion.image};
        const std::array clears{VkClearColorValue{{.1f, .3f, .5f, 1.f}},
                                VkClearColorValue{{.5f, 0, 0, 0}}, VkClearColorValue{}};
        for (size_t i = 0; i < images.size(); ++i) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = images[i],
                                     .subresourceRange = range});
            cmd.ClearColorImage(images[i], VK_IMAGE_LAYOUT_GENERAL, clears[i], range);
        }
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                            VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_MEMORY_READ_BIT});
    });
    auto& api = const_cast<Ngx::Api&>(Ngx::GetApi());
    const auto create = api.create;
    const auto evaluate = api.evaluate;
    const auto capabilities = api.capabilities;
    SCOPE_EXIT {
        api.create = create;
        api.evaluate = evaluate;
        api.capabilities = capabilities;
    };
    static decltype(api.create) forward_create;
    static decltype(api.evaluate) forward_evaluate;
    static decltype(api.capabilities) forward_capabilities;
    static u32 creates, queries, calls;
    static int observed_reset;
    forward_create = create;
    forward_evaluate = evaluate;
    forward_capabilities = capabilities;
    creates = queries = calls = 0;
    api.create = [](VkDevice dev, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
        ++creates;
        return forward_create(dev, cmd, feature, parameters, handle);
    };
    api.capabilities = [](NVSDK_NGX_Parameter** parameters) {
        ++queries;
        return forward_capabilities(parameters);
    };
    api.evaluate = [](VkCommandBuffer cmd, const NVSDK_NGX_Handle* feature,
                      const NVSDK_NGX_Parameter* parameters,
                      PFN_NVSDK_NGX_ProgressCallback progress) {
        ++calls;
        parameters->Get(NVSDK_NGX_Parameter_Reset, &observed_reset);
        return forward_evaluate(cmd, feature, parameters, progress);
    };
    const auto before = Ngx::GetStatus().evaluations;
    VkImage stable_output{};
    for (u32 step = 0; step < 5; ++step) {
        const bool rejected = step == 1 || step == 2;
        NvidiaDlssUpscaler::Inputs inputs{};
        inputs.color = color.resource;
        inputs.depth = depth.resource;
        inputs.motion_vectors = motion.resource;
        inputs.render_size = rejected ? backing : supported;
        inputs.upscale_size = output;
        inputs.motion_vector_scale = {1, 1};
        inputs.depth_inverted = false;
        inputs.dynamic_resolution = true;
        Require(upscaler.Upscale(inputs) != rejected,
                "Direct backend input recovery declined valid input");
        scheduler.Finish();
        Require(!upscaler.Failed(), "Unsupported size poisoned the direct backend");
        if (!step)
            stable_output = upscaler.OutputImage();
        Require(upscaler.OutputImage() == stable_output,
                "Rejected input recreated the existing output");
        if (!rejected)
            Require(observed_reset == int(step != 4),
                    "Direct A/B/A recovery missed or retained pending reset");
        Require(creates == 1, "Direct A/B/A recovery unnecessarily recreated the feature");
        Require(queries == (step == 0 ? 2u : 3u),
                "Unchanged/rejected inputs repeated capability queries");
        Require(calls == (step == 0  ? 1u
                          : step < 3 ? 1u
                                     : step - 1),
                "Rejected input submitted an evaluation");
    }
    Require(Ngx::GetStatus().evaluations == before + 3, "Direct recovery evaluation count changed");
    std::printf(
        "PASS direct NGX recovery: A/B/B/A/A, one feature, cached query, pending reset consumed\n");
}

} // namespace

/// Exercise NGX startup, model fallback, and repeated device lifecycles on the selected GPU.
int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: ngx_smoke <Linux DLSS directory> [GPU index] [scale 1-8] [NR "
                             "model directory or missing]\n");
        return 2;
    }
    const auto scratch =
        std::filesystem::temp_directory_path() / ("lichii-ngx-smoke-" + std::to_string(getpid()));
    std::filesystem::create_directories(scratch / "log");
    std::filesystem::create_directories(scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, scratch);
    Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, scratch / "log");
    Common::Log::Initialize();
    Common::Log::Start();
    Settings::values.nvidia_dlss = false;
    setenv("LICHII_NGX_DIR", scratch.c_str(), 1);
    Require(!NvidiaDlss::Start() && !Ngx::GetStatus().error.empty(), "Missing model not diagnosed");
    setenv("LICHII_NGX_DIR", std::filesystem::absolute(argv[1]).c_str(), 1);
    const size_t gpu = argc > 2 ? std::stoul(argv[2]) : 0;
    const auto scale = argc > 3 ? std::stoul(argv[3]) : 1;
    const bool request_neural = argc > 4;
    const bool expect_neural = request_neural && std::string_view{argv[4]} != "missing";
    if (expect_neural) {
        const auto target = VideoCore::NeuralRendering::UserFolder();
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::create_directory_symlink(std::filesystem::absolute(argv[4]), target);
    }
    Require(scale >= 1 && scale <= 8, "Scale must be between 1 and 8 (4K output)");
    const bool scene_only = std::getenv("LICHII_NGX_SMOKE_SCENE_ONLY") != nullptr;
    const bool neural_only = std::getenv("LICHII_NGX_SMOKE_NR_ONLY") != nullptr;
    Require(!neural_only || (expect_neural && !scene_only),
            "NR-only smoke requires an installed model and cannot be combined with scene-only");
    for (int run = 0; run < (scene_only || neural_only ? 1 : 3); ++run) {
        const bool fail_creation = run == 2;
        // Both DLSS and neural rendering may start off, then be enabled without a new device.
        Settings::values.nvidia_dlss = run != 0;
        Settings::values.neural_rendering = Settings::NeuralRendering::Off;
        Settings::RefreshNvidiaDlssRuntimeSettings();
        Require(NvidiaDlss::Start(), "NGX startup failed");
        {
            auto library = OpenLibrary();
            vk::InstanceDispatch dispatch;
            auto instance = CreateInstance(*library, dispatch, VK_API_VERSION_1_3,
                                           Core::Frontend::WindowSystemType::X11);
            const auto physical_devices = instance.EnumeratePhysicalDevices();
            Require(gpu < physical_devices.size(), "GPU index out of range");
            Device device{*instance, vk::PhysicalDevice{physical_devices[gpu], dispatch},
                          VK_NULL_HANDLE, dispatch};
            Require(NvidiaDlss::IsAvailable(), "Native DLSS unavailable on selected device");
            Require(!request_neural || device.SupportsNeuralRendering(),
                    "Neural rendering device features were not enabled");
            Require(!NvidiaDlss::WantsDevice(), "Attached runtime still requests a device");
            auto duplicate_dispatch = device.GetDispatchLoader();
            Require(!NvidiaDlss::AttachDevice(*instance, physical_devices[gpu],
                                              *device.GetLogical(), device.GetGraphicsFamily(),
                                              duplicate_dispatch),
                    "NGX accepted a second device attachment");
            Require(NvidiaDlss::IsAvailable(), "Duplicate attachment disturbed the active device");
            Settings::values.nvidia_dlss = true;
            Settings::RefreshNvidiaDlssRuntimeSettings();
            if (!scene_only) {
                RenderCases(device, static_cast<u32>(scale), request_neural, expect_neural,
                            fail_creation);
            }
            if (!scene_only && !neural_only) {
                StarFoxCases(device, false);
                StarFoxCases(device, true);
                StarFoxCases(device, false, true);
                StarFoxCases(device, true, true);
            }
            if (!neural_only) {
                SceneRegionCases(device);
                SceneResampleCases(device);
                SceneZoomCases(device);
                NvidiaInputRecoveryCases(device);
                NrCropSmoke(device);
                NrExposureSmoke(device);
                YoshiTemporalMotionSmoke(device);
                MarioKartMotionSmoke(device);
                SceneCases(device, false);
                SceneCases(device, true);
                SceneCases(device, true, true);
                SceneCases(device, false, false, true);
                SceneCases(device, false, false, false, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                SceneCases(device, true, false, false, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                SceneCases(device, false, false, false, VK_FORMAT_R8G8B8A8_UNORM, true);
                SceneCases(device, true, false, false, VK_FORMAT_R8G8B8A8_UNORM, true);
                SceneCases(device, false, false, false, VK_FORMAT_R16G16B16A16_SFLOAT, true);
                SceneCases(device, true, false, false, VK_FORMAT_R16G16B16A16_SFLOAT, true);
                SceneCases(device, false, false, false, VK_FORMAT_B10G11R11_UFLOAT_PACK32, true,
                           true);
                SceneCases(device, true, false, false, VK_FORMAT_B10G11R11_UFLOAT_PACK32, true,
                           true);
            }
        }
        Require(!NvidiaDlss::IsAvailable(), "Device teardown left NGX active");
        std::printf("PASS device lifecycle %d\n", run + 1);
    }
    NvidiaDlss::Stop();
    std::printf("Logs/cache: %s\n", scratch.c_str());
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
