// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

[[nodiscard]] bool FlyHighZeroCopyRequested();

class FlyHighFrameGen {
public:
    static constexpr size_t MAX_GENERATIONS = 3;

    /// Throws std::runtime_error with the reason when FlyHigh can't run on this device.
    explicit FlyHighFrameGen(const Device& device, MemoryAllocator& memory_allocator,
                             VkExtent2D extent, VkFormat format, bool protect_hud);
    ~FlyHighFrameGen();

    FlyHighFrameGen(const FlyHighFrameGen&) = delete;
    FlyHighFrameGen& operator=(const FlyHighFrameGen&) = delete;

    /// Hands the frame in source (GENERAL layout) to FlyHigh and asks for `generations`
    /// in-betweens of it and the frame before. Returns how many it made.
    [[nodiscard]] size_t Process(Scheduler& scheduler, VkImage source, size_t generations);

    /// Copies in-between `generation` into destination, left in GENERAL layout.
    void Generate(Scheduler& scheduler, VkImage destination, size_t generation);
    [[nodiscard]] SemaphoreValue TakeWait();

private:
    struct SharedBuffer {
        VkBuffer buffer{};
        vk::DeviceMemory memory;
    };

    struct Worker;

    void Build(MemoryAllocator& memory_allocator, VkFormat format, bool protect_hud);
    void ReleaseBuffers() noexcept;
    void CreateSharedBuffer(SharedBuffer& shared);
    vk::Semaphore CreateSharedTimeline(bool is_input);

    const Device& device;
    VkExtent2D extent;
    bool zero_copy;
    u32 row_pitch{};
    VkDeviceSize buffer_size{};

    vk::Image staging;
    vk::Buffer readback;
    std::array<vk::Buffer, MAX_GENERATIONS> uploads;
    SharedBuffer input;
    std::array<SharedBuffer, MAX_GENERATIONS> outputs{};
    vk::Semaphore frames_in;
    vk::Semaphore frames_out;
    u64 frame_value{};
    bool generated_once{};
    bool output_ready{};
    std::shared_ptr<Worker> worker;
};

} // namespace Vulkan
