// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Lichii's guest memory as the replay's memory pools need it.
typedef struct nvn_direct_memory {
    /// Maps the pages behind guest [address, address + size) (page aligned) once more, at a host
    /// range of their own: read-write and never protected, so the renderer can import them while the
    /// guest's own view (the fastmem arena) may be write-tracked or partly unmapped. Pages the
    /// guest has not mapped are zero pages of the view (their bytes in *unmapped). NULL on
    /// failure. The view is given back with unmap_view.
    void* (*map_view)(void* context, uint64_t address, uint64_t size, uint64_t* unmapped);
    /// Takes down a view map_view made, of size bytes.
    void (*unmap_view)(void* context, void* view, uint64_t size);
    /// Arms (1) or disarms (0) Lichii's tracking of CPU writes to the pool pages [address,
    /// address + size): a write to an armed page is reported (nvn_direct_cpu_written).
    void (*track_pages)(void* context, uint64_t address, uint64_t size, int armed);
    /// The backing memory of the guest page at address (never write-protected), NULL when the
    /// guest has not mapped it.
    void* (*backing)(void* context, uint64_t address);
    /// The guest range [*address, *address + *size) of a Switch 2 native image handle (nx2:gsv's
    /// images, as gsv 428 answers it); 0 when the handle names no image with bound memory.
    int (*native_image)(void* context, uint64_t handle, uint64_t* address, uint64_t* size);
    void* context;
    /// The file behind the backing memory (Lichii's memfd), -1 when there is none, and the
    /// backing view's base: the backing byte at p is the file's byte p - backing_base. Pools are
    /// imported as udmabufs of it where the driver refuses host-pointer imports of it (amdgpu).
    int backing_fd;
    const uint8_t* backing_base;
    /// The backing memory's bytes from backing_base (native Metal: its reverse map's extent).
    uint64_t backing_size;
    /// Native Metal on hosts whose pages are larger than the guest's
    /// (HostMemory::ArenaReadWritePages): bit arena_page_first + (address >> arena_page_shift) is
    /// set while the arena's host page holding guest address is mapped read-write; the dispatch
    /// reaches the others through backing. NULL: every mapped guest page is in the arena.
    const uint64_t* arena_pages;
    uint64_t arena_page_first;
    uint32_t arena_page_shift;
#if defined(_WIN32)
    /// Windows, where there is no file to map: a virtual pool's host range (HostMemory's alias
    /// regions), inaccessible until alias_map maps backing memory [backing_offset, backing_offset
    /// + size) into it at address (0 on failure); alias_unmap makes pages inaccessible again and
    /// alias_release gives the range back.
    void* (*alias_reserve)(void* context, uint64_t size);
    int (*alias_map)(void* context, void* address, uint64_t backing_offset, uint64_t size);
    void (*alias_unmap)(void* context, void* address, uint64_t size);
    void (*alias_release)(void* context, void* region);
#endif
} nvn_direct_memory;

/// Takes the front end's slot table (the names of upstream's names.txt, in its order), the
/// arena the game's addresses are offsets into (host = arena + address for every address below
/// guest_size), the guest memory's views and the game's NVN: switch2 = NVN2 (else NVN1), sdk_major
/// the NintendoSDK major its sdk was built with (-1 unknown, 0 a prerelease SDK 0.x; NVN1 reports
/// other API versions to SDK 10 and older). Called once, when the game's NVN is taken over.
void nvn_direct_init(uint8_t* arena, uint64_t guest_size, const nvn_direct_memory* memory,
                     int switch2, int sdk_major);

int nvn_direct_name_count(void);
const char* nvn_direct_name(int slot);
/// Whether the front end implements the slot's function (else the hook's stub answers 0).
int nvn_direct_implemented(int slot);
/// Whether the function takes or returns floats (v registers).
int nvn_direct_float_args(int slot);

#define NVN_DISPATCH_FLOAT_ARGS 1
#define NVN_DISPATCH_FLOAT_RETURN 2
/// Native dispatchers: the guest stack slots the function reads, from this bit of its flags.
#define NVN_DISPATCH_STACK_SHIFT 8

/// Runs an implemented slot's function with the guest's x0-x7 and v0-v7 (low 64 bits) as its
/// arguments, and the guest's stack at sp for the arguments past them; returns x0, and v0 through
/// *v0 for a float result. lr: the guest's return address (the call site the front end records).
uint64_t nvn_direct_call(int slot, const uint64_t x[8], const uint64_t v[8], uint64_t sp, uint64_t lr,
                         uint64_t* v0);

#ifdef __cplusplus
}
#endif
