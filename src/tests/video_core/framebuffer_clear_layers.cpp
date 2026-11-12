// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/framebuffer_clear_layers.h"
#include "video_core/renderer_vulkan/volume_clear.h"

TEST_CASE("Framebuffer clears stay within the smallest attachment",
          "[video_core][framebuffer_layers]") {
    const auto clear = Vulkan::GetFramebufferClearLayers(0, 4, 1);
    REQUIRE(clear.count == 1);
    REQUIRE(clear.covers_all_layers);
}

TEST_CASE("Framebuffer clears skip empty and nonoverlapping layer ranges",
          "[video_core][framebuffer_layers]") {
    for (const u32 base : {0U, 1U, 4U, (std::numeric_limits<u32>::max)()}) {
        const auto empty = Vulkan::GetFramebufferClearLayers(base, 0, 1);
        REQUIRE(empty.count == 0);
        REQUIRE_FALSE(empty.covers_all_layers);
        if (base != 0) {
            const auto outside = Vulkan::GetFramebufferClearLayers(base, 4, 1);
            REQUIRE(outside.count == 0);
            REQUIRE_FALSE(outside.covers_all_layers);
        }
    }
}

TEST_CASE("Framebuffer clear clipping preserves the base and avoids overflow",
          "[video_core][framebuffer_layers]") {
    const auto tail = Vulkan::GetFramebufferClearLayers(2, 4, 4);
    REQUIRE(tail.count == 2);
    REQUIRE_FALSE(tail.covers_all_layers);

    const auto huge = Vulkan::GetFramebufferClearLayers(1, (std::numeric_limits<u32>::max)(), 4);
    REQUIRE(huge.count == 3);
    REQUIRE_FALSE(huge.covers_all_layers);
}

TEST_CASE("Only clears covering every framebuffer layer can become load-op clears",
          "[video_core][framebuffer_layers]") {
    const auto partial = Vulkan::GetFramebufferClearLayers(0, 1, 4);
    REQUIRE(partial.count == 1);
    REQUIRE_FALSE(partial.covers_all_layers);

    const auto full = Vulkan::GetFramebufferClearLayers(0, 4, 4);
    REQUIRE(full.count == 4);
    REQUIRE(full.covers_all_layers);

    const auto single = Vulkan::GetFramebufferClearLayers(0, 1, 1);
    REQUIRE(single.count == 1);
    REQUIRE(single.covers_all_layers);
}

TEST_CASE("Volume byte clears preserve the normal clear's format conversion",
          "[video_core][volume_clear]") {
    using namespace VideoCore::Surface;
    const std::array<f32, 4> zero{};
    for (const auto format : {PixelFormat::A8B8G8R8_SINT, PixelFormat::R8_SINT,
                              PixelFormat::R16G16B16A16_SINT, PixelFormat::R32G32B32A32_SINT,
                              PixelFormat::R32G32_SINT, PixelFormat::R16_SINT,
                              PixelFormat::R16G16_SINT, PixelFormat::R8G8_SINT,
                              PixelFormat::R32_SINT}) {
        // Match the regular clear conversion: zero registers must not use a zero-byte copy.
        const auto bits = PixelComponentSizeBitsInteger(format);
        const auto normal_clear = s32(f32(s64(bits - 1) << 1) * (zero[0] - 0.5f));
        REQUIRE(normal_clear != 0);
        REQUIRE_FALSE(Vulkan::CanUseZeroVolumeClear(format, zero));
    }
    for (const auto format : {PixelFormat::B10G11R11_FLOAT, PixelFormat::R16G16B16A16_FLOAT,
                              PixelFormat::A8B8G8R8_UNORM, PixelFormat::R8_SNORM,
                              PixelFormat::A8B8G8R8_SRGB, PixelFormat::R32_UINT,
                              PixelFormat::A8B8G8R8_UINT}) {
        REQUIRE(Vulkan::CanUseZeroVolumeClear(format, zero));
        REQUIRE(Vulkan::CanUseZeroVolumeClear(format, {-0.0f, 0.0f, -0.0f, 0.0f}));
        for (size_t channel = 0; channel < zero.size(); ++channel) {
            auto nonzero = zero;
            nonzero[channel] = 1.0f;
            REQUIRE_FALSE(Vulkan::CanUseZeroVolumeClear(format, nonzero));
            nonzero[channel] = std::numeric_limits<f32>::quiet_NaN();
            REQUIRE_FALSE(Vulkan::CanUseZeroVolumeClear(format, nonzero));
        }
    }
}
