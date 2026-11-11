// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "video_core/kirby_upscale.h"

namespace {
using Block = std::array<std::byte, 0x210>;
void Put(Block& block, std::size_t offset, float value) {
    std::memcpy(block.data() + offset, &value, sizeof(value));
}
void Origin(Block& block, std::array<std::int32_t, 3> value) {
    std::memcpy(block.data() + 0x200, value.data(), sizeof(value));
}
Block CameraBlock() {
    Block block{};
    // Camera at (13,-2,7), looking towards -world-X. This independently specifies
    // the shader's inverse-view basis and its rotation-only clip transform.
    const std::array<float, 12> inverse_view{0, 0, 1, 13, 0, 1, 0, -2, -1, 0, 0, 7};
    std::memcpy(block.data() + 0x30, inverse_view.data(), sizeof(inverse_view));
    constexpr float near = .1f, far = 20000;
    constexpr float a = near / (far - near), b = near * far / (far - near);
    const std::array<float, 16> p{1.2f, 0, 0, 0, 0, 2.1f, 0, 0, 0, 0, a, b, 0, 0, -1, 0};
    const std::array<float, 16> inv{1 / 1.2f, 0, 0, 0,  0, 1 / 2.1f, 0,     0,
                                    0,        0, 0, -1, 0, 0,        1 / b, a / b};
    const std::array<float, 16> clip{0, 0, -1.2f, 0, 0, 2.1f, 0, 0, a, 0, 0, b, -1, 0, 0, 0};
    std::memcpy(block.data() + 0x60, p.data(), sizeof(p));
    std::memcpy(block.data() + 0xa0, inv.data(), sizeof(inv));
    std::memcpy(block.data() + 0xe0, clip.data(), sizeof(clip));
    Put(block, 0x1e0, 854);
    Put(block, 0x1e4, 480);
    Put(block, 0x1e8, near);
    Put(block, 0x1ec, far);
    Put(block, 0x1f0, 1 / 854.f);
    Put(block, 0x1f4, 1 / 480.f);
    return block;
}
} // namespace

TEST_CASE("Kirby camera reconstructs the native camera-relative projection", "[kirby_upscale]") {
    const auto block = CameraBlock();
    const auto camera = VideoCommon::KirbyUpscale::DecodeCamera(block, {}, 854, 480);
    REQUIRE(camera);
    const std::array<float, 4> point{11, -1, 8, 1};
    std::array<double, 4> clip{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t col = 0; col < 4; ++col)
            clip[row] += camera->view_projection[row * 4 + col] * point[col];
    REQUIRE(std::abs(clip[0] + 1.2) < 2e-6);
    REQUIRE(std::abs(clip[1] - 2.1) < 2e-6);
    REQUIRE(std::abs(clip[3] - 2) < 2e-6);
    REQUIRE(std::abs(clip[2] - (camera->projection[11] - 2 * camera->projection[10])) < 2e-6);
    REQUIRE(std::abs((-.1 * camera->projection[10] + camera->projection[11]) / .1 - 1) < 1e-6);
    REQUIRE(std::abs((-20000 * camera->projection[10] + camera->projection[11]) / 20000) < 1e-9);
}

TEST_CASE("Kirby integer-origin rebasing preserves temporal coordinates", "[kirby_upscale]") {
    auto block = CameraBlock();
    const std::array<std::int32_t, 3> reference{1073741824, -1073741824, 1073741824};
    Origin(block, reference);
    const auto first = VideoCommon::KirbyUpscale::DecodeCamera(block, reference, 854, 480);
    REQUIRE(first);
    Origin(block, {reference[0] + 64, reference[1] - 32, reference[2] + 128});
    Put(block, 0x3c, 13 - 64);
    Put(block, 0x4c, -2 + 32);
    Put(block, 0x5c, 7 - 128);
    const auto rebased = VideoCommon::KirbyUpscale::DecodeCamera(block, reference, 854, 480);
    REQUIRE(rebased);
    REQUIRE(rebased->view == first->view);
    REQUIRE(rebased->view_projection == first->view_projection);
    // Actual movement remains observable after a cell rebase.
    Put(block, 0x3c, 13 - 64 + 2);
    const auto moved = VideoCommon::KirbyUpscale::DecodeCamera(block, reference, 854, 480);
    REQUIRE(moved);
    REQUIRE(std::abs(moved->view[11] - first->view[11] + 2) < 1e-6);
    Origin(block, {std::numeric_limits<std::int32_t>::min(), 0, 0});
    REQUIRE_FALSE(VideoCommon::KirbyUpscale::DecodeCamera(block, reference, 854, 480));
}

TEST_CASE("Kirby camera rejects unproven projection and buffer contracts", "[kirby_upscale]") {
    const auto valid = CameraBlock();
    REQUIRE_FALSE(
        VideoCommon::KirbyUpscale::DecodeCamera(std::span(valid).first(0x20f), {}, 854, 480));
    REQUIRE_FALSE(VideoCommon::KirbyUpscale::DecodeCamera(valid, {}, 864, 480));
    for (const auto [offset, value] : std::array<std::pair<std::size_t, float>, 9>{
             {{0x30, 2},
              {0x68, .001f},
              {0x88, -.000005f},
              {0xa0, 0},
              {0xe0, 1},
              {0x1e8, 1},
              {0x1e8, std::numeric_limits<float>::quiet_NaN()},
              {0x3c, std::numeric_limits<float>::infinity()},
              {0x74, std::numeric_limits<float>::quiet_NaN()}}}) {
        auto invalid = valid;
        Put(invalid, offset, value);
        REQUIRE_FALSE(VideoCommon::KirbyUpscale::DecodeCamera(invalid, {}, 854, 480));
    }
}
