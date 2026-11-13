// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <map>
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
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

struct CachedPages {
    std::map<DAddr, s32> counts;

    void UpdatePagesCachedCount(DAddr address, size_t size, s32 delta) {
        constexpr u64 Page = Core::Memory::YUZU_PAGESIZE;
        for (DAddr page = address / Page; page < (address + size + Page - 1) / Page; ++page) {
            counts[page * Page] += delta;
        }
    }

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        for (const auto& [address, size] : ranges) {
            UpdatePagesCachedCount(address, size, delta);
        }
    }
};

// Use the buffer cache's real page-state tracker behind the SMMU invalidation callback.
class InvalidationRasterizer final : public VideoCore::RasterizerInterface {
public:
    CachedPages pages;
    VideoCommon::MemoryTrackerBase<CachedPages> tracker{pages};
    std::vector<std::pair<DAddr, u64>> invalidations;

    void InvalidateRegion(DAddr address, u64 size, VideoCommon::CacheType) override {
        invalidations.emplace_back(address, size);
        tracker.MarkRegionAsCpuModified(address, size);
    }

    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr address, u64 size) override {
        return {address, address + size, false};
    }
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};

} // namespace

TEST_CASE("NX2 presentation resolves parent handles and image offsets",
          "[video_core][nx2_mapping]") {
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
    std::memset(new_page, 0xbb, 3 * Page);
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
    InvalidationRasterizer rasterizer;
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
    rasterizer.tracker.UnmarkRegionAsCpuModified(Device, 3 * Page);
    rasterizer.tracker.MarkRegionAsGpuModified(Device, 3 * Page);
    rasterizer.invalidations.clear();
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
    REQUIRE(rasterizer.invalidations.empty());
    for (u64 offset = 0; offset < 3 * Page; offset += Page) {
        REQUIRE(rasterizer.tracker.IsRegionGpuModified(Device + offset, Page));
        REQUIRE(rasterizer.pages.counts.at(Device + offset) == 1);
    }

    SECTION("A changed later page replaces stale geometry and keeps other aliases intact") {
        // Same CPU address and size, different physical page, as after sub 102 and sub 101.
        set_page(Page, new_page);
        REQUIRE(memory.GetPointerSilent(Cpu + Page) == new_page);
        REQUIRE(first.GetPointer(Gpu + Page) == old_pages + Page);
        REQUIRE(*first.GetPointer(Gpu + Page) == 0x22);
        refresh();
        const std::vector<std::pair<DAddr, u64>> expected{{Device + Page, Page}};
        REQUIRE(rasterizer.invalidations == expected);
        REQUIRE(rasterizer.tracker.IsRegionGpuModified(Device, Page));
        REQUIRE_FALSE(rasterizer.tracker.IsRegionGpuModified(Device + Page, Page));
        REQUIRE(rasterizer.tracker.IsRegionGpuModified(Device + 2 * Page, Page));
        REQUIRE(rasterizer.pages.counts.at(Device) == 1);
        REQUIRE(rasterizer.pages.counts.at(Device + Page) == 0);
        REQUIRE(rasterizer.pages.counts.at(Device + 2 * Page) == 1);
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

    SECTION("Separated replacements preserve the GPU-owned page between them") {
        set_page(0, new_page);
        set_page(2 * Page, new_page + 2 * Page);
        refresh();
        const std::vector<std::pair<DAddr, u64>> expected{{Device, Page},
                                                          {Device + 2 * Page, Page}};
        REQUIRE(rasterizer.invalidations == expected);
        REQUIRE_FALSE(rasterizer.tracker.IsRegionGpuModified(Device, Page));
        REQUIRE(rasterizer.tracker.IsRegionGpuModified(Device + Page, Page));
        REQUIRE_FALSE(rasterizer.tracker.IsRegionGpuModified(Device + 2 * Page, Page));
        REQUIRE(rasterizer.pages.counts.at(Device + Page) == 1);
        REQUIRE(first.GetPointer(Gpu + Page) == old_pages + Page);
        std::array<u8, 3 * Page> bytes{};
        second.ReadBlockUnsafe(GpuAlias, bytes.data(), bytes.size());
        REQUIRE(std::all_of(bytes.begin(), bytes.begin() + Page, [](u8 b) { return b == 0xbb; }));
        REQUIRE(std::all_of(bytes.begin() + Page, bytes.begin() + 2 * Page,
                            [](u8 b) { return b == 0x22; }));
        REQUIRE(std::all_of(bytes.begin() + 2 * Page, bytes.end(), [](u8 b) { return b == 0xbb; }));
    }

    SECTION("Adjacent replacements share one invalidation ending at the range boundary") {
        set_page(Page, new_page);
        set_page(2 * Page, new_page + Page);
        refresh();
        const std::vector<std::pair<DAddr, u64>> expected{{Device + Page, 2 * Page}};
        REQUIRE(rasterizer.invalidations == expected);
        REQUIRE(rasterizer.tracker.IsRegionGpuModified(Device, Page));
        REQUIRE_FALSE(rasterizer.tracker.IsRegionGpuModified(Device + Page, 2 * Page));
        REQUIRE(rasterizer.pages.counts.at(Device) == 1);
        std::array<u8, 3 * Page> bytes{};
        first.ReadBlockUnsafe(Gpu, bytes.data(), bytes.size());
        REQUIRE(std::all_of(bytes.begin(), bytes.begin() + Page, [](u8 b) { return b == 0x11; }));
        REQUIRE(std::all_of(bytes.begin() + Page, bytes.end(), [](u8 b) { return b == 0xbb; }));
    }
}

TEST_CASE("NX2 destroyed memory gets a new device range when registered again",
          "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    // Like nvhost_as_gpu: device ranges cached by CPU range, shared through the registry.
    std::map<std::pair<u64, u64>, u64> ranges;
    u64 next_device = 0x100000;
    const auto allocate = [&next_device] {
        const u64 device_address = next_device;
        next_device += 0x20000;
        return device_address;
    };
    registry.AddListener(
        &ranges,
        [&](u64, u64 cpu_address, u64 size) {
            const std::pair key{cpu_address, size};
            if (!ranges.contains(key)) {
                ranges[key] = registry.AcquireDeviceRange(1, cpu_address, size, allocate);
            }
            registry.RecordDeviceRange(cpu_address, size, ranges[key]);
        },
        [](u64, u64) {},
        [&](u64 cpu_address, u64 size) {
            if (ranges.erase(std::pair{cpu_address, size}) != 0) {
                registry.ReleaseDeviceRange(1, cpu_address, size);
            }
        });
    registry.Register(0x10000, 0x4000);
    const u64 first = ranges.at({0x10000, 0x4000});
    REQUIRE(registry.DeviceAddressOf(0x10000) == first);

    // Sub 102, then the game registers new memory at the same address.
    registry.UnmapMemory(0x10000, 0x4000);
    registry.ReleaseBacking(0x10000, 0x4000);
    REQUIRE(ranges.empty());
    REQUIRE_FALSE(registry.DeviceAddressOf(0x10000));
    registry.Register(0x10000, 0x4000);

    const u64 second = ranges.at({0x10000, 0x4000});
    REQUIRE(second != first);
    REQUIRE(registry.DeviceAddressOf(0x10000) == second);
}

namespace {
using Service::Nvidia::Devices::Nx2EraseContainedRanges;
using Service::Nvidia::Devices::Nx2GpuMemoryRegistry;
using Key = std::pair<u64, u64>;

// Like nvhost_as_gpu: device ranges by CPU range, shared through the registry, freed by the last
// address space that maps them.
struct AddressSpace {
    Nx2GpuMemoryRegistry& registry;
    std::vector<Key>& freed;
    std::map<Key, u64> ranges;

    void Map(u64 cpu_address, u64 size) {
        ranges[{cpu_address, size}] =
            registry.AcquireDeviceRange(1, cpu_address, size, [cpu_address] { return cpu_address << 4; });
    }

    void Release(u64 cpu_address, u64 size) {
        Nx2EraseContainedRanges(ranges, cpu_address, size, [this](u64 range_cpu, u64 range_size, u64) {
            if (registry.ReleaseDeviceRange(1, range_cpu, range_size)) {
                freed.emplace_back(range_cpu, range_size);
            }
        });
    }
};
} // namespace

TEST_CASE("NX2 releasing memory frees every device range inside it",
          "[video_core][nx2_mapping][nx2_release]") {
    Nx2GpuMemoryRegistry registry;
    std::vector<Key> freed;
    AddressSpace as{registry, freed};
    as.Map(0x10000, 0x4000); // the memory itself
    as.Map(0x11000, 0x1000); // 211 maps of parts of it
    as.Map(0x12000, 0x2000);
    as.Map(0x10000, 0x1000);
    as.Map(0x13000, 0x2000); // reaches past its end
    as.Map(0xf000, 0x2000);  // starts before it
    as.Map(0xe000, 0x2000);  // neighbours
    as.Map(0x14000, 0x1000);

    as.Release(0x10000, 0x4000);

    const std::vector<Key> expected_freed{
        {0x10000, 0x1000}, {0x10000, 0x4000}, {0x11000, 0x1000}, {0x12000, 0x2000}};
    REQUIRE(freed == expected_freed);
    const std::vector<Key> expected_kept{{0xe000, 0x2000}, {0xf000, 0x2000}, {0x13000, 0x2000},
                                         {0x14000, 0x1000}};
    std::vector<Key> kept;
    for (const auto& [key, device_address] : as.ranges) {
        kept.push_back(key);
    }
    REQUIRE(kept == expected_kept);
}

TEST_CASE("NX2 releasing an exact range frees only that range",
          "[video_core][nx2_mapping][nx2_release]") {
    Nx2GpuMemoryRegistry registry;
    std::vector<Key> freed;
    AddressSpace as{registry, freed};
    as.Map(0x10000, 0x1000);
    as.Map(0x10000, 0x2000);
    as.Map(0x11000, 0x1000);

    as.Release(0x10000, 0x1000);

    REQUIRE(freed == std::vector<Key>{{0x10000, 0x1000}});
    REQUIRE(as.ranges.size() == 2);
}

TEST_CASE("NX2 a released range another address space maps stays until it releases it too",
          "[video_core][nx2_mapping][nx2_release]") {
    Nx2GpuMemoryRegistry registry;
    std::vector<Key> freed;
    AddressSpace first{registry, freed};
    AddressSpace second{registry, freed};
    first.Map(0x10000, 0x4000);
    first.Map(0x11000, 0x1000);
    second.Map(0x11000, 0x1000);
    REQUIRE(first.ranges.at({0x11000, 0x1000}) == second.ranges.at({0x11000, 0x1000}));

    first.Release(0x10000, 0x4000);
    REQUIRE(freed == std::vector<Key>{{0x10000, 0x4000}});
    REQUIRE(first.ranges.empty());

    second.Release(0x10000, 0x4000);
    REQUIRE(freed == std::vector<Key>{{0x10000, 0x4000}, {0x11000, 0x1000}});
}

TEST_CASE("NX2 releasing memory at the top of the address space does not wrap",
          "[video_core][nx2_mapping][nx2_release]") {
    Nx2GpuMemoryRegistry registry;
    std::vector<Key> freed;
    AddressSpace as{registry, freed};
    constexpr u64 top = 0xffff'ffff'ffff'0000ULL;
    as.Map(top + 0x8000, 0x8000);
    as.Map(top + 0x8000, 0x10000); // would wrap past the end of the address space
    as.Map(0x1000, 0x1000);

    as.Release(top, 0x10000);

    REQUIRE(freed == std::vector<Key>{{top + 0x8000, 0x8000}});
    REQUIRE(as.ranges.size() == 2);
}

TEST_CASE("NX2 released memory loses the device addresses of the ranges inside it",
          "[video_core][nx2_mapping][nx2_release]") {
    Nx2GpuMemoryRegistry registry;
    registry.RecordDeviceRange(0x10000, 0x1000, 0x100000);
    registry.RecordDeviceRange(0x11000, 0x1000, 0x200000);
    registry.RecordDeviceRange(0x13000, 0x2000, 0x300000); // reaches past the memory
    registry.RecordDeviceRange(0x14000, 0x1000, 0x400000); // neighbour

    registry.ReleaseBacking(0x10000, 0x4000);

    REQUIRE_FALSE(registry.DeviceAddressOf(0x10000));
    REQUIRE_FALSE(registry.DeviceAddressOf(0x11000));
    REQUIRE(registry.DeviceAddressOf(0x13800) == 0x300800U);
    REQUIRE(registry.DeviceAddressOf(0x14000) == 0x400000U);
}
