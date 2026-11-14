// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include "common/common_types.h"

namespace VideoCommon {

// The last field used is the previous view-projection matrix at 0x180, ending at 0x1c0.
inline constexpr size_t OcarinaCameraBytes = 0x1c0;
using OcarinaCameraBuffer = std::array<f32, OcarinaCameraBytes / sizeof(f32)>;

enum class OcarinaCameraIssue { None, ShortBuffer, Extent, Projection };

struct OcarinaCamera {
    OcarinaCameraIssue issue{OcarinaCameraIssue::ShortBuffer};
    u32 width{}, height{};
    std::array<f32, 16> current{}, previous{};
};

inline OcarinaCamera DecodeOcarinaCamera(std::span<const f32> values, u32 target_width,
                                        u32 target_height) {
    OcarinaCamera camera;
    if (values.size_bytes() < OcarinaCameraBytes) {
        return camera;
    }
    const f32 width = values[0x120 / sizeof(f32)];
    const f32 height = values[0x124 / sizeof(f32)];
    camera.issue = OcarinaCameraIssue::Extent;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 640.0f || height < 360.0f ||
        static_cast<f64>(width) > target_width || static_cast<f64>(height) > target_height) {
        return camera;
    }
    camera.width = static_cast<u32>(width);
    camera.height = static_cast<u32>(height);
    if (static_cast<f32>(camera.width) != width || static_cast<f32>(camera.height) != height ||
        camera.width != target_width || camera.height != target_height) {
        return camera;
    }
    camera.issue = OcarinaCameraIssue::Projection;
    if (values[0xa8 / sizeof(f32)] != -1.0f || values[0xac / sizeof(f32)] != 0.0f) {
        return camera;
    }
    std::copy_n(values.begin() + 0x30 / sizeof(f32), 16, camera.current.begin());
    std::copy_n(values.begin() + 0x180 / sizeof(f32), 16, camera.previous.begin());
    camera.issue = OcarinaCameraIssue::None;
    return camera;
}

} // namespace VideoCommon
