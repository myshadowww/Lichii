// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include "common/common_types.h"

namespace VideoCommon {

[[nodiscard]] constexpr bool IsNvnGraphicsStorageDescriptor(size_t stage, u32 cbuf_index,
                                                            u32 cbuf_offset, u32 indirect_offset) {
    const u32 base = 0x110 + static_cast<u32>(stage) * 0x100;
    return stage < 5 && cbuf_index == 0 && indirect_offset == ~0U && cbuf_offset >= base &&
           cbuf_offset < base + 0x100 && (cbuf_offset - base) % 0x10 == 0;
}

} // namespace VideoCommon
