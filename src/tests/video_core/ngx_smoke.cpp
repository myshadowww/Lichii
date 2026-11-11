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
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

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
    auto depth = MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, input_extent);
    auto motion = MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT,
                            VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    auto alternate_motion_view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *motion.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = motion.resource.format,
        .subresourceRange = motion.resource.range,
    });
    const auto original_motion_view = motion.resource.view;
    auto &api = const_cast<Ngx::Api &>(Ngx::GetApi());
    const auto create = api.create;
    const auto evaluate = api.evaluate;
    SCOPE_EXIT {
        api.create = create;
        api.evaluate = evaluate;
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
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Natural, 100, 100, 100,
                                          true, Settings::NvidiaDlssPreset::M},
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
        Settings::NvidiaDlssRuntimeSettings{Quality::Quality, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::K},
        Settings::NvidiaDlssRuntimeSettings{Quality::Balanced, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::M},
        Settings::NvidiaDlssRuntimeSettings{Quality::Performance, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::L},
        Settings::NvidiaDlssRuntimeSettings{Quality::UltraPerformance, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::J},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::NvidiaDefault},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural}};
    u64 neural_frames{};
    std::array<double, 3> natural_reset{};
    for (size_t frame = 0; frame < cases.size(); ++frame) {
        Settings::values.nvidia_dlss_quality = cases[frame].quality;
        Settings::values.nvidia_dlss_preset = cases[frame].preset;
        Settings::values.neural_rendering = request_neural ? cases[frame].neural_rendering : Neural::Off;
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
            api.evaluate = [](VkCommandBuffer, const NVSDK_NGX_Handle *,
                              const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback) {
                return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
            };
        }
        const bool hdr = frame == 5 || frame == 10 || frame == 14;
        const std::array<float, 4> expected = frame == 14 ? std::array{2.0f, 0.25f, 0.5f, 1.0f}
                                            : hdr ? std::array{0.75f, 0.25f, 0.5f, 1.0f}
                                                  : std::array{0.25f, 0.5f, 0.75f, 1.0f};
        scheduler.Record([&, frame, expected](vk::CommandBuffer cmd) {
            for (const auto &image : {color.resource, depth.resource, motion.resource}) {
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
        Require(upscaler.Upscale({color.resource,
                                  depth.resource,
                                  motion.resource,
                                  input_extent,
                                  extent,
                                  {0, 0},
                                  {1, 1},
                                  frame == 0,
                                  frame != 0 && !hdr,
                                  hdr}),
                "Upscale refused a frame");
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
        Require(upscaler.Failed() == inject_failure, "Unexpected NGX evaluation result");
        const auto after_status = NvidiaDlssUpscaler::Status();
        Require(after_status.evaluations == before_status.evaluations + !inject_failure,
                "DLSS status counted an unsuccessful evaluation");
        if (!inject_failure) {
            Require(after_status.requested == options.quality && after_status.preset == options.preset,
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
        double style_difference{};
        for (size_t c = 0; c < sums.size(); ++c) {
            if (!neural_on || options.strength == 0 || inject_failure) {
                Require(std::abs(sums[c] / pixels - (inject_failure ? 0.0f : expected[c])) < 0.025,
                        "Readback differs from input; stale or incorrect output");
            } else {
                Require(sums[c] / pixels > 0.01 && sums[c] / pixels < 16.0,
                        "Neural output is empty or outside the expected HDR range");
            }
            if (expect_neural && frame == 2) {
                natural_reset[c] = sums[c] / pixels;
            }
            if (expect_neural && (frame == 6 || frame == 9 || frame == 16 || frame == 18)) {
                Require(std::abs(sums[c] / pixels - natural_reset[c]) < 0.001,
                        "Neural adjustment retained old history or differs from a fresh activation");
            }
            if (expect_neural && frame == 12) {
                const auto halfway = (expected[c] + natural_reset[c]) / 2;
                Require(std::abs(sums[c] / pixels - halfway) < 0.003,
                        "50% strength did not blend the original and neural images");
            }
            style_difference += std::abs(sums[c] / pixels - natural_reset[c]);
        }
        if (expect_neural && frame == 3) {
            Require(style_difference > 0.01, "Switching Natural to Cinematic did not change output");
        }
        if (expect_neural && (frame == 15 || frame == 17)) {
            Require(style_difference > 0.00001, "Tone/structure control did not change output");
        }
        neural_frames += neural_on && !inject_failure;
        const auto neural = VideoCore::NeuralRendering::GetStatus();
        Require(neural.submitted_frames == neural_frames, "Incorrect neural rendering submission count");
        Require(neural.ready == (neural_on && !inject_failure),
                "Neural readiness did not follow the live setting and DLSS result");
        std::printf("PASS frame %zu: %ux%u -> %ux%u; strength/tone/structure %u/%u/%u; "
                    "RGB %.4f %.4f %.4f\n", frame,
                    input_extent.width, input_extent.height, extent.width, extent.height,
                    options.strength, options.tone, options.structure,
                    sums[0] / pixels, sums[1] / pixels, sums[2] / pixels);
    }
    Require(Ngx::GetStatus().evaluations == cases.size() - 1, "Incorrect evaluation count");
}
} // namespace

/// Exercise NGX startup, model fallback, and repeated device lifecycles on the selected GPU.
int main(int argc, char **argv) try {
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
    for (int run = 0; run < 3; ++run) {
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
            RenderCases(device, static_cast<u32>(scale), request_neural, expect_neural,
                        fail_creation);
            StarFoxCases(device, false);
            StarFoxCases(device, true);
            StarFoxCases(device, false, true);
            StarFoxCases(device, true, true);
        }
        Require(!NvidiaDlss::IsAvailable(), "Device teardown left NGX active");
        std::printf("PASS device lifecycle %d\n", run + 1);
    }
    NvidiaDlss::Stop();
    std::printf("Logs/cache: %s\n", scratch.c_str());
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
