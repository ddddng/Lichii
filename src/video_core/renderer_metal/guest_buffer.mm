// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_metal/guest_buffer.h"

#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#include <utility>

namespace Metal {

id<MTLBuffer> ImportGuestBuffer(id<MTLDevice> device, std::shared_ptr<void> mapping,
                               std::size_t length) {
    const auto page_size = static_cast<std::size_t>(getpagesize());
    const auto start = reinterpret_cast<std::uintptr_t>(mapping.get());
    if (!device.hasUnifiedMemory || !mapping || mapping.use_count() == 0 || length == 0 ||
        length > device.maxBufferLength || start % page_size != 0 || length % page_size != 0) {
        return nil;
    }

    mach_vm_address_t region_start = start;
    mach_vm_size_t region_size{};
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t name = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &region_start, &region_size,
                                       VM_REGION_BASIC_INFO_64,
                                       reinterpret_cast<vm_region_info_t>(&info), &info_count, &name);
    if (name != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), name);
    }
    constexpr auto read_write = VM_PROT_READ | VM_PROT_WRITE;
    if (result != KERN_SUCCESS || (info.protection & read_write) != read_write ||
        region_start > start || length > region_size ||
        start - region_start > region_size - length) {
        return nil;
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
