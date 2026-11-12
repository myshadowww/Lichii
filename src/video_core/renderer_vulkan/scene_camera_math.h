// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Vulkan {
// A title may animate focal scale without changing the projection's aspect or depth mapping.
// Reprojection still uses the complete current and previous matrices to model that zoom.
inline bool SceneFocalScaleCompatible(const std::array<float, 16>& current,
                                      const std::array<float, 16>& previous) {
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (!std::isfinite(current[i]) || !std::isfinite(previous[i]) ||
            (i != 0 && i != 5 && std::abs(current[i] - previous[i]) > 1e-4f))
            return false;
    }
    if (current[0] <= 0 || current[5] <= 0 || previous[0] <= 0 || previous[5] <= 0)
        return false;
    const double a = static_cast<double>(current[0]) * previous[5];
    const double b = static_cast<double>(previous[0]) * current[5];
    return std::abs(a - b) <= 1e-5 * std::max(std::abs(a), std::abs(b));
}

// A rigid world-to-view matrix stores t = -R * camera_position. Comparing t directly
// mistakes rotation around a stationary camera for translation, especially far from origin.
inline std::array<double, 3> SceneCameraPosition(const std::array<float, 12>& view) {
    std::array<double, 3> position{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t row = 0; row < 3; ++row) {
            position[axis] -= static_cast<double>(view[row * 4 + axis]) * view[row * 4 + 3];
        }
    }
    return position;
}
} // namespace Vulkan
