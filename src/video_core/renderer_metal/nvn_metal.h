// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What Lichii's NVN hook calls in the native Metal NVN library (nvn_metal.cmake): the dispatch
// boundary (nvn_direct_dispatch.h), the shader preflight, the pool mapper and nxnative's stop and
// drain.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "video_core/nvn_direct/nvn_direct_dispatch.h"
#include "video_core/renderer_metal/nvn_metal_shaders.h"
#include "video_core/renderer_metal/nvn_pool_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Whether the halted call would wait for the Metal replay now: the hook then sleeps the guest
/// thread and asks again, so the wait does not hold its guest core.
int nvn_direct_call_blocks(int slot, const uint64_t x[8]);

/// An answer of NVN2's nvnDeviceGetInteger read from the game's driver (nvn_dispatch.c).
void nvn_direct_set_device_integer(unsigned item, int value);

/// Why the dispatch stopped running calls (a guest address it could not translate safely on 16 KiB
/// host pages, nvn_guest_address.h), NULL while it runs them.
const char* nvn_direct_failure(void);

/// The arena address of guest address (nvn_dispatch.c).
uint64_t nvn_direct_host_of(uint64_t guest);
/// A CPU write to tracked pool pages (TrackPages), at their arena address: the textures over them
/// are uploaded again at their next use (nxnative's nvn_dirty.c).
void nvn_dirty_cpu_written(void* addr, size_t len);

/// Permanent close, before the guest cores are suspended (never on a pause): waiters return.
void nvn_mtl_request_stop(void);
/// After every guest NVN call returned, before guest memory or the layer go: joins the replay
/// and present workers and waits for the Metal queues.
void nvn_mtl_drain(void);

#ifdef __cplusplus
}
#endif
