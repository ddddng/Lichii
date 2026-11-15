// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon::GpuTrace {

/// Writes a small synthetic trace that needs no game: one Switch 2 channel whose frames are
/// CPU-written images and 3D engine clears (whole and scissored), each presented.
/// Returns false if the file can't be written.
bool WriteSampleTrace(const std::filesystem::path& path);

/// Writes a synthetic trace for texture cache churn: each of `frames` frames binds the render
/// targets `pattern` names (steady, resize, resize-unique, color-resize-unique, formats, drift,
/// heap, heap-drift, table), clears them and presents a fixed CPU-written image; "table" also
/// rewrites TIC pool slots and resolves a 64-entry light table (RecordType::TextureVisit). Replay
/// it with EDEN_NX2_TC_TRACE=1 and count the images with tools/nx2/tc_churn.py.
bool WriteChurnTrace(const std::filesystem::path& path, std::string_view pattern, u32 frames);

bool IsChurnPattern(std::string_view pattern);

} // namespace VideoCommon::GpuTrace
