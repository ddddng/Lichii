// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A successful map transfers one lease. Release may run after the mapper is cleared. */
typedef struct lichii_metal_pool_lease {
    void* bytes;
    void* context;
    void (*release)(void* context, void* bytes, uint64_t length);
} lichii_metal_pool_lease;

typedef struct lichii_metal_pool_mapper {
    void* context;
    int (*map)(void* context, uint64_t guest_address, uint64_t aligned_length,
               lichii_metal_pool_lease* out);
} lichii_metal_pool_mapper;

/* The mapper context must remain valid until this is called with a null mapper. */
void lichii_metal_set_pool_mapper(const void* arena, uint64_t guest_size,
                                const lichii_metal_pool_mapper* mapper);

#ifdef __OBJC__
@protocol MTLBuffer;
@protocol MTLDevice;

/* Returns nil on failure; on success, buffer_offset locates the first logical pool byte. */
id<MTLBuffer> lichii_metal_new_guest_pool_buffer(id<MTLDevice> device, const void* address,
                                               uint64_t length, uint64_t* buffer_offset)
    __attribute__((ns_returns_retained));
#endif

#ifdef __cplusplus
}
#endif
