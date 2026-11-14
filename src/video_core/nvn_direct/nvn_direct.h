// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN2 direct renderer (dotfiles-25x, lab): with EDEN_NX2_NVN_DIRECT=1 an NVN2 game's NVN calls
// run in Banana Native's NVN front end (bananative/dm/gfx/nvn/hle_nvn.c: objects and command
// records in the game's own memory) and its NVN -> Vulkan replay (bananative/dm/gfx/vk_android),
// not in the game's NVN driver and Lichii's Maxwell GPU path. This is the C interface between
// them and Lichii; the replay's own headers stay out of C++.
//
// One launch per process: the front end and the replay keep process-lifetime state (globals,
// detached threads), so direct mode is decided once (Core::NvnHook::Install) and a second game
// launched in the same process cannot use it.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/// At most this many queues of the graphics family are handed to the replay.
#define NVN_DIRECT_MAX_QUEUES 4

/// Lichii's Vulkan device as the replay adopts it (vka_dev.c makes none of its own).
typedef struct nvn_direct_device {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    /// The replay's queues: the NVN queues take all but the last one, the replay's own work
    /// (uploads, sparse binds) the last, which is Lichii's graphics queue.
    uint32_t queue_count;
    VkQueue queues[NVN_DIRECT_MAX_QUEUES];
    /// The device extensions Lichii enabled (the replay uses no other).
    const char* const* extensions;
    uint32_t extension_count;
    /// Every queue submission of the replay holds this lock (Lichii: Scheduler::submit_mutex).
    void (*queue_lock)(void* context);
    void (*queue_unlock)(void* context);
    void* queue_context;
} nvn_direct_device;

/// Hands Lichii's device to the replay and initialises it (vka_init); returns whether it is up.
/// Called once, after the device exists. Without it the replay has no device: submissions run
/// their CPU-side effects only and fences complete at submit (the null backend).
int nvn_direct_adopt_device(const nvn_direct_device* device);

/// Lichii's guest memory as the replay's memory pools need it (dotfiles-25x.5).
typedef struct nvn_direct_memory {
    /// Maps the pages behind guest [address, address + size) (page aligned) once more, at a host
    /// range of their own: read-write and never protected, so Vulkan imports them while the
    /// guest's own view (the fastmem arena) may be write-tracked or partly unmapped. Pages the
    /// guest has not mapped are zero pages of the view (their bytes in *unmapped). NULL on
    /// failure. The view is given back with munmap.
    void* (*map_view)(void* context, uint64_t address, uint64_t size, uint64_t* unmapped);
    void* context;
} nvn_direct_memory;

/// Takes the front end's slot table (the names of upstream's names.txt, in its order), the
/// arena the game's addresses are offsets into (host = arena + address for every address below
/// guest_size) and the guest memory's views. Called once, when the game's NVN is taken over.
void nvn_direct_init(uint8_t* arena, uint64_t guest_size, const nvn_direct_memory* memory);

/// The guest unmaps [address, address + size): the pool memory imported over it is given back
/// first (after the GPU is done with it), so no import keeps pages the guest reuses.
void nvn_direct_guest_unmap(uint64_t address, uint64_t size);
int nvn_direct_name_count(void);
const char* nvn_direct_name(int slot);
/// Whether the front end implements the slot's function (else the hook's stub answers 0).
int nvn_direct_implemented(int slot);
/// Whether the function takes or returns floats (v registers).
int nvn_direct_float_args(int slot);

#define NVN_DISPATCH_FLOAT_ARGS 1
#define NVN_DISPATCH_FLOAT_RETURN 2

/// Runs an implemented slot's function with the guest's x0-x7 and v0-v7 (low 64 bits) as its
/// arguments; returns x0, and v0 through *v0 for a float result. lr: the guest's return address
/// (the call site the front end records).
uint64_t nvn_direct_call(int slot, const uint64_t x[8], const uint64_t v[8], uint64_t lr, uint64_t* v0);

/// nvnDeviceGetInteger's answer for item, as the game's own driver gave it.
void nvn_direct_set_device_integer(unsigned item, int value);

#ifdef __cplusplus
}
#endif
