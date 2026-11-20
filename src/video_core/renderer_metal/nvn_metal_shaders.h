// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lichii_nvn_metal_prepare_shaders(const char* assets_dir, uint64_t program_id,
                                     const char* main_build_id, char* error, size_t error_size);
bool lichii_nvn_metal_shader_failed(void);

#ifdef __cplusplus
}
#endif
