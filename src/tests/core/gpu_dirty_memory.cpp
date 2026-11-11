// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/gpu_dirty_memory_manager.h"

TEST_CASE("GPUDirtyMemoryManager reports collected writes until they are gathered",
          "[core][gpu_dirty_memory]") {
    Core::GPUDirtyMemoryManager manager;
    manager.Collect(0x10040, 0x10);
    manager.Collect(0x20000, 0x40);

    REQUIRE(manager.Overlaps(0x10040, 1));
    REQUIRE(manager.Overlaps(0x10000, 0x1000));
    REQUIRE(manager.Overlaps(0x20000, 4));
    REQUIRE_FALSE(manager.Overlaps(0x10000, 0x40));
    REQUIRE_FALSE(manager.Overlaps(0x10080, 0x40));
    REQUIRE_FALSE(manager.Overlaps(0x30000, 0x1000));

    std::vector<std::pair<PAddr, size_t>> gathered;
    std::function<void(PAddr, size_t)> callback = [&](PAddr address, size_t size) {
        gathered.emplace_back(address, size);
    };
    manager.Gather(callback);

    REQUIRE(gathered.size() == 2);
    REQUIRE_FALSE(manager.Overlaps(0x10000, 0x1000));
    REQUIRE_FALSE(manager.Overlaps(0x20000, 4));
}

namespace {
using DirtyRange = std::pair<PAddr, size_t>;

bool Contains(const std::vector<DirtyRange>& ranges, PAddr address) {
    return std::ranges::any_of(ranges, [address](const auto& range) {
        return address >= range.first && address - range.first < range.second;
    });
}

void CheckDirtyCoverage(PAddr address, size_t size) {
    Core::GPUDirtyMemoryManager manager;
    manager.Collect(address, size);
    std::vector<DirtyRange> pending;
    manager.ForEachPending([&](PAddr start, size_t length) { pending.emplace_back(start, length); });
    const PAddr first = address & ~PAddr{63};
    const PAddr end = (address + size + 63) & ~PAddr{63};
    REQUIRE_FALSE(pending.empty());
    for (const auto& [start, length] : pending) {
        REQUIRE(length != 0);
        REQUIRE(start >= first);
        REQUIRE(start + length <= end);
        REQUIRE(start % 64 == 0);
        REQUIRE(length % 64 == 0);
    }
    for (PAddr block = first; block < end; block += 64) {
        REQUIRE(Contains(pending, block));
        REQUIRE(Contains(pending, block + 63));
        REQUIRE(manager.Overlaps(block, 1));
        REQUIRE(manager.Overlaps(block + 63, 1));
    }
    REQUIRE_FALSE(manager.Overlaps(first - 1, 1));
    REQUIRE_FALSE(manager.Overlaps(end, 1));
    REQUIRE_FALSE(manager.Overlaps(address, 0));

    std::vector<DirtyRange> gathered;
    std::function<void(PAddr, size_t)> callback = [&](PAddr start, size_t length) {
        gathered.emplace_back(start, length);
    };
    manager.Gather(callback);
    REQUIRE(gathered == pending);
    pending.clear();
    manager.ForEachPending([&](PAddr start, size_t length) { pending.emplace_back(start, length); });
    REQUIRE(pending.empty());
    REQUIRE_FALSE(manager.Overlaps(first, end - first));
    gathered.clear();
    manager.Gather(callback);
    REQUIRE(gathered.empty());
}
} // namespace

TEST_CASE("GPU dirty ranges preserve every block across tracking windows",
          "[core][gpu_dirty_memory]") {
    // Full guest pages, both complete tracking halves, tiny boundary crossings, unaligned
    // partial pages and writes spanning several windows all retain their full coverage.
    for (const auto& [address, size] : std::vector<DirtyRange>{
             {0x100000, 0x1000}, {0x100000, 0x800}, {0x100800, 0x800},
             {0x1007f0, 0x20}, {0x1007ff, 2}, {0x100123, 0xabc},
             {0x1007f0, 0x810}, {0x100000, 0xc00}, {0x1007fd, 0x3181}}) {
        CheckDirtyCoverage(address, size);
    }
}

TEST_CASE("Empty and overflowing dirty ranges do not alter pending writes",
          "[core][gpu_dirty_memory]") {
    Core::GPUDirtyMemoryManager manager;
    manager.Collect(0x1007c0, 64);
    std::vector<DirtyRange> expected{{0x1007c0, 64}};
    constexpr PAddr maximum = std::numeric_limits<PAddr>::max();
    manager.Collect(0x1007f0, 0);
    manager.Collect(maximum, 0);
    manager.Collect(maximum - 7, 16);
    manager.Collect(1, std::numeric_limits<size_t>::max());
    REQUIRE_FALSE(manager.Overlaps(0x1007f0, 0));
    REQUIRE_FALSE(manager.Overlaps(maximum - 7, 16));
    REQUIRE_FALSE(manager.Overlaps(1, std::numeric_limits<size_t>::max()));
    REQUIRE(manager.Overlaps(0x1007ff, 1));
    std::vector<DirtyRange> gathered;
    std::function<void(PAddr, size_t)> callback = [&](PAddr start, size_t size) {
        gathered.emplace_back(start, size);
    };
    manager.Gather(callback);
    REQUIRE(gathered == expected);
}
