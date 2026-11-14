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

#ifdef __cplusplus
}
#endif
