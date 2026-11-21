// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/frame_gen/flyhigh_library.h"
#include "video_core/renderer_vulkan/present/flyhigh_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

#ifdef _WIN32
constexpr VkExternalMemoryHandleTypeFlagBits MEMORY_HANDLE =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits SEMAPHORE_HANDLE =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr VkExternalMemoryHandleTypeFlagBits MEMORY_HANDLE =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits SEMAPHORE_HANDLE =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

/// A handle passed to FlyHigh: a Windows handle stays ours after the import, a file descriptor
/// becomes FlyHigh's once the import succeeds.
void ReleaseHandle(intptr_t handle, bool imported) {
#ifdef _WIN32
    (void)imported;
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    if (!imported) {
        close(static_cast<int>(handle));
    }
#endif
}

struct InteropHandle {
    explicit InteropHandle(intptr_t handle) : value{handle} {}
    ~InteropHandle() {
        ReleaseHandle(value, imported);
    }

    intptr_t value;
    bool imported{};
};

VkBufferMemoryBarrier BufferBarrier(VkBuffer buffer, u32 src_queue, u32 dst_queue,
                                    VkAccessFlags src_access, VkAccessFlags dst_access) {
    return VkBufferMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = src_queue,
        .dstQueueFamilyIndex = dst_queue,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
}

void TransferOwnership(vk::CommandBuffer cmdbuf, VkBuffer buffer, u32 src_queue, u32 dst_queue,
                       VkAccessFlags src_access, VkAccessFlags dst_access) {
    const auto barrier = BufferBarrier(buffer, src_queue, dst_queue, src_access, dst_access);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                           {}, barrier, {});
}

[[noreturn]] void Fail(const std::string& what) {
    throw std::runtime_error(what);
}

const VideoCore::FrameGen::FlyHighLibrary& RequireLibrary() {
    const auto* library = VideoCore::FrameGen::LoadFlyHighLibrary();
    if (library == nullptr) {
        Fail("the FlyHigh library is missing or too old");
    }
    return *library;
}

VkImageMemoryBarrier Transition(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageBlit WholeBlit(VkExtent2D extent) {
    const VkImageSubresourceLayers layers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const VkOffset3D corner{static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1};
    return VkImageBlit{
        .srcSubresource = layers,
        .srcOffsets = {{0, 0, 0}, corner},
        .dstSubresource = layers,
        .dstOffsets = {{0, 0, 0}, corner},
    };
}

VkBufferImageCopy RowsCopy(VkExtent2D extent, u32 row_pitch) {
    return VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = row_pitch / 4,
        .bufferImageHeight = extent.height,
        .imageSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {},
        .imageExtent = {extent.width, extent.height, 1},
    };
}

bool IsSrgb(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

} // Anonymous namespace

bool FlyHighZeroCopyRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_FLYHIGH_ZEROCOPY");
        return value && std::string_view{value} == "1";
    }();
    return requested;
}

struct FlyHighFrameGen::Worker {
    struct Engine {
        const VideoCore::FrameGen::FlyHighLibrary& fh = RequireLibrary();
        FhEngine* engine{};
        FhModel* model{};
        FhStream* stream{};
        FhVkBuffer* input{};
        std::array<FhVkBuffer*, MAX_GENERATIONS> outputs{};
        FhVkSemaphore* frames_in{};
        FhVkSemaphore* frames_out{};
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

        ~Engine() {
            if (stream) {
                fh.fh_stream_destroy(stream);
            }
            for (auto* buffer : outputs) {
                if (buffer) {
                    fh.fh_vk_buffer_destroy(buffer);
                }
            }
            if (input) {
                fh.fh_vk_buffer_destroy(input);
            }
            if (frames_in) {
                fh.fh_vk_semaphore_destroy(frames_in);
            }
            if (frames_out) {
                fh.fh_vk_semaphore_destroy(frames_out);
            }
            if (model) {
                fh.fh_model_destroy(model);
            }
            if (engine) {
                fh.fh_engine_destroy(engine);
            }
        }

        void Check(int result, const char* call) const {
            if (result < 0) {
                Fail(fmt::format("{} failed: {}", call, fh.LastError()));
            }
        }
    };

    std::mutex mutex;
    std::condition_variable ready;
    std::packaged_task<void(Engine&)> pending;
    bool stopped{};
    bool owns_slot{};
    inline static std::atomic_uint running{};

    ~Worker() {
        if (owns_slot) {
            running.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static std::shared_ptr<Worker> Start() {
        (void)RequireLibrary();
        auto worker = std::make_shared<Worker>();
        if (running.fetch_add(1, std::memory_order_relaxed) >= 2) {
            running.fetch_sub(1, std::memory_order_relaxed);
            Fail("two FlyHigh workers are still running");
        }
        worker->owns_slot = true;
        std::thread([worker] {
            Engine engine;
            for (;;) {
                std::packaged_task<void(Engine&)> job;
                {
                    std::unique_lock lock{worker->mutex};
                    worker->ready.wait(lock, [&] { return worker->stopped || worker->pending.valid(); });
                    if (worker->stopped) {
                        return;
                    }
                    job = std::move(worker->pending);
                }
                job(engine);
            }
        }).detach();
        return worker;
    }

    template<typename Task>
    auto Run(std::chrono::steady_clock::time_point deadline, Task task) {
        using Result = std::invoke_result_t<Task, Engine&>;
        std::packaged_task<Result(Engine&)> job{std::move(task)};
        auto result = job.get_future();
        {
            std::scoped_lock lock{mutex};
            pending = std::packaged_task<void(Engine&)>{std::move(job)};
        }
        ready.notify_one();
        if (result.wait_until(deadline) != std::future_status::ready) {
            Fail("the FlyHigh engine did not finish before the deadline");
        }
        return result.get();
    }

    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopped = true;
        }
        ready.notify_one();
    }
};

FlyHighFrameGen::FlyHighFrameGen(const Device& device_, MemoryAllocator& memory_allocator,
                                 VkExtent2D extent_, VkFormat format, bool protect_hud)
    : device{device_}, extent{extent_}, zero_copy{FlyHighZeroCopyRequested()} {
    try {
        Build(memory_allocator, format, protect_hud);
    } catch (...) {
        ReleaseBuffers();
        throw;
    }
}

void FlyHighFrameGen::Build(MemoryAllocator& memory_allocator, VkFormat format, bool protect_hud) {
    using namespace std::chrono_literals;

    if (!VideoCore::FrameGen::FlyHighModelInstalled()) {
        Fail("the FlyHigh model is not installed");
    }
    if (zero_copy) {
        if (!device.HasExternalInterop() || !device.HasTimelineSemaphore()) {
            Fail("this device can't share memory and timeline semaphores with another Vulkan device");
        }
    }

    const VkFormat rgba_format =
        IsSrgb(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    constexpr auto blit_features = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const auto physical = device.GetPhysical();
    if ((physical.GetFormatProperties(format).optimalTilingFeatures & blit_features) != blit_features ||
        (physical.GetFormatProperties(rgba_format).optimalTilingFeatures & blit_features) != blit_features) {
        Fail("this device can't blit between the present format and RGBA8");
    }
    VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &id};
    physical.GetProperties2(properties);

    const std::string adapter =
        fmt::format("{:04x}:{:04x}", device.GetVendorID(), device.GetDeviceID());
    const auto tune_dir = Common::FS::PathToUTF8String(VideoCore::FrameGen::FlyHighTuneDir());
    const auto model_dir = Common::FS::PathToUTF8String(VideoCore::FrameGen::FlyHighModelDir());
    std::array<u8, VK_UUID_SIZE> device_uuid;
    std::memcpy(device_uuid.data(), id.deviceUUID, device_uuid.size());

    worker = Worker::Start();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto info = worker->Run(
        deadline, [adapter, tune_dir, model_dir, device_uuid, extent = extent,
                   protect_hud, zero_copy = zero_copy](Worker::Engine& state) {
            const auto& fh = state.fh;
            FhEngineOptions options = fh.fh_engine_options_default();
            options.backend = 1;
            options.adapter = adapter.c_str();
            options.tune_cache = tune_dir.c_str();
            state.engine = fh.fh_engine_create(&options);
            if (!state.engine) {
                Fail(fmt::format("no FlyHigh engine on {}: {}", adapter, fh.LastError()));
            }
            if (zero_copy) {
                std::array<u8, VK_UUID_SIZE> engine_uuid{};
                if (fh.fh_engine_vulkan_interop(state.engine, engine_uuid.data()) != 1 ||
                    engine_uuid != device_uuid) {
                    Fail("FlyHigh cannot import Vulkan resources on Lichii's device");
                }
            }
            state.model = fh.fh_model_load(model_dir.c_str());
            if (!state.model) {
                Fail(fmt::format("the model did not load: {}", fh.LastError()));
            }
            FhSessionOptions session = fh.fh_session_options_default(extent.width, extent.height);
            session.keep_static = protect_hud ? 1 : 0;
            FhStreamOptions stream = fh.fh_stream_options_default();
            stream.outputs = static_cast<u32>(MAX_GENERATIONS);
            state.stream = fh.fh_stream_create(state.engine, state.model, &session, &stream);
            if (!state.stream) {
                Fail(fmt::format("no FlyHigh stream at {}x{}: {}", extent.width, extent.height,
                                 fh.LastError()));
            }
            state.Check(fh.fh_stream_warm_up(state.stream, state.engine), "fh_stream_warm_up");
            char info[512]{};
            fh.fh_engine_info(state.engine, info, sizeof(info));
            return std::string(info);
        });

    row_pitch = zero_copy ? RequireLibrary().fh_vk_row_pitch(extent.width) : extent.width * 4;
    buffer_size = static_cast<VkDeviceSize>(row_pitch) * extent.height;
    staging = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        // The network sees the bytes as they are encoded, the way it was trained.
        .format = rgba_format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    if (zero_copy) {
        CreateSharedBuffer(input);
        for (auto& output : outputs) {
            CreateSharedBuffer(output);
        }
        frames_in = CreateSharedTimeline(true);
        frames_out = CreateSharedTimeline(false);
    } else {
        readback = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
        for (auto& upload : uploads) {
            upload = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Upload);
        }
    }
    LOG_INFO(Render_Vulkan, "FlyHigh {} at {}x{}: {}",
             zero_copy ? "experimental zero-copy" : "CPU staging", extent.width, extent.height, info);
}

FlyHighFrameGen::~FlyHighFrameGen() {
    ReleaseBuffers();
}

void FlyHighFrameGen::ReleaseBuffers() noexcept {
    if (worker) {
        worker->Stop();
    }
    const auto& dld = device.GetDispatchLoader();
    dld.vkDestroyBuffer(*device.GetLogical(), input.buffer, nullptr);
    for (auto& output : outputs) {
        dld.vkDestroyBuffer(*device.GetLogical(), output.buffer, nullptr);
    }
}

void FlyHighFrameGen::CreateSharedBuffer(SharedBuffer& shared) {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    const VkExternalMemoryBufferCreateInfo external{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = MEMORY_HANDLE,
    };
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external,
        .flags = 0,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    vk::Check(dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &shared.buffer));

    const VkMemoryRequirements requirements =
        device.GetLogical().GetBufferMemoryRequirements(shared.buffer);
    const VkPhysicalDeviceMemoryProperties memory_types =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    u32 type_index = memory_types.memoryTypeCount;
    for (u32 i = 0; i < memory_types.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
            (memory_types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            type_index = i;
            break;
        }
    }
    if (type_index == memory_types.memoryTypeCount) {
        Fail("no device-local memory can be shared");
    }
    const VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = VK_NULL_HANDLE,
        .buffer = shared.buffer,
    };
    const VkExportMemoryAllocateInfo export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = MEMORY_HANDLE,
    };
    shared.memory = device.GetLogical().AllocateMemory(VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type_index,
    });
    vk::Check(dld.vkBindBufferMemory(logical, shared.buffer, *shared.memory, 0));

#ifdef _WIN32
    const intptr_t handle = reinterpret_cast<intptr_t>(shared.memory.GetMemoryWin32HandleKHR());
#else
    const intptr_t handle = shared.memory.GetMemoryFdKHR();
#endif
    const FhVkMemory memory{
        .handle = handle,
        .allocation_size = requirements.size,
        .memory_type_index = type_index,
    };
    auto owned = std::make_shared<InteropHandle>(handle);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    worker->Run(deadline, [memory, owned, bytes = buffer_size, is_input = &shared == &input,
                           index = &shared == &input ? 0 : static_cast<size_t>(&shared - outputs.data())](
                              Worker::Engine& state) {
        auto* imported = state.fh.fh_vk_buffer_import(state.engine, &memory, bytes);
        if (!imported) {
            Fail(fmt::format("FlyHigh could not import shared memory: {}", state.fh.LastError()));
        }
        owned->imported = true;
        (is_input ? state.input : state.outputs[index]) = imported;
    });
}

vk::Semaphore FlyHighFrameGen::CreateSharedTimeline(bool is_input) {
    const VkExportSemaphoreCreateInfo export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = SEMAPHORE_HANDLE,
    };
    const VkSemaphoreTypeCreateInfo type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &export_info,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    auto semaphore = device.GetLogical().CreateSemaphore(VkSemaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type,
        .flags = 0,
    });

    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
#ifdef _WIN32
    const auto get_handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        dld.vkGetDeviceProcAddr(logical, "vkGetSemaphoreWin32HandleKHR"));
    if (get_handle == nullptr) {
        Fail("vkGetSemaphoreWin32HandleKHR is missing");
    }
    const VkSemaphoreGetWin32HandleInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
        .pNext = nullptr,
        .semaphore = *semaphore,
        .handleType = SEMAPHORE_HANDLE,
    };
    HANDLE win32_handle{};
    vk::Check(get_handle(logical, &info, &win32_handle));
    const intptr_t handle = reinterpret_cast<intptr_t>(win32_handle);
#else
    const auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        dld.vkGetDeviceProcAddr(logical, "vkGetSemaphoreFdKHR"));
    if (get_fd == nullptr) {
        Fail("vkGetSemaphoreFdKHR is missing");
    }
    const VkSemaphoreGetFdInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .semaphore = *semaphore,
        .handleType = SEMAPHORE_HANDLE,
    };
    int fd{};
    vk::Check(get_fd(logical, &info, &fd));
    const intptr_t handle = fd;
#endif
    auto owned = std::make_shared<InteropHandle>(handle);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    worker->Run(deadline, [owned, is_input](Worker::Engine& state) {
        auto* imported = state.fh.fh_vk_semaphore_import(state.engine, owned->value, 1);
        if (!imported) {
            Fail(fmt::format("FlyHigh could not import a semaphore: {}", state.fh.LastError()));
        }
        owned->imported = true;
        (is_input ? state.frames_in : state.frames_out) = imported;
    });
    return semaphore;
}

size_t FlyHighFrameGen::Process(Scheduler& scheduler, VkImage source, size_t generations) {
    using namespace std::chrono_literals;

    if (zero_copy && frame_value == 0) {
        const auto queue = device.GetGraphicsFamily();
        const std::array buffers{outputs[0].buffer, outputs[1].buffer, outputs[2].buffer};
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([buffers, queue](vk::CommandBuffer cmdbuf) {
            for (VkBuffer buffer : buffers) {
                TransferOwnership(cmdbuf, buffer, queue, VK_QUEUE_FAMILY_EXTERNAL, 0, 0);
            }
        });
    }

    const bool reacquire = zero_copy && frame_value != 0;
    const VkBuffer target = zero_copy ? input.buffer : *readback;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source, staging_image = *staging, buffer = target, extent = extent,
                      row_pitch = row_pitch, cross_device = zero_copy, reacquire,
                      queue = device.GetGraphicsFamily()](vk::CommandBuffer cmdbuf) {
        if (reacquire) {
            TransferOwnership(cmdbuf, buffer, VK_QUEUE_FAMILY_EXTERNAL, queue, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        const std::array before{
            Transition(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            Transition(staging_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        cmdbuf.BlitImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WholeBlit(extent),
                         VK_FILTER_NEAREST);
        const std::array after{
            Transition(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Transition(staging_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               0, {}, {}, after);
        cmdbuf.CopyImageToBuffer(staging_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                                 RowsCopy(extent, row_pitch));
        if (cross_device) {
            TransferOwnership(cmdbuf, buffer, queue, VK_QUEUE_FAMILY_EXTERNAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        } else {
            const auto barrier = BufferBarrier(buffer, VK_QUEUE_FAMILY_IGNORED,
                                               VK_QUEUE_FAMILY_IGNORED,
                                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                   {}, barrier, {});
        }
    });

    const u64 value = ++frame_value;
    std::vector<u8> source_bytes;
    if (zero_copy) {
        const auto previous = value > 1 ? SemaphoreValue{*frames_out, value - 1} : SemaphoreValue{};
        scheduler.Flush(SemaphoreValue{*frames_in, value}, previous);
    } else {
        scheduler.Finish();
        readback.Invalidate();
        const auto mapped = readback.Mapped().first(static_cast<size_t>(buffer_size));
        source_bytes.assign(mapped.begin(), mapped.end());
    }

    const auto budget = !generated_once && generations != 0 ? 2s : 50ms;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    auto [made, frames] = worker->Run(
        deadline, [source = std::move(source_bytes), wanted = std::min(generations, MAX_GENERATIONS),
                   bytes = static_cast<size_t>(buffer_size), cross_device = zero_copy,
                   pitch = row_pitch, value](Worker::Engine& state) {
            const auto& fh = state.fh;
            const int slot = fh.fh_stream_next_input(state.stream, nullptr, 0);
            state.Check(slot, "fh_stream_next_input");
            if (cross_device) {
                state.Check(fh.fh_stream_copy_in(state.stream, state.engine, static_cast<u32>(slot),
                                                 state.input, pitch, state.frames_in, value),
                            "fh_stream_copy_in");
            } else {
                state.Check(fh.fh_stream_write_rgba(state.stream, state.engine, static_cast<u32>(slot),
                                                    source.data(), source.size()),
                            "fh_stream_write_rgba");
            }
            const double pts = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - state.start).count();
            FhPush push{};
            state.Check(fh.fh_stream_push(state.stream, state.engine, static_cast<u32>(slot),
                                          pts, &push), "fh_stream_push");

            const size_t count = push.ready ? wanted : 0;
            std::array<std::vector<u8>, MAX_GENERATIONS> frames;
            for (size_t i = 0; i < count; ++i) {
                const float t = static_cast<float>(i + 1) / static_cast<float>(count + 1);
                const u32 out = static_cast<u32>(i);
                const int generated = fh.fh_stream_generate(state.stream, state.engine, t, out);
                state.Check(generated, "fh_stream_generate");
                if (generated == 0) {
                    Fail("FlyHigh did not write an intermediate frame");
                }
                if (cross_device) {
                    state.Check(fh.fh_stream_copy_out(state.stream, state.engine, out,
                                                      state.outputs[i], pitch, nullptr, 0),
                                "fh_stream_copy_out");
                } else {
                    frames[i].resize(bytes);
                    const int written = fh.fh_stream_read_rgba(state.stream, state.engine, out,
                                                               frames[i].data(), frames[i].size());
                    state.Check(written, "fh_stream_read_rgba");
                    if (static_cast<size_t>(written) != bytes) {
                        Fail("FlyHigh returned an incomplete frame");
                    }
                }
            }
            if (cross_device) {
                state.Check(fh.fh_vk_signal(state.engine, state.frames_out, value), "fh_vk_signal");
            }
            return std::pair{count, std::move(frames)};
        });
    if (zero_copy) {
        const auto remaining = std::chrono::ceil<std::chrono::nanoseconds>(
            deadline - std::chrono::steady_clock::now());
        if (!frames_out.Wait(value, static_cast<u64>(std::max(remaining, 0ns).count()))) {
            Fail("the FlyHigh interop semaphore did not signal before the deadline");
        }
        output_ready = made != 0;
    } else {
        for (size_t i = 0; i < made; ++i) {
            std::ranges::copy(frames[i], uploads[i].Mapped().begin());
            uploads[i].Flush();
        }
    }
    generated_once |= made != 0;
    return made;
}

SemaphoreValue FlyHighFrameGen::TakeWait() {
    if (!std::exchange(output_ready, false)) {
        return {};
    }
    return SemaphoreValue{*frames_out, frame_value};
}

void FlyHighFrameGen::Generate(Scheduler& scheduler, VkImage destination, size_t generation) {
    scheduler.RequestOutsideRenderPassOperationContext();
    const VkBuffer buffer = zero_copy ? outputs[generation].buffer : *uploads[generation];
    scheduler.Record([destination, staging_image = *staging, buffer, extent = extent,
                      row_pitch = row_pitch, zero_copy = zero_copy,
                      queue = device.GetGraphicsFamily()](vk::CommandBuffer cmdbuf) {
        if (zero_copy) {
            TransferOwnership(cmdbuf, buffer, VK_QUEUE_FAMILY_EXTERNAL, queue, 0,
                              VK_ACCESS_TRANSFER_READ_BIT);
        }
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            Transition(staging_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        cmdbuf.CopyBufferToImage(buffer, staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 RowsCopy(extent, row_pitch));
        if (zero_copy) {
            TransferOwnership(cmdbuf, buffer, queue, VK_QUEUE_FAMILY_EXTERNAL,
                              VK_ACCESS_TRANSFER_READ_BIT, 0);
        }
        const std::array before{
            Transition(staging_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            Transition(destination, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        cmdbuf.BlitImage(staging_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WholeBlit(extent),
                         VK_FILTER_NEAREST);
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            Transition(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL));
    });
}

} // namespace Vulkan
