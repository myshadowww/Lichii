// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS replacement through the platform runtime (Streamline or native NGX).

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace VideoCore::NeuralRendering {
class Backend;
}
namespace VideoCore {
struct DlssStatus;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class NvidiaDlssUpscaler {
  public:
    struct Image {
        VkImageView view;
        VkImage image;
        VkFormat format;
        VkImageSubresourceRange range;     ///< The view's level and layer, with the view's aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
        VkExtent2D extent{}; ///< Actual view extent; zero uses the active render/output size.
    };

    struct Inputs {
        Image color;          ///< Jittered colour at the render size.
        Image depth;          ///< Device depth (reversed) at the render size.
        Image motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        /// This frame's sample offset in render pixels, as the game hands its own DLSS.
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels (the render size for UV offsets).
        std::array<f32, 2> motion_vector_scale;
        bool reset;
        bool depth_inverted = true;
        bool hdr = true; ///< Linear HDR colour. False for tonemapped, perceptually encoded colour.
        bool dynamic_resolution =
            false; ///< Opt in to dynamic input range and maximum L/M allocation.
        f32 neural_reference_white =
            1.0f; ///< Scene-linear reference white for NR only; no DLSS exposure change.
        std::array<f32, 3> neural_reference_exposure{}; ///< Optional base, extra, luminance cap.
    };

    /// Throws vk::Exception when the runtime has no DLSS for this device.
    explicit NvidiaDlssUpscaler(const Device& device, Scheduler& scheduler,
                                MemoryAllocator& memory_allocator);
    ~NvidiaDlssUpscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when DLSS can't run, so the caller can fall back to FSR 3.1.
    bool Upscale(const Inputs &inputs);

    [[nodiscard]] bool Failed() const noexcept { return failed.load(); }

    /// Evaluations that succeeded in this process, for the status bar.
    [[nodiscard]] static VideoCore::DlssStatus Status();

    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }
    [[nodiscard]] VkSampler LinearSampler() const noexcept {
        return *linear_sampler;
    }

  private:
    void CreateSampler();
    void CreateOutput(VkExtent2D upscale_size);

    const Device &device;
    Scheduler &scheduler;
    MemoryAllocator &memory_allocator;
#ifdef EGLANTINE_NGX
    void PrepareNeuralRendering(const Inputs& inputs, bool requested);
    std::unique_ptr<VideoCore::NeuralRendering::Backend> neural_rendering;
    VkImageView neural_motion_view{};
    bool neural_requested{};
    bool neural_failed{};
    bool neural_model_verified{};
    NVSDK_NGX_Handle *feature{};
    NVSDK_NGX_Parameter *parameters{};
    bool depth_inverted = true;
    bool dynamic_resolution{};
    // Written on the scheduler worker; the renderer reads these only after WaitWorker().
    VkExtent2D feature_min_extent{}, feature_max_extent{};
    VkExtent2D requested_min_extent{}, requested_max_extent{};
    VkExtent2D unsupported_render_extent{}, unsupported_output_extent{};
    Settings::NvidiaDlssQuality unsupported_quality{};
    Settings::NvidiaDlssPreset unsupported_preset{};
    bool unsupported_dynamic_resolution{};
    bool unsupported_input{};
    bool pending_history_reset{};
#else
    void *set_options{}; ///< slDLSSSetOptions
    void *get_state{}; ///< slDLSSGetState
    void *get_optimal_settings{};
#endif
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    bool hdr_input = true;
    Settings::NvidiaDlssQuality quality_mode{};
    Settings::NvidiaDlssPreset model_preset{};
    Settings::NvidiaDlssQuality evaluated_mode{}; ///< Recorded on the scheduler's worker
    u32 frame_index{};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    vk::Sampler linear_sampler;
    std::atomic<bool> failed{};
    std::atomic<bool> vram_warned{};
};

} // namespace Vulkan
