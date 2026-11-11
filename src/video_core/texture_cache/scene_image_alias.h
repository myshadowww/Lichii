// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include "video_core/texture_cache/image_base.h"

namespace VideoCommon {

// SynchronizeAliases records these copies before giving both images the same modification tick.
inline bool SynchronizedSceneAliasCovers(std::span<const AliasedImage> aliases, ImageId producer,
                                       u64 producer_tick, u64 sampled_tick, Extent2D crop) {
    if (!producer || producer_tick == 0 || producer_tick != sampled_tick || crop.width == 0 ||
        crop.height == 0)
        return false;
    return std::ranges::any_of(aliases, [&](const AliasedImage& alias) {
        return alias.id == producer &&
               std::ranges::any_of(alias.copies, [&](const ImageCopy& copy) {
                   return copy.src_subresource.base_level == 0 &&
                          copy.dst_subresource.base_level == 0 &&
                          copy.src_subresource.base_layer == 0 &&
                          copy.dst_subresource.base_layer == 0 &&
                          copy.src_subresource.num_layers == 1 &&
                          copy.dst_subresource.num_layers == 1 && copy.src_offset.x == 0 &&
                          copy.src_offset.y == 0 && copy.src_offset.z == 0 &&
                          copy.dst_offset.x == 0 && copy.dst_offset.y == 0 &&
                          copy.dst_offset.z == 0 && copy.extent.width >= crop.width &&
                          copy.extent.height >= crop.height && copy.extent.depth == 1;
               });
    });
}

} // namespace VideoCommon
