// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#import <Metal/Metal.h>

namespace Metal {

[[nodiscard]] id<MTLBuffer> ImportGuestBuffer(id<MTLDevice> device,
                                            std::shared_ptr<void> mapping, std::size_t length);

} // namespace Metal
