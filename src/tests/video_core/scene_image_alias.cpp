// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <array>
#include "video_core/texture_cache/scene_image_alias.h"

TEST_CASE("Scene aliases require synchronized complete origin copies", "[scene_alias]") {
    using namespace VideoCommon;
    const ImageId producer{7};
    const ImageCopy complete{{0, 0, 1}, {0, 0, 1}, {0, 0, 0}, {0, 0, 0}, {2095, 1179, 1}};
    std::array aliases{AliasedImage{{complete}, producer}};
    const auto matches = [&](u64 source = 42, u64 sampled = 42) {
        return SynchronizedSceneAliasCovers(aliases, producer, source, sampled, {2095, 1179});
    };
    REQUIRE(matches());
    REQUIRE_FALSE(matches(42, 41));
    REQUIRE_FALSE(matches(41, 42));
    REQUIRE_FALSE(matches(0, 0));
    aliases[0].id = ImageId{8};
    REQUIRE_FALSE(matches());
    aliases[0].id = producer;
    auto& copy = aliases[0].copies[0];
    copy.extent.width--;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.extent.height--;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.src_offset.x = 1;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.dst_offset.y = 1;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.src_subresource.base_level = 1;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.dst_subresource.base_layer = 1;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.dst_subresource.num_layers = 2;
    REQUIRE_FALSE(matches());
    copy = complete;
    copy.extent.depth = 2;
    REQUIRE_FALSE(matches());
    aliases[0].copies = {complete};
    aliases[0].copies[0].extent.width = 2096;
    REQUIRE(matches());
}
