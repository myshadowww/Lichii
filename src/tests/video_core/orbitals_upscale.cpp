// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/orbitals_upscale.h"

namespace {
auto CameraConstants(unsigned player) {
    std::array<float, VideoCommon::OrbitalsUpscale::CameraBytes / 4> data{};
    const auto matrix = [&](std::size_t offset, const std::array<float, 16>& row_major) {
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t col = 0; col < 4; ++col)
                data[offset / 4 + col * 4 + row] = row_major[row * 4 + col];
    };
    matrix(0xc0, {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1});
    for (const auto offset : {0x1c0, 0x200, 0x560})
        matrix(offset, {2, 0, 0, 0, 0, 1.8f, 0, 0, 0, 0, 0, 10, 0, 0, 1, 0});
    matrix(0x3c0, {0, 2, 0, 400, 0, 0, 1.8f, -90, 0, 0, 0, 10, 1, 0, 0, -1000});
    data[0x480 / 4] = 1000;
    data[0x484 / 4] = -200;
    data[0x488 / 4] = 50;
    data[0x4f0 / 4] = 0.25f;
    data[0x4f4 / 4] = -0.5f;
    data[0x4f8 / 4] = 0.5f;
    data[0x4fc / 4] = player ? 0.75f : 0.25f;
    return data;
}
} // namespace

TEST_CASE("Orbitals local players retain distinct camera spaces", "[orbitals_upscale]") {
    using VideoCommon::OrbitalsUpscale::DecodeCamera;
    for (unsigned player : {0, 1}) {
        const auto data = CameraConstants(player);
        const auto camera = DecodeCamera(data, player);
        REQUIRE(camera);
        REQUIRE_FALSE(DecodeCamera(data, 1 - player));
        const std::array<float, 4> point{1100, -190, 60, 1};
        std::array<float, 4> clip{};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t col = 0; col < 4; ++col)
                clip[row] += camera->view_projection[row * 4 + col] * point[col];
        CHECK(std::abs((clip[0] / clip[3] * 0.5f + 0.5f) * 960 - 576) < 0.001f);
        CHECK(std::abs((0.5f - clip[1] / clip[3] * 0.5f) * 1080 - 442.8f) < 0.001f);
        CHECK(std::abs(clip[2] / clip[3] - 0.1f) < 1e-6f);
        CHECK(camera->projection[11] / 10 == 1);
        CHECK(camera->projection[11] / 1000000 < 0.000011f);
        CHECK(camera->view[3] == 200);
        CHECK(camera->view[7] == -50);
        CHECK(camera->view[11] == -1000);
    }
}

TEST_CASE("Orbitals rejects mismatched camera and projection inputs", "[orbitals_upscale]") {
    using VideoCommon::OrbitalsUpscale::DecodeCamera;
    auto data = CameraConstants(0);
    SECTION("short") {
        REQUIRE_FALSE(DecodeCamera(std::span{data}.first(data.size() - 1), 0));
    }
    SECTION("jittered") {
        data[0x200 / 4 + 8] = 0.001f;
        REQUIRE_FALSE(DecodeCamera(data, 0));
    }
    SECTION("nonfinite origin") {
        data[0x480 / 4] = std::numeric_limits<float>::infinity();
        REQUIRE_FALSE(DecodeCamera(data, 0));
    }
    SECTION("different absolute camera") {
        data[0x480 / 4] += 1;
        REQUIRE_FALSE(DecodeCamera(data, 0));
    }
    SECTION("scaled view") {
        data[0xc0 / 4 + 2] *= 2;
        REQUIRE_FALSE(DecodeCamera(data, 0));
    }
    SECTION("unsupported player") {
        REQUIRE_FALSE(DecodeCamera(data, 2));
    }
}
