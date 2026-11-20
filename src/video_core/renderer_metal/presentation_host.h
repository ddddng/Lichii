// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <functional>
#include <memory>

namespace Metal {

using PresentedCallback = std::function<void(uint64_t ticket, double seconds)>;

// The callback runs on a Metal thread and must not call back into this host.
[[nodiscard]] bool BindPresentationHost(std::shared_ptr<void> layer, PresentedCallback presented);
// Stop and drain the backend's layer users before unbinding the borrowed C handle.
void UnbindPresentationHost();
[[nodiscard]] bool IsPresentationHostBound();

} // namespace Metal

extern "C" {
#endif

void* front_metal_layer(void);
// Capture the generation when registering the drawable's presented handler.
uint64_t lichii_nvn_metal_present_generation(void);
void lichii_nvn_metal_presented(uint64_t generation, uint64_t ticket, double seconds);

#ifdef __cplusplus
}
#endif
