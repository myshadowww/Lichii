// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>
#include <zstd.h>

#include "common/common_types.h"

namespace Vulkan {

inline constexpr size_t RetainedGraphicsStageCount = 5;
inline constexpr u64 MaxRetainedSpirvBytes = 64 * 1024 * 1024;
using CompressedGraphicsStages = std::array<std::vector<u8>, RetainedGraphicsStageCount>;
using GraphicsStageCode = std::array<std::vector<u32>, RetainedGraphicsStageCount>;
using GraphicsStagePresence = std::array<bool, RetainedGraphicsStageCount>;

inline bool CanRetainGraphicsStages(bool built, bool failed,
                                    const GraphicsStagePresence& present,
                                    const CompressedGraphicsStages& compressed) {
    if (!built || failed) {
        return false;
    }
    bool any = false;
    for (size_t stage = 0; stage < present.size(); ++stage) {
        if (present[stage] == compressed[stage].empty()) {
            return false;
        }
        any |= present[stage];
    }
    return any;
}

inline std::optional<GraphicsStageCode> DecodeRetainedGraphicsStages(
    const GraphicsStagePresence& present, const CompressedGraphicsStages& compressed) {
    if (!CanRetainGraphicsStages(true, false, present, compressed)) {
        return std::nullopt;
    }
    GraphicsStageCode code;
    for (size_t stage = 0; stage < present.size(); ++stage) {
        if (!present[stage]) {
            continue;
        }
        const auto& frame = compressed[stage];
        const u64 size = ZSTD_getFrameContentSize(frame.data(), frame.size());
        if (size < 5 * sizeof(u32) || size > MaxRetainedSpirvBytes || size % sizeof(u32) != 0) {
            return std::nullopt;
        }
        auto& words = code[stage];
        words.resize(size / sizeof(u32));
        if (ZSTD_decompress(words.data(), size, frame.data(), frame.size()) != size ||
            words[0] != 0x07230203 || words[4] != 0) {
            return std::nullopt;
        }
    }
    return code;
}

} // namespace Vulkan
