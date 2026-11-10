// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

TEST_CASE("NX2 presentation resolves parent handles and image offsets", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0x1000, 0x8000, 0x4000, 0x3000);
    registry.RecordDeviceRange(0x4000, 0x3000, 0x10000);
    REQUIRE(registry.GpuAddressOf(7, 0x1800) == 0x8800U);
    REQUIRE(registry.DeviceAddressOfGpu(0x8800) == 0x10800U);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0xfff));
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0x4000));
    REQUIRE_FALSE(registry.GpuAddressOf(8, 0x1800));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0xb000));
}

TEST_CASE("NX2 partial unmaps preserve remaining parent offsets", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0x1000, 0x8000, 0x4000, 0x3000);
    registry.RecordDeviceRange(0x4000, 0x3000, 0x10000);
    registry.Unmap(0x9000, 0x1000);
    REQUIRE(registry.GpuAddressOf(7, 0x1000) == 0x8000U);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0x2000));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0x9000));
    REQUIRE(registry.GpuAddressOf(7, 0x3000) == 0xa000U);
    REQUIRE(registry.DeviceAddressOfGpu(0xa000) == 0x12000U);
}

TEST_CASE("NX2 remaps and resets discard stale parent handles", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0, 0x8000, 0x4000, 0x1000);
    registry.MapAt(8, 0x1000, 0x8000, 0x6000, 0x1000);
    registry.RecordDeviceRange(0x6000, 0x1000, 0x20000);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0));
    REQUIRE(registry.GpuAddressOf(8, 0x1000) == 0x8000U);
    REQUIRE(registry.DeviceAddressOfGpu(0x8000) == 0x20000U);
    registry.Reset();
    REQUIRE_FALSE(registry.GpuAddressOf(8, 0x1000));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0x8000));
}

TEST_CASE("NX2 destroyed memory leaves no GPU range mapped into it", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    std::vector<std::pair<u64, u64>> unmapped;
    registry.AddListener(
        &unmapped, [](u64, u64, u64) {},
        [&unmapped](u64 gpu_address, u64 size) { unmapped.emplace_back(gpu_address, size); });
    registry.Register(0x10000, 0x4000);
    registry.MapAt(5, 0x1000, 0x80000, 0x11000, 0x2000);
    registry.MapAt(6, 0, 0x90000, 0x20000, 0x1000);
    registry.MapAt(7, 0, 0xa0000, 0x13000, 0x3000);

    const auto leftovers = registry.UnmapMemory(0x10000, 0x4000);

    const std::vector<std::pair<u64, u64>> expected_leftovers{{0x80000, 0x2000}, {0xa0000, 0x1000}};
    REQUIRE(leftovers == expected_leftovers);
    const std::vector<std::pair<u64, u64>> expected_unmapped{
        {0x10000, 0x4000}, {0x80000, 0x2000}, {0xa0000, 0x1000}};
    REQUIRE(unmapped == expected_unmapped);
    REQUIRE_FALSE(registry.GpuAddressOf(5, 0x1000));
    REQUIRE(registry.GpuAddressOf(6, 0) == 0x90000U);
    // Only the part of a mapping that reached the destroyed memory goes.
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0));
    REQUIRE(registry.GpuAddressOf(7, 0x1000) == 0xa1000U);
}

TEST_CASE("NX2 reused device backing follows CPU pages through every GPU alias",
          "[video_core][nx2_mapping][nx2_stale_backing]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(24, 12);

    constexpr u64 Page = Core::Memory::YUZU_PAGESIZE;
    constexpr u64 Cpu = 0x10000;
    constexpr DAddr Device = 0x20000;
    constexpr DAddr OldAlias = 0x40000;
    constexpr GPUVAddr Gpu = 0x100000;
    constexpr GPUVAddr GpuAlias = 0x800000;
    auto* old_pages = system.DeviceMemory().GetPointerFromRaw<u8>(0x10000);
    auto* new_page = system.DeviceMemory().GetPointerFromRaw<u8>(0x30000);
    std::memset(old_pages, 0x11, Page);
    std::memset(old_pages + Page, 0x22, Page);
    std::memset(old_pages + 2 * Page, 0x33, Page);
    std::memset(new_page, 0xbb, Page);
    const auto set_page = [&](u64 offset, u8* backing) {
        const u64 address = Cpu + offset;
        table.entries.GetAndFault(address / Page)
            .Store(false, backing ? Common::PageType::Memory : Common::PageType::Unmapped, 1,
                   backing ? reinterpret_cast<uintptr_t>(backing) - address : 0);
    };
    for (u64 offset = 0; offset < 3 * Page; offset += Page) {
        set_page(offset, old_pages + offset);
    }
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    Tegra::MaxwellDeviceMemoryManager device{system.DeviceMemory()};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    device.BindInterface(&rasterizer);
    const Core::Asid asid = device.RegisterProcess(&memory);
    device.Map(Device, Cpu, 3 * Page, asid, true);
    // Another device range still owns the old physical page: refreshing must not remove its
    // reverse mapping or redirect it along with the reused range.
    device.Map(OldAlias, Cpu + Page, Page, asid, true);
    SCOPE_EXIT {
        device.Unmap(Device, 3 * Page);
        device.Unmap(OldAlias, Page);
        device.UnregisterProcess(asid);
    };
    Tegra::MemoryManager first{system, device, 32, 1ULL << 31, 16};
    Tegra::MemoryManager second{system, device, 32, 1ULL << 31, 16};
    first.BindRasterizer(&rasterizer);
    second.BindRasterizer(&rasterizer);
    first.Map(Gpu, Device, 3 * Page, Tegra::PTEKind::PITCH, false);
    second.Map(GpuAlias, Device, 3 * Page, Tegra::PTEKind::PITCH, false);
    const auto refresh = [&] {
        Service::Nvidia::Devices::Nx2RefreshBacking(device, memory, asid, Device, Cpu, 3 * Page);
    };
    const auto reverse_mappings = [&](u8* backing) {
        Common::ScratchBuffer<u32> scratch;
        std::vector<DAddr> result;
        device.ApplyOpOnPointer(backing, scratch,
                                [&](DAddr address) { result.push_back(address); });
        std::ranges::sort(result);
        return result;
    };

    // Re-registering unchanged pages must preserve caches and translation generations.
    const u64 unchanged_generation = device.TranslationGeneration();
    refresh();
    REQUIRE(device.TranslationGeneration() == unchanged_generation);
    REQUIRE(first.GetPointer(Gpu + Page) == old_pages + Page);
    REQUIRE(second.GetPointer(GpuAlias + Page) == old_pages + Page);

    SECTION("A changed later page replaces stale geometry and keeps other aliases intact") {
        // Same CPU address and size, different physical page, as after sub 102 and sub 101.
        set_page(Page, new_page);
        REQUIRE(memory.GetPointerSilent(Cpu + Page) == new_page);
        REQUIRE(first.GetPointer(Gpu + Page) == old_pages + Page);
        REQUIRE(*first.GetPointer(Gpu + Page) == 0x22);
        refresh();
        REQUIRE(device.TranslationGeneration() > unchanged_generation);
        REQUIRE(first.GetPointer(Gpu + Page) == new_page);
        REQUIRE(second.GetPointer(GpuAlias + Page) == new_page);
        REQUIRE(first.GpuToCpuAddress(Gpu + Page) == Device + Page);
        REQUIRE(second.GpuToCpuAddress(GpuAlias + Page) == Device + Page);
        REQUIRE(device.GetPointer<u8>(OldAlias) == old_pages + Page);
        REQUIRE(reverse_mappings(old_pages + Page) == std::vector<DAddr>{OldAlias});
        REQUIRE(reverse_mappings(new_page) == std::vector<DAddr>{Device + Page});

        // The old three-page continuous host run is now broken at the middle page.
        std::array<u8, 3 * Page> bytes{};
        first.ReadBlockUnsafe(Gpu, bytes.data(), bytes.size());
        REQUIRE(std::all_of(bytes.begin(), bytes.begin() + Page, [](u8 b) { return b == 0x11; }));
        REQUIRE(std::all_of(bytes.begin() + Page, bytes.begin() + 2 * Page,
                            [](u8 b) { return b == 0xbb; }));
        REQUIRE(std::all_of(bytes.begin() + 2 * Page, bytes.end(), [](u8 b) { return b == 0x33; }));

        // Repeated reuse must not accumulate duplicate reverse-map entries.
        for (int reuse = 0; reuse < 4; ++reuse) {
            set_page(Page, old_pages + Page);
            refresh();
            const std::vector<DAddr> both{Device + Page, OldAlias};
            REQUIRE(reverse_mappings(old_pages + Page) == both);
            set_page(Page, new_page);
            refresh();
            REQUIRE(reverse_mappings(old_pages + Page) == std::vector<DAddr>{OldAlias});
            REQUIRE(reverse_mappings(new_page) == std::vector<DAddr>{Device + Page});
        }
        const u64 refreshed_generation = device.TranslationGeneration();
        refresh();
        REQUIRE(device.TranslationGeneration() == refreshed_generation);
    }

    SECTION("An unmapped page is cleared and can acquire backing again") {
        set_page(2 * Page, nullptr);
        refresh();
        REQUIRE(first.GetPointer(Gpu + 2 * Page) == nullptr);
        REQUIRE(second.GetPointer(GpuAlias + 2 * Page) == nullptr);
        std::array<u8, Page> bytes;
        bytes.fill(0xff);
        second.ReadBlockUnsafe(GpuAlias + 2 * Page, bytes.data(), bytes.size());
        REQUIRE(std::ranges::all_of(bytes, [](u8 b) { return b == 0; }));
        set_page(2 * Page, new_page);
        refresh();
        REQUIRE(first.GetPointer(Gpu + 2 * Page) == new_page);
        REQUIRE(second.GetPointer(GpuAlias + 2 * Page) == new_page);
    }
}
