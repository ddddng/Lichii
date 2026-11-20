// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_metal/guest_buffer.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <new>
#include <unistd.h>
#include <utility>

#include "video_core/renderer_metal/guest_pool_backing.h"
#include "video_core/renderer_metal/nvn_pool_bridge.h"

namespace {

std::mutex pool_mapper_mutex;
lichii_metal_pool_mapper pool_mapper{};
uintptr_t pool_arena{};
uint64_t pool_guest_size{};

} // namespace

namespace Metal {

id<MTLBuffer> ImportGuestBuffer(id<MTLDevice> device, std::shared_ptr<void> mapping,
                               std::size_t length) {
    const auto page_size = static_cast<std::size_t>(getpagesize());
    const auto start = reinterpret_cast<std::uintptr_t>(mapping.get());
    if (!device.hasUnifiedMemory || !mapping || mapping.use_count() == 0 || length == 0 ||
        length > device.maxBufferLength || start % page_size != 0 || length % page_size != 0) {
        return nil;
    }

    constexpr auto read_write = VM_PROT_READ | VM_PROT_WRITE;
    for (std::uintptr_t address = start; address < start + length;) {
        mach_vm_address_t region_start = address;
        mach_vm_size_t region_size{};
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t name = MACH_PORT_NULL;
        const auto result =
            mach_vm_region(mach_task_self(), &region_start, &region_size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &info_count, &name);
        if (name != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), name);
        }
        if (result != KERN_SUCCESS || region_start > address ||
            (info.protection & read_write) != read_write) {
            std::fprintf(stderr, "[lichii] Metal buffer %#llx+%#llx: no read-write memory at %#llx\n",
                         static_cast<unsigned long long>(start),
                         static_cast<unsigned long long>(length),
                         static_cast<unsigned long long>(address));
            return nil;
        }
        address = region_start + region_size;
    }

    auto* const bytes = mapping.get();
    __block auto lease = std::move(mapping);
    return [device newBufferWithBytesNoCopy:bytes
                                     length:length
                                    options:MTLResourceStorageModeShared
                                deallocator:^(void*, NSUInteger) {
                                  lease.reset();
                                }];
}

} // namespace Metal

void lichii_metal_set_pool_mapper(const void* arena, uint64_t guest_size,
                                const lichii_metal_pool_mapper* mapper) {
    const auto base = reinterpret_cast<uintptr_t>(arena);
    const auto page = static_cast<uintptr_t>(getpagesize());
    std::scoped_lock lock{pool_mapper_mutex};
    pool_mapper = {};
    pool_arena = 0;
    pool_guest_size = 0;
    if (mapper && mapper->map && base && base % page == 0 && guest_size &&
        guest_size <= std::numeric_limits<uintptr_t>::max() - base) {
        pool_mapper = *mapper;
        pool_arena = base;
        pool_guest_size = guest_size;
    }
}

id<MTLBuffer> lichii_metal_new_guest_pool_buffer(id<MTLDevice> device, const void* address,
                                               uint64_t length, uint64_t* buffer_offset) {
    if (!device.hasUnifiedMemory || !length || !buffer_offset) {
        return nil;
    }
    const auto page = static_cast<uint64_t>(getpagesize());
    const auto start = reinterpret_cast<uintptr_t>(address);
    uint64_t bias{};
    uint64_t aligned_length{};
    lichii_metal_pool_lease lease{};
    auto node = std::unique_ptr<Metal::PendingUnpin>{new (std::nothrow) Metal::PendingUnpin{}};
    if (!node) {
        return nil;
    }
    try {
        {
            std::scoped_lock lock{pool_mapper_mutex};
            if (!pool_mapper.map || start < pool_arena || start - pool_arena >= pool_guest_size) {
                std::fprintf(stderr,
                             "[lichii] Metal pool %#llx+%#llx: outside the guest arena %#llx+%#llx "
                             "(mapper %s)\n",
                             static_cast<unsigned long long>(start),
                             static_cast<unsigned long long>(length),
                             static_cast<unsigned long long>(pool_arena),
                             static_cast<unsigned long long>(pool_guest_size),
                             pool_mapper.map ? "set" : "unset");
                return nil;
            }
            const uint64_t guest_start = start - pool_arena;
            if (length > pool_guest_size - guest_start) {
                return nil;
            }
            bias = guest_start % page;
            const uint64_t aligned_start = guest_start - bias;
            aligned_length = bias + length;
            const uint64_t tail = (page - aligned_length % page) % page;
            if (tail > pool_guest_size - aligned_start - aligned_length) {
                return nil;
            }
            aligned_length += tail;
            if (aligned_length > device.maxBufferLength) {
                std::fprintf(stderr, "[lichii] Metal pool %#llx+%#llx: over maxBufferLength %#llx\n",
                             static_cast<unsigned long long>(aligned_start),
                             static_cast<unsigned long long>(aligned_length),
                             static_cast<unsigned long long>(device.maxBufferLength));
                return nil;
            }
            if (!pool_mapper.map(pool_mapper.context, aligned_start, aligned_length, &lease)) {
                return nil;
            }
        }
        if (!lease.release) {
            if (lease.unpin) {
                lease.unpin(lease.pin, 1);
            }
            return nil;
        }
        auto* const pin = lease.unpin ? node.release() : nullptr;
        if (pin) {
            *pin = {lease.pin, lease.unpin, nullptr};
        }
        auto mapping =
            std::shared_ptr<void>{lease.bytes, [lease, aligned_length, pin](void* bytes) {
                                      lease.release(lease.context, bytes, aligned_length);
                                      if (pin) {
                                          Metal::DeferUnpin(pin);
                                      }
                                  }};
        id<MTLBuffer> buffer = Metal::ImportGuestBuffer(device, std::move(mapping), aligned_length);
        if (!buffer) {
            std::fprintf(stderr, "[lichii] Metal pool %#llx+%#llx: no zero-copy buffer\n",
                         static_cast<unsigned long long>(start),
                         static_cast<unsigned long long>(length));
            return nil;
        }
        *buffer_offset = bias;
        return buffer;
    } catch (const std::bad_alloc&) {
        return nil;
    }
}

void lichii_metal_drain_pool_pins(int close) {
    Metal::DrainUnpins(close != 0);
}
