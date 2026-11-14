// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the front end (bananative/dm/gfx/nvn/hle_nvn.c) and Lichii's own NVN functions
// (nvn_window.c) take from the dispatch (nvn_dispatch.c). C only.

#pragma once

#include <stdint.h>

/// The guest's return address of the NVN call this thread runs (its LR). Upstream records the
/// host's __builtin_return_address(0) as the game's call site.
extern _Thread_local uint64_t nvn_direct_guest_lr;
static inline uint64_t nvn_caller(void) { return nvn_direct_guest_lr; }

/// The game's address of a host pointer into the fastmem arena (anything else: unchanged). The
/// replay keys its pool memory by the game's addresses, so GPU addresses the game wrote into its
/// own data (shader-loaded pointers, c0's storage table) find it as the front end's do.
uint64_t nvn_direct_guest_of(uint64_t host);
/// The host pointer of a game address (anything at or past the guest address space: unchanged).
uint64_t nvn_direct_host_of(uint64_t guest);
/// nvn_direct_memory.map_view (nvn_direct.h).
void* nvn_direct_map_view(uint64_t address, uint64_t size, uint64_t* unmapped);
