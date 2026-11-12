// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/zstd_compression.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/renderer_vulkan/retained_graphics_stages.h"

namespace {

const std::vector<u32> Spirv{0x07230203, 0x00010000, 0, 1, 0, 0x00010000};
constexpr Vulkan::GraphicsStagePresence Present{true, false, false, false, true};

std::vector<u8> Compress(std::span<const u32> words) {
    return Common::Compression::CompressDataZSTD(reinterpret_cast<const u8*>(words.data()),
                                                words.size_bytes(), 1);
}

Vulkan::CompressedGraphicsStages Stages() {
    Vulkan::CompressedGraphicsStages result;
    result[0] = Compress(Spirv);
    result[4] = Compress(Spirv);
    return result;
}

} // namespace

TEST_CASE("Only successful pipeline jobs can retain stages", "[video_core][retained_stages]") {
    const auto stages = Stages();
    Vulkan::PipelineBuild pending{{.owner_builds = true}, [] {}};
    CHECK_FALSE(Vulkan::CanRetainGraphicsStages(pending.IsBuilt(), pending.Failed(), Present,
                                               stages));
    pending.Build();
    CHECK(Vulkan::CanRetainGraphicsStages(pending.IsBuilt(), pending.Failed(), Present, stages));

    Vulkan::PipelineBuild failed{{}, [] { throw std::runtime_error("driver build failed"); }};
    REQUIRE(failed.IsBuilt());
    REQUIRE(failed.Failed());
    CHECK_FALSE(Vulkan::CanRetainGraphicsStages(failed.IsBuilt(), failed.Failed(), Present, stages));
}

TEST_CASE("Retained stages preserve present and absent shaders", "[video_core][retained_stages]") {
    const auto decoded = Vulkan::DecodeRetainedGraphicsStages(Present, Stages());
    REQUIRE(decoded);
    CHECK((*decoded)[0] == Spirv);
    CHECK((*decoded)[4] == Spirv);
    CHECK((*decoded)[1].empty());
    CHECK((*decoded)[2].empty());
    CHECK((*decoded)[3].empty());
}

TEST_CASE("An incomplete retained pipeline requires translation", "[video_core][retained_stages]") {
    auto stages = Stages();
    SECTION("Compression failed for an expected stage") {
        stages[4].clear();
    }
    SECTION("Unexpected stage") {
        stages[2] = Compress(Spirv);
    }
    CHECK_FALSE(Vulkan::CanRetainGraphicsStages(true, false, Present, stages));
    CHECK_FALSE(Vulkan::DecodeRetainedGraphicsStages(Present, stages));
    CHECK_FALSE(Vulkan::DecodeRetainedGraphicsStages({}, {}));
}

TEST_CASE("Corrupt retained SPIR-V requires translation", "[video_core][retained_stages]") {
    auto stages = Stages();
    SECTION("Truncated zstd frame") {
        stages[4].pop_back();
    }
    SECTION("Invalid zstd frame") {
        stages[4] = {1, 2, 3, 4};
    }
    SECTION("Truncated SPIR-V header") {
        stages[4] = Compress(std::span{Spirv}.first(4));
    }
    SECTION("Wrong SPIR-V magic") {
        auto words = Spirv;
        words[0] = 0;
        stages[4] = Compress(words);
    }
    SECTION("Reserved SPIR-V schema") {
        auto words = Spirv;
        words[4] = 1;
        stages[4] = Compress(words);
    }
    SECTION("Partial SPIR-V word") {
        stages[4] = Common::Compression::CompressDataZSTD(
            reinterpret_cast<const u8*>(Spirv.data()), Spirv.size() * sizeof(u32) - 1, 1);
    }
    SECTION("Oversized advertised frame is rejected before allocation") {
        const u32 size = static_cast<u32>(Vulkan::MaxRetainedSpirvBytes + 4);
        stages[4] = {0x28, 0xb5, 0x2f, 0xfd, 0xa0, static_cast<u8>(size),
                     static_cast<u8>(size >> 8), static_cast<u8>(size >> 16),
                     static_cast<u8>(size >> 24)};
        REQUIRE(ZSTD_getFrameContentSize(stages[4].data(), stages[4].size()) == size);
    }
    SECTION("Frame without a known size") {
        stages[4] = {0x28, 0xb5, 0x2f, 0xfd, 0, 0};
        REQUIRE(ZSTD_getFrameContentSize(stages[4].data(), stages[4].size()) ==
                ZSTD_CONTENTSIZE_UNKNOWN);
    }
    CHECK_FALSE(Vulkan::DecodeRetainedGraphicsStages(Present, stages));
}
