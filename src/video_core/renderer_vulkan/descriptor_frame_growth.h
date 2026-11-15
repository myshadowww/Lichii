// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace Vulkan {

enum class DescriptorChunkAdvance { Existing, Grown, NextFrame, Exhausted };

// Called during draw setup: only an already submitted tick may be waited for.
template <size_t Frames, class Grow, class Wait>
DescriptorChunkAdvance AdvanceDescriptorChunk(
    size_t& frame, size_t& chunk, std::array<size_t, Frames>& counts,
    const std::array<u64, Frames>& ticks, size_t maximum, u64 current_tick,
    Grow&& grow, Wait&& wait) {
    const size_t count = counts[frame];
    if (chunk + 1 < count) {
        ++chunk;
        return DescriptorChunkAdvance::Existing;
    }
    if (count < maximum && grow(frame * maximum + count)) {
        ++counts[frame];
        ++chunk;
        return DescriptorChunkAdvance::Grown;
    }
    const size_t next = (frame + 1) % Frames;
    if (ticks[next] >= current_tick) {
        return DescriptorChunkAdvance::Exhausted;
    }
    wait(ticks[next]);
    frame = next;
    chunk = 0;
    return DescriptorChunkAdvance::NextFrame;
}

} // namespace Vulkan
