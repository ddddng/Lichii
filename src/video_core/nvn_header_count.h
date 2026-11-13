// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace VideoCommon::NvnHeaderCount {

/// EDEN_NX2_NVN_EMITTED (Core::NvnHook): the front counts the command headers it parses by
/// (subchannel << 13 | method), to compare with the headers NVN's proc-table calls wrote.
inline bool g_enabled{};
/// Written by the GPU (front) thread only; read racily by the NVN hook's periodic dump.
inline std::array<u64, 1u << 16> g_headers{};

inline void Count(u32 subchannel, u32 method) {
    if (g_enabled) [[unlikely]] {
        ++g_headers[((subchannel & 7) << 13) | (method & 0x1fff)];
    }
}

} // namespace VideoCommon::NvnHeaderCount
