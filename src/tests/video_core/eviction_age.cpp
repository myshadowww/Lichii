// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/lru_cache.h"

#include "video_core/texture_cache/eviction_age.h"

namespace {

constexpr u64 MiB = u64{1} << 20;

} // namespace

TEST_CASE("The collector keeps recently used images until the device runs short",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::EvictionAge;
    using VideoCommon::MinEvictionAge;
    // Mario Kart World's race: 6268 MiB in use, past the 5120 MiB critical threshold, under the
    // 7678 MiB budget. Aggressive (10), high priority (25) and normal (50) modes all wait.
    // (The limit here is the budget alone; the cache also takes its margins and host commit off.)
    for (const u64 mode_age : {u64{10}, u64{25}, u64{50}}) {
        REQUIRE(EvictionAge(mode_age, 6268 * MiB, 7678 * MiB, false) == MinEvictionAge);
    }
    REQUIRE(EvictionAge(600, 6268 * MiB, 7678 * MiB, false) == 600);
    // At the limit, after a failed image, or with no limit to compare: the mode's own age.
    REQUIRE(EvictionAge(10, 7678 * MiB, 7678 * MiB, false) == 10);
    REQUIRE(EvictionAge(10, 7900 * MiB, 7678 * MiB, false) == 10);
    REQUIRE(EvictionAge(25, 6268 * MiB, 7678 * MiB, true) == 25);
    REQUIRE(EvictionAge(50, 6268 * MiB, 0, false) == 50);
}

TEST_CASE("Texture GC diagnostic retention yields to every pressure path",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::TextureGcEffectiveMinAge;
    constexpr u64 limit = 7678 * MiB;
    constexpr u64 usage = 6268 * MiB;
    for (const u64 diagnostic : {u64{600}, u64{9000}, (std::numeric_limits<u64>::max)()}) {
        // Routine collection still honors diagnostics, with and without a device budget.
        CHECK(TextureGcEffectiveMinAge(50, diagnostic, usage, limit, false, false) == diagnostic);
        CHECK(TextureGcEffectiveMinAge(25, diagnostic, usage, 0, false, false) == diagnostic);
        // A capped critical threshold can request aggressive collection with budget to spare.
        // Keep #1435's adaptive floor, but drop the diagnostic extension.
        CHECK(TextureGcEffectiveMinAge(10, diagnostic, usage, limit, false, true) == 150);
        CHECK(TextureGcEffectiveMinAge(10, diagnostic, usage, 0, false, true) == 10);
        // The first, non-aggressive pass must already ignore diagnostics after allocation failure.
        CHECK(TextureGcEffectiveMinAge(25, diagnostic, usage, limit, true, false) == 25);
        CHECK(TextureGcEffectiveMinAge(10, diagnostic, usage, limit, true, true) == 10);
        // Exact budget equality and one byte over are pressure even in a non-aggressive pass.
        CHECK(TextureGcEffectiveMinAge(25, diagnostic, limit, limit, false, false) == 25);
        CHECK(TextureGcEffectiveMinAge(50, diagnostic, limit + 1, limit, false, false) == 50);
        CHECK(TextureGcEffectiveMinAge(25, diagnostic, limit - 1, limit, false, false) == diagnostic);
    }
    CHECK(TextureGcEffectiveMinAge(50, 0, usage, limit, false, false) == 150);
    CHECK(TextureGcEffectiveMinAge(25, 20, usage, limit, false, false) == 150);
    CHECK(TextureGcEffectiveMinAge(50, 20, usage, 0, false, false) == 50);
}

TEST_CASE("Texture GC age cutoff skips early frames and includes the exact age boundary",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::EvictionCutoff;
    struct Traits {
        using ObjectType = u32;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<Traits> images;
    images.Insert(1, 0);
    images.Insert(2, 1);
    const auto candidates = [&](u64 frame, u64 age) {
        std::vector<u32> result;
        if (const auto cutoff = EvictionCutoff(frame, age)) {
            images.ForEachItemBelow(*cutoff, [&](u32 image) { result.push_back(image); });
        }
        return result;
    };
    for (const u64 age : {u64{10}, u64{25}, u64{50}, u64{150}, u64{600}}) {
        CHECK(candidates(0, age).empty());
        CHECK(candidates(age - 1, age).empty());
        CHECK(candidates(age, age) == std::vector<u32>{1});
        CHECK(candidates(age + 1, age) == std::vector<u32>{1, 2});
    }
    CHECK(candidates(0, 0) == std::vector<u32>{1});
    CHECK_FALSE(EvictionCutoff(150, (std::numeric_limits<u64>::max)()).has_value());
}
