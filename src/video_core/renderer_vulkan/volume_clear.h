// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"
#include "video_core/surface.h"

namespace Vulkan {

inline bool CanUseZeroVolumeClear(VideoCore::Surface::PixelFormat format,
                                  const std::array<f32, 4>& clear_color) {
    // The normal signed-integer clear maps a zero register to a negative value, not zero bytes.
    return !VideoCore::Surface::IsPixelFormatSignedInteger(format) &&
           std::ranges::all_of(clear_color, [](f32 value) { return value == 0.0f; });
}

} // namespace Vulkan
