// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <new>
#include <span>
#include <utility>
#include <unistd.h>

#include "common/host_memory.h"
#include "video_core/renderer_metal/nvn_pool_bridge.h"

namespace Metal {

[[nodiscard]] inline bool MapGuestPoolBacking(std::shared_ptr<Common::HostMemory> backing,
                                              size_t length,
                                              std::span<const Common::HostMemory::BackingRun> runs,
                                              lichii_metal_pool_lease* out) {
    if (!out) {
        return false;
    }
    *out = {};
    const auto page = static_cast<size_t>(getpagesize());
    if (!backing || !length || length % page != 0) {
        return false;
    }
    size_t covered = 0;
    const size_t backing_size = backing->BackingSize();
    for (const auto& run : runs) {
        if (run.view_offset != covered || run.host_offset % page != 0 || !run.length ||
            run.length % page != 0 || run.length > length - covered ||
            run.host_offset > backing_size || run.length > backing_size - run.host_offset) {
            return false;
        }
        covered += run.length;
    }
    if (covered != length) {
        return false;
    }
    auto owner = std::unique_ptr<std::shared_ptr<Common::HostMemory>>{
        new (std::nothrow) std::shared_ptr<Common::HostMemory>{std::move(backing)}};
    if (!owner) {
        return false;
    }
    auto* const view = (*owner)->MapBackingView(length, runs);
    if (!view) {
        return false;
    }
    *out = {view, owner.release(), [](void* context, void* bytes, uint64_t size) {
                const auto memory = std::unique_ptr<std::shared_ptr<Common::HostMemory>>{
                    static_cast<std::shared_ptr<Common::HostMemory>*>(context)};
                (*memory)->UnmapBackingView(static_cast<u8*>(bytes), size);
            }};
    return true;
}

} // namespace Metal
