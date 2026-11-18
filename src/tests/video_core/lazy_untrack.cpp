// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <optional>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/nx2_lazy_untrack.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

class TrackerRasterizer final : public VideoCore::RasterizerInterface {
public:
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
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType) override {
        invalidate(addr, size);
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

    std::function<void(DAddr, u64)> invalidate;
};

using Tracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;

constexpr DAddr DevicePage = 0x10000;
constexpr u64 PageSize = 0x1000;
constexpr u64 GuestPageA = 8;
constexpr u64 GuestPageB = 9;

/// A device memory manager over one process with guest pages 8 and 9 backed, and the buffer
/// cache's tracker on it. InvalidateRegion untracks as BufferCache::WriteMemory does.
struct Fixture {
    Fixture() {
        system.Initialize();
        process.emplace(system.Kernel());
        device_memory.BindInterface(&rasterizer);
        rasterizer.invalidate = [this](DAddr addr, u64 size) {
            tracker.MarkRegionAsCpuModified(addr, size);
        };
        process->GetPageTable().GetImpl().Resize(20, 12);
        Back(GuestPageA, physical.GetPointerFromRaw<u8>(0x10000));
        Back(GuestPageB, physical.GetPointerFromRaw<u8>(0x30000));
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device_memory.RegisterProcess(&process->GetMemory());
    }

    ~Fixture() {
        VideoCommon::Nx2LazyUntrack::t_hold = false;
        device_memory.Unmap(DevicePage, PageSize);
        device_memory.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(&*process);
    }

    void Back(u64 page, u8* host) {
        process->GetPageTable().GetImpl().entries.GetAndFault(page).Store(
            false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(host) - (page << 12));
    }

    /// True while the guest page is write protected for the rasterizer.
    bool Protected(u64 page) {
        return process->GetPageTable().GetImpl().entries.GetAndFault(page).Type() ==
               Common::PageType::RasterizerCachedMemory;
    }

    /// A sync of the page: uploads what the CPU wrote and tracks it again. Returns the word the
    /// upload read, or 0 when nothing was uploaded.
    u32 Upload() {
        u32 uploaded = 0;
        tracker.ForEachUploadRange(DevicePage, PageSize, [&](DAddr addr, u64) {
            uploaded = device_memory.Read<u32>(addr);
        });
        return uploaded;
    }

    /// The GPU back thread untracks the page for a guest write: the page is held.
    void BackUntrack() {
        VideoCommon::Nx2LazyUntrack::t_hold = true;
        tracker.MarkRegionAsCpuModified(DevicePage, PageSize);
        VideoCommon::Nx2LazyUntrack::t_hold = false;
    }

    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    TrackerRasterizer rasterizer;
    Tracker tracker{device_memory};
    std::optional<Kernel::KProcess> process;
    Core::Asid asid{};
};

} // namespace

TEST_CASE("Lazy untrack: a CPU write during the hold reaches the next upload",
          "[video_core][lazy_untrack]") {
    Fixture f;
    f.device_memory.Map(DevicePage, GuestPageA << 12, PageSize, f.asid);
    f.Upload();
    REQUIRE(f.Protected(GuestPageA));

    f.BackUntrack();
    REQUIRE(f.Protected(GuestPageA));
    REQUIRE(f.tracker.IsRegionCpuModified(DevicePage, PageSize));

    // The guest write traps on the held protection and lands in memory; the trap's untrack
    // finds the page untracked already.
    f.device_memory.Write<u32>(DevicePage, 0x1234'5678);
    f.BackUntrack();
    REQUIRE(f.Protected(GuestPageA));

    REQUIRE(f.Upload() == 0x1234'5678);
    REQUIRE(f.Protected(GuestPageA));
    REQUIRE(!f.tracker.IsRegionCpuModified(DevicePage, PageSize));

    // The upload took the hold over: ticks drop nothing, and the one untrack left unprotects.
    for (int frame = 0; frame < 4; ++frame) {
        f.device_memory.TickHeldPages(false);
    }
    REQUIRE(f.Protected(GuestPageA));
    f.tracker.MarkRegionAsCpuModified(DevicePage, PageSize);
    REQUIRE(!f.Protected(GuestPageA));
}

TEST_CASE("Lazy untrack: an unmap during the hold releases the page",
          "[video_core][lazy_untrack]") {
    Fixture f;
    f.device_memory.Map(DevicePage, GuestPageA << 12, PageSize, f.asid);
    f.Upload();
    f.BackUntrack();
    REQUIRE(f.Protected(GuestPageA));

    f.device_memory.Unmap(DevicePage, PageSize);
    REQUIRE(!f.Protected(GuestPageA));

    // Mapped again, the page is tracked and protected from a count of zero.
    f.device_memory.Map(DevicePage, GuestPageA << 12, PageSize, f.asid);
    f.Upload();
    REQUIRE(f.Protected(GuestPageA));
    f.tracker.MarkRegionAsCpuModified(DevicePage, PageSize);
    REQUIRE(!f.Protected(GuestPageA));
}

TEST_CASE("Lazy untrack: a map over a held page protects the new backing",
          "[video_core][lazy_untrack]") {
    Fixture f;
    f.device_memory.Map(DevicePage, GuestPageA << 12, PageSize, f.asid);
    f.Upload();
    f.BackUntrack();

    f.device_memory.Map(DevicePage, GuestPageB << 12, PageSize, f.asid);
    REQUIRE(!f.Protected(GuestPageA));
    f.Upload();
    REQUIRE(f.Protected(GuestPageB));
}

TEST_CASE("Lazy untrack: the frame tick releases a held page after two frames",
          "[video_core][lazy_untrack]") {
    Fixture f;
    f.device_memory.Map(DevicePage, GuestPageA << 12, PageSize, f.asid);
    f.Upload();
    f.BackUntrack();

    f.device_memory.TickHeldPages(false);
    REQUIRE(f.Protected(GuestPageA));
    f.device_memory.TickHeldPages(false);
    REQUIRE(!f.Protected(GuestPageA));

    // Its count is gone with it: tracked again, it is protected, and one untrack unprotects.
    f.Upload();
    REQUIRE(f.Protected(GuestPageA));
    f.tracker.MarkRegionAsCpuModified(DevicePage, PageSize);
    REQUIRE(!f.Protected(GuestPageA));
}
