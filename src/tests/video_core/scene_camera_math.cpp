// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/scene_camera_math.h"

TEST_CASE("Scene focal animation preserves aspect and the remaining projection", "[scene_camera]") {
    const std::array<float, 16> projection{1, 0, 0, 0, 0, 2, 0, 0, 0, 0, .001f, .1f, 0, 0, -1, 0};
    auto zoom = projection;
    zoom[0] = 1.25f;
    zoom[5] = 2.5f;
    REQUIRE(Vulkan::SceneFocalScaleCompatible(zoom, projection));
    REQUIRE(Vulkan::SceneFocalScaleCompatible(projection, zoom));
    zoom[5] = 2.6f;
    REQUIRE_FALSE(Vulkan::SceneFocalScaleCompatible(zoom, projection));
    zoom[5] = 2.5f;
    for (const auto index : {2, 6, 10, 11, 14, 15}) {
        auto changed = zoom;
        changed[index] += .001f;
        REQUIRE_FALSE(Vulkan::SceneFocalScaleCompatible(changed, projection));
    }
    for (const float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()}) {
        auto changed = zoom;
        changed[0] = invalid;
        REQUIRE_FALSE(Vulkan::SceneFocalScaleCompatible(changed, projection));
        REQUIRE_FALSE(Vulkan::SceneFocalScaleCompatible(projection, changed));
    }
    auto tiny = projection;
    tiny[11] += 5e-5f;
    REQUIRE(Vulkan::SceneFocalScaleCompatible(tiny, projection));
    auto not_finite = projection;
    not_finite[8] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(Vulkan::SceneFocalScaleCompatible(not_finite, projection));
}

TEST_CASE("Scene camera cuts use world position rather than view translation", "[scene_camera]") {
    const std::array<float, 12> straight{1, 0, 0, -16000, 0, 1, 0, -4000, 0, 0, 1, 3000};
    const std::array<float, 12> rotated{0, 0, -1, -3000, 0, 1, 0, -4000, 1, 0, 0, -16000};
    const auto position = Vulkan::SceneCameraPosition(straight);
    REQUIRE(position == std::array<double, 3>{16000, 4000, -3000});
    REQUIRE(Vulkan::SceneCameraPosition(rotated) == position);
    auto moved = rotated;
    moved[11] -= 100;
    REQUIRE(Vulkan::SceneCameraPosition(moved) == std::array<double, 3>{16100, 4000, -3000});
    const float s = std::sqrt(0.5f);
    const std::array<float, 12> diagonal{s, 0, -s, -19000 * s, 0, 1, 0, -4000, s, 0, s, -13000 * s};
    const auto diagonal_position = Vulkan::SceneCameraPosition(diagonal);
    for (size_t axis = 0; axis < 3; ++axis)
        REQUIRE(std::abs(diagonal_position[axis] - position[axis]) < 0.002);
}
