// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "common/host_memory.h"
#include "common/logging.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/kernel.h"
#include "video_core/renderer_metal/guest_pool_backing.h"

namespace Core::NvnHook {

/// The Metal replay's pool mapper (lichii_metal_set_pool_mapper). It opens a reference to every
/// guest page of the host-page envelope, so the kernel cannot free and reuse them while Metal may
/// read or write them, and maps their backing once more for the zero-copy buffer. Map runs in
/// nvnMemoryPoolInitialize, a halted call; the references are closed by
/// lichii_metal_drain_pool_pins on a kernel thread once Metal has released the buffer.
class MetalPoolPins {
public:
    MetalPoolPins(Kernel::KernelCore& kernel_, Kernel::KProcessPageTable& page_table_,
                  std::shared_ptr<Common::HostMemory> backing_)
        : kernel{kernel_}, page_table{page_table_}, backing{std::move(backing_)} {}

    [[nodiscard]] lichii_metal_pool_mapper Mapper() {
        return {this, Map};
    }

private:
    struct Pin {
        Kernel::KernelCore* kernel;
        std::vector<Common::HostMemory::BackingRun> runs;
    };

    static int Map(void* context, u64 address, u64 length, lichii_metal_pool_lease* out) {
        auto& self = *static_cast<MetalPoolPins*>(context);
        std::unique_ptr<Pin> pin;
        try {
            pin = std::make_unique<Pin>(Pin{&self.kernel, {}});
        } catch (const std::bad_alloc&) {
            return 0;
        }
        Kernel::KPageGroup group{self.kernel, self.page_table.GetBlockInfoManager()};
        if (const Result result = self.page_table.MakeAndOpenPageGroup(
                &group, address, length / Kernel::PageSize, Kernel::KMemoryState::None,
                Kernel::KMemoryState::None, Kernel::KMemoryPermission::None,
                Kernel::KMemoryPermission::None, Kernel::KMemoryAttribute::None,
                Kernel::KMemoryAttribute::None);
            R_FAILED(result)) {
            LOG_ERROR(Core, "NVN Metal pool {:#x}+{:#x}: no page group (result {:#x})", address,
                      length, result.raw);
            return 0;
        }
        try {
            size_t offset = 0;
            for (const auto& block : group) {
                pin->runs.push_back({.view_offset = offset,
                                     .host_offset = static_cast<size_t>(
                                         GetInteger(block.GetAddress()) - DramMemoryMap::Base),
                                     .length = block.GetSize()});
                offset += block.GetSize();
            }
            if (Metal::MapGuestPoolBacking(self.backing, length, pin->runs, out)) {
                out->pin = pin.release();
                out->unpin = Unpin;
                return 1;
            }
            size_t unaligned = 0;
            for (const auto& run : pin->runs) {
                unaligned += (run.host_offset | run.length) % 0x4000 != 0;
            }
            LOG_ERROR(Core,
                      "NVN Metal pool {:#x}+{:#x}: no backing view over {} physical runs, {} not "
                      "on 16 KiB host pages (first at {:#x}+{:#x})",
                      address, length, pin->runs.size(), unaligned,
                      pin->runs.empty() ? 0 : pin->runs.front().host_offset,
                      pin->runs.empty() ? 0 : pin->runs.front().length);
        } catch (const std::bad_alloc&) {
        }
        group.Close(self.kernel);
        return 0;
    }

    static void Unpin(void* context, int close) {
        const std::unique_ptr<Pin> pin{static_cast<Pin*>(context)};
        if (!close) {
            return;
        }
        for (const auto& run : pin->runs) {
            pin->kernel->MemoryManager().Close(run.host_offset + DramMemoryMap::Base,
                                               run.length / Kernel::PageSize);
        }
    }

    Kernel::KernelCore& kernel;
    Kernel::KProcessPageTable& page_table;
    std::shared_ptr<Common::HostMemory> backing;
};

} // namespace Core::NvnHook
