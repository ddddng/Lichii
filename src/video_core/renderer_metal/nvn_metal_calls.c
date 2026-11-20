// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string.h>

#include "nvn_metal.h"
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "nvn_queue.h"

uint64_t nvn_direct_host_of(uint64_t guest);

int nvn_direct_call_blocks(int slot, const uint64_t x[8]) {
    if (nvn_queue_stopping() || strcmp(nvn_direct_name(slot), "nvnSyncWait") != 0) {
        return 0;
    }
    const sync_t* const sync = (const sync_t*)(uintptr_t)nvn_direct_host_of(x[0]);
    if (!sync->fenced) {
        return 0;
    }
    if (sync->fenced == 3) {
        return !nvn_sync_wait(sync, 0);
    }
    return !(sync->fenced == 2 ? mtl_wait_acq : mtl_wait)(sync->fence, 0);
}
