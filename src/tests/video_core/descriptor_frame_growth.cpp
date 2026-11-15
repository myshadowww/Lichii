// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/descriptor_frame_growth.h"

using Vulkan::AdvanceDescriptorChunk;
using Vulkan::DescriptorChunkAdvance;

TEST_CASE("Descriptor frames grow without recycling pending draw bindings",
          "[descriptor_frame]") {
    constexpr size_t frames = 8;
    constexpr size_t maximum = 4;
    std::array<size_t, frames> counts;
    counts.fill(1);
    std::array<u64, frames> ticks{};
    std::array<u64, frames * maximum> last_use{};
    size_t frame = 0;
    size_t chunk = 0;
    u64 tick = 1;
    unsigned allocations = 0;
    unsigned waits = 0;
    const auto grow = [&](size_t index) {
        REQUIRE(index == frame * maximum + counts[frame]);
        REQUIRE(last_use[index] == 0);
        ++allocations;
        return true;
    };
    const auto wait = [&](u64 submitted) {
        REQUIRE(submitted < tick);
        ++waits;
    };
    // Fill every slot in each command buffer, then submit between draws. Chunk indices remain
    // stable across growth and repeated wraps; no active command buffer's bytes are reused.
    for (unsigned submission = 0; submission < 100; ++submission) {
        do {
            const size_t index = frame * maximum + chunk;
            REQUIRE(last_use[index] < tick);
            last_use[index] = tick;
            ticks[frame] = tick;
            const auto previous_frame = frame;
            const auto previous_chunk = chunk;
            const auto result = AdvanceDescriptorChunk(frame, chunk, counts, ticks, maximum,
                                                       tick, grow, wait);
            if (result == DescriptorChunkAdvance::Exhausted) {
                REQUIRE(frame == previous_frame);
                REQUIRE(chunk == previous_chunk);
                REQUIRE(ticks[(frame + 1) % frames] == tick);
                break;
            }
        } while (true);
        ++tick;
        REQUIRE(AdvanceDescriptorChunk(frame, chunk, counts, ticks, maximum, tick, grow, wait) ==
                DescriptorChunkAdvance::NextFrame);
    }
    REQUIRE(allocations == frames * (maximum - 1));
    REQUIRE(waits == frames * 100);
}

TEST_CASE("Descriptor allocation failure advances only to a submitted slot", "[descriptor_frame]") {
    std::array<size_t, 2> counts{1, 1};
    std::array<u64, 2> ticks{7, 6};
    size_t frame = 0;
    size_t chunk = 0;
    std::vector<u64> waited;
    auto fail = [](size_t) { return false; };
    auto wait = [&](u64 tick) { waited.push_back(tick); };
    REQUIRE(AdvanceDescriptorChunk(frame, chunk, counts, ticks, 4, 7, fail, wait) ==
            DescriptorChunkAdvance::NextFrame);
    REQUIRE(frame == 1);
    REQUIRE(counts == std::array<size_t, 2>{1, 1});
    REQUIRE(waited == std::vector<u64>{6});
    ticks[1] = 7;
    REQUIRE(AdvanceDescriptorChunk(frame, chunk, counts, ticks, 4, 7, fail, wait) ==
            DescriptorChunkAdvance::Exhausted);
    REQUIRE(frame == 1);
    REQUIRE(chunk == 0);
    REQUIRE(waited == std::vector<u64>{6});
}
