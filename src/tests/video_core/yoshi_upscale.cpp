// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/yoshi_upscale.h"
TEST_CASE("Yoshi gameplay temporal contract rejects mismatched crops and motion", "[video_core]") {
    std::array<u8, 64> motion{};
    std::array<u8, 608> temporal{};
    const auto set = [](auto& data, size_t at, auto value) {
        std::memcpy(data.data() + at, &value, sizeof(value));
    };
    set(motion, 0, -.21875f);
    set(motion, 4, -.42592591f);
    set(motion, 8, 1.f / 960);
    set(motion, 12, 1.f / 540);
    set(motion, 0x18, 960u);
    set(motion, 0x1c, 540u);
    set(motion, 0x20, 960.f);
    set(motion, 0x24, 540.f);
    set(motion, 0x30, 1920.f);
    set(motion, 0x34, 1080.f);
    set(motion, 0x38, 1.f / 1920);
    set(motion, 0x3c, 1.f / 1080);
    constexpr std::array<u32, 12> rect{0, 0, 959, 539, 0, 0, 1919, 1079, 0, 0, 1920, 1080};
    std::memcpy(temporal.data() + 0x1c0, rect.data(), sizeof(rect));
    set(temporal, 0x160, .5f);
    set(temporal, 0x164, .5f);
    set(temporal, 0x170, -.71875f);
    set(temporal, 0x174, -.92592591f);
    set(temporal, 0x188, 1.f);
    set(temporal, 0x18c, 1.f);
    set(temporal, 0x198, 2.f);
    set(temporal, 0x19c, 2.f);
    set(temporal, 0x1f8, 1.f / 960);
    set(temporal, 0x1fc, 1.f / 540);
    const auto good = VideoCommon::YoshiUpscale::Decode(motion, temporal);
    REQUIRE(good);
    CHECK(good->jitter[0] == -.21875f);
    CHECK_FALSE(good->reset);
    for (const size_t offset : {0x160, 0x168, 0x170, 0x188, 0x198, 0x1c0, 0x1d8, 0x1e8}) {
        auto invalid = temporal;
        set(invalid, offset, 3.f);
        CHECK_FALSE(VideoCommon::YoshiUpscale::Decode(motion, invalid));
    }
    auto other = motion;
    set(other, 0x18, 1400u);
    CHECK_FALSE(VideoCommon::YoshiUpscale::Decode(other, temporal));
    set(other, 0x18, 960u);
    set(other, 0, std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(VideoCommon::YoshiUpscale::Decode(other, temporal));
    temporal[0x180] = 1;
    REQUIRE(VideoCommon::YoshiUpscale::Decode(motion, temporal));
    CHECK(VideoCommon::YoshiUpscale::Decode(motion, temporal)->reset);
    CHECK_FALSE(VideoCommon::YoshiUpscale::Decode(std::span{motion}.first(63), temporal));
}
