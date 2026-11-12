// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"

TEST_CASE("Device cold hints ignore synthetic and overflowing ranges", "[device_memory][cold_hint]") {
    ScopedSystemTestContext context;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager memory{physical};
    constexpr DAddr end = 1ULL << Tegra::MaxwellDeviceTraits::device_virtual_bits;
    constexpr DAddr maximum = std::numeric_limits<DAddr>::max();

    // Kirby's actual race-entry crash: ImageCpuAddress's no-backing sentinel reached HintCold.
    constexpr DAddr captured = 0xffffff8120f27fffULL;
    STATIC_REQUIRE(captured == ~(1ULL << 40) + 0x8120f28000ULL);
    REQUIRE_NOTHROW(memory.HintCold(captured, 0x200000));
    REQUIRE_NOTHROW(memory.HintCold(end, 1));
    REQUIRE_NOTHROW(memory.HintCold(end - 1, 2));
    REQUIRE_NOTHROW(memory.HintCold(maximum - 0x1000, 0x2000));
    REQUIRE_NOTHROW(memory.HintCold(0, maximum));
    REQUIRE_NOTHROW(memory.HintCold(1, maximum));
    REQUIRE_NOTHROW(memory.HintCold(maximum, 0));

    // Valid low and last-page ranges remain legal; these pages simply have no mapping yet.
    REQUIRE_NOTHROW(memory.HintCold(0, 1));
    REQUIRE_NOTHROW(memory.HintCold(0, 0));
    REQUIRE_NOTHROW(memory.HintCold(1, 0));
    REQUIRE_NOTHROW(memory.HintCold(0, 0x1000));
    REQUIRE_NOTHROW(memory.HintCold(end - 1, 1));
    REQUIRE_NOTHROW(memory.HintCold(end - 0x1000, 0x1000));
}
