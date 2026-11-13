// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "common/literals.h"
#include "common/memory_detect.h"

using namespace Common::Literals;
TEST_CASE("Cold hints only when the host is short of RAM", "[common]") {
    using Common::IsLowOnMemory;
    CHECK_FALSE(IsLowOnMemory(0, 32_GiB)); // unknown
    CHECK_FALSE(IsLowOnMemory(18_GiB, 32_GiB));
    CHECK_FALSE(IsLowOnMemory(5_GiB, 32_GiB));
    CHECK(IsLowOnMemory(3_GiB, 32_GiB));
    CHECK_FALSE(IsLowOnMemory(5_GiB, 16_GiB));
    CHECK(IsLowOnMemory(3_GiB, 16_GiB));
    CHECK(IsLowOnMemory(1_GiB, 8_GiB));
    CHECK_FALSE(IsLowOnMemory(2_GiB + 1, 8_GiB));
}
