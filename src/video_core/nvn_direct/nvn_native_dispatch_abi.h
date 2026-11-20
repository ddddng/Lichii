// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nvn_obj.h"

typedef struct {
    const NVNtexture* tex[DLSS_TEXTURES];
    int32_t iw, ih, base[DLSS_TEXTURES][2];
    float mv[2], jitter[2], pre_exposure;
    uint32_t reset;
} dlss_params_t;

// The pinned hle_nvn.c retains the original array in the builder, including its guest addresses.
// Only the synchronous raster initialization gets a translated copy.
typedef struct {
    NVNdevice* dev;
    const NVNshaderData *sd, *rt;
    int n, nrt;
    uint32_t trace_depth, callable_depth, flags;
} nvn_native_program_builder;

_Static_assert(sizeof(void*) == 8 && sizeof(int) == 4, "NVN2 guest word sizes");
_Static_assert(sizeof(NVNbufferRange) == 16 && offsetof(NVNbufferRange, size) == 8, "NVN2 buffer range");
_Static_assert(sizeof(NVNshaderData) == 16 && offsetof(NVNshaderData, control) == 8, "NVN2 shader data");
_Static_assert(sizeof(NVNseparateTextureHandle) == 8 && sizeof(NVNseparateSamplerHandle) == 8,
               "NVN2 separate handles");
_Static_assert(sizeof(nvn_native_program_builder) == 48 &&
               offsetof(nvn_native_program_builder, sd) == 8 &&
               offsetof(nvn_native_program_builder, n) == 24, "nxnative program builder");

static NVNboolean nvn_native_program_initialize(NVNboolean (*fn)(NVNprogram*, const void*),
                                                 NVNprogram* program, const void* builder) {
    if (!builder) return 0;
    nvn_native_program_builder copy;
    memcpy(&copy, builder, sizeof copy);
    if (copy.n < 0 || copy.n > 6 || copy.nrt || (copy.n && !copy.sd)) return 0;
    copy.sd = xl_shader_data(guest_addr((uint64_t)(uintptr_t)copy.sd), copy.n, 0);
    return fn(program, &copy);
}
