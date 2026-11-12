// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon {

/// Frames an image must go unused before the collector deletes it while the device has room. The
/// collector's thresholds sit under what a scene can use: Mario Kart World's races need about
/// 5.5-6.3 GB against a 5 GiB critical threshold, and deleting images unused for 10 frames there
/// made about 42 images again every frame (back thread 25 -> 35 ms per frame).
constexpr u64 MinEvictionAge = 150;

/// Frames since its last use before the collector may delete an image. `mode_age` is the age the
/// collector's mode asks for; it applies as is only when memory is short: an image could not be
/// made, no limit is known (0), or the usage has reached `limit` (the room the driver's budget,
/// its margins and the host's commit leave).
[[nodiscard]] constexpr u64 EvictionAge(u64 mode_age, u64 usage, u64 limit, bool out_of_memory) {
    if (out_of_memory || limit == 0 || usage >= limit) {
        return mode_age;
    }
    return (std::max)(mode_age, MinEvictionAge);
}

/// Diagnostic retention may delay routine collection, but never a pressure pass. Aggressive
/// collection below a known budget still keeps the adaptive floor selected by EvictionAge.
[[nodiscard]] constexpr u64 TextureGcEffectiveMinAge(u64 mode_age, u64 diagnostic_min_age,
                                                     u64 usage, u64 limit, bool out_of_memory,
                                                     bool aggressive_mode) {
    const u64 age = EvictionAge(mode_age, usage, limit, out_of_memory);
    const bool at_limit = limit != 0 && usage >= limit;
    return aggressive_mode || out_of_memory || at_limit ? age
                                                       : (std::max)(age, diagnostic_min_age);
}

/// No image is old enough before the age window has elapsed, including one last used at tick 0.
[[nodiscard]] constexpr std::optional<u64> EvictionCutoff(u64 frame_tick, u64 age) {
    if (frame_tick < age) {
        return std::nullopt;
    }
    return frame_tick - age;
}

} // namespace VideoCommon
