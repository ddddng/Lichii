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
