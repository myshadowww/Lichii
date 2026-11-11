// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace VideoCore::NeuralRendering {

enum class Style : std::uint32_t { Standard, Natural, Cinematic };

inline bool ValidReferenceWhite(float value) {
    return std::isfinite(value) && value >= 0.05f;
}

// Zero disables this native-only proxy transform. Enabled values describe
// Yout = base*Yin + extra*min(Yin, cap), using native Rec.709 weights.
inline bool ValidReferenceExposure(const std::array<float, 3>& value) {
    if (value == std::array<float, 3>{})
        return true;
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]) &&
           value[0] >= 0.05f && value[1] >= 0 && value[0] + value[1] <= 20 && value[2] >= 0 &&
           value[2] <= 65504;
}

struct Frame {
    Style style = Style::Standard;
    float intensity = 1.0f;
    float tone = 1.0f;
    float structure = 1.0f;
    std::array<float, 2> motion_scale{};  // Stored motion vector to UV offset.
    std::array<float, 2> motion_jitter{}; // Motion sampling offset in output pixels.
    std::array<std::uint16_t, 2>
        motion_extent{}; // Active origin-zero crop; zero uses the full view.
    bool reset = false;
    bool hdr = false;
    float reference_white = 1.0f; // Scene-linear value corresponding to display reference white.
    std::array<float, 3> reference_exposure{}; // Optional native exposure instead of scalar white.
};

// Private native inference backend. Construction submits uploads and warms the graph: the caller
// must finish the renderer queue first and externally synchronize device queues throughout
// construction and destruction. Images/views must outlive the backend and remain GENERAL.
// The model directory must have passed IsInstalled(). No Vulkan loader symbols escape this target.
class Backend {
public:
    Backend(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            std::uint32_t queue_family, const std::filesystem::path& model, VkExtent2D extent,
            VkImage color, VkImageView color_view, VkImageView motion_view);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // Record after DLSS, outside a render pass, on the renderer's graphics queue. No allocation,
    // queue submission, descriptor update or CPU image copy occurs here. Overwrites color last.
    // Returns whether reference exposure changed and invalidated NR history.
    bool Record(VkCommandBuffer commands, const Frame& frame);

    // Rebind same-sized renderer images after finishing their queue. Keeps the model, pipelines
    // and temporal history resident; recorded commands are rebuilt because descriptors changed.
    void Rebind(VkImage color, VkImageView color_view, VkImageView motion_view);

    // Only call after the submitted frame has completed on the GPU.
    [[nodiscard]] bool HadTimeout() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VideoCore::NeuralRendering
