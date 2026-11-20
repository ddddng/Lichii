// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <mutex>
#include <utility>

#include "video_core/renderer_metal/presentation_host.h"

namespace {

std::mutex host_mutex;
std::shared_ptr<void> host_layer;
Metal::PresentedCallback host_presented;
uint64_t host_generation{};

} // namespace

namespace Metal {

bool BindPresentationHost(std::shared_ptr<void> layer, PresentedCallback presented) {
    const std::scoped_lock lock{host_mutex};
    if (!layer || !presented || host_layer) {
        return false;
    }
    ++host_generation;
    host_layer = std::move(layer);
    host_presented = std::move(presented);
    return true;
}

void UnbindPresentationHost() {
    std::shared_ptr<void> layer;
    PresentedCallback presented;
    {
        const std::scoped_lock lock{host_mutex};
        layer.swap(host_layer);
        presented.swap(host_presented);
    }
}

bool IsPresentationHostBound() {
    const std::scoped_lock lock{host_mutex};
    return static_cast<bool>(host_layer);
}

} // namespace Metal

void* front_metal_layer(void) {
    const std::scoped_lock lock{host_mutex};
    return host_layer.get();
}

uint64_t lichii_nvn_metal_present_generation(void) {
    const std::scoped_lock lock{host_mutex};
    return host_layer ? host_generation : 0;
}

void lichii_nvn_metal_presented(uint64_t generation, uint64_t ticket, double seconds) {
    if (!generation || !ticket || !(seconds > 0) || !std::isfinite(seconds)) {
        return;
    }
    const std::scoped_lock lock{host_mutex};
    if (host_layer && host_generation == generation) {
        host_presented(ticket, seconds);
    }
}
