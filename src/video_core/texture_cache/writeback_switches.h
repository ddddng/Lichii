// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings.h"

/// Switches for the texture cache's write-backs of GPU-written overlaps. Each is on for The
/// Duskbloods and Ocarina of Time when unset; set it to 1 to turn it on for every title, or 0 to
/// turn it off.
namespace VideoCommon::WritebackSwitches {

inline constexpr u64 DuskbloodsNetworkTestProgramId = 0x0400D1D02399A000ULL;
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;

/// The titles the write-backs are on for when unset. The Duskbloods drew its message banner and
/// character maps from dropped overlaps; Ocarina of Time's shooting gallery text box showed noise
/// and older lettering on every try after the first (#1310).
[[nodiscard]] inline bool DefaultTitle() {
    const u64 program_id = Settings::GetCurrentProgramID();
    return program_id == DuskbloodsNetworkTestProgramId || program_id == OcarinaOfTimeProgramId;
}

/// A switch's setting: 1 on for every title, 0 off, -1 unset.
[[nodiscard]] inline int SwitchSetting(const char* name) {
    const char* const value = std::getenv(name);
    return value == nullptr ? -1 : value[0] == '1' ? 1 : value[0] == '0' ? 0 : -1;
}

[[nodiscard]] inline bool TitleDefault(int setting) {
    return setting >= 0 ? setting == 1 : DefaultTitle();
}

/// Whether a switch with this setting is on, for a default title or another one.
[[nodiscard]] constexpr bool SwitchOn(int setting, bool default_title) {
    return setting >= 0 ? setting == 1 : default_title;
}

/// Whether the collector's (EDEN_NX2_GC_DOWNLOAD_OVERLAPS) or joins' (EDEN_NX2_JOIN_WRITEBACK)
/// write-backs are on, from their settings. Either one queues write-backs, so whatever else
/// writes an image to guest memory has to be kept from them and noted while either is on.
[[nodiscard]] constexpr bool AnyWritebackOn(int collector_setting, int join_setting,
                                            bool default_title) {
    return SwitchOn(collector_setting, default_title) || SwitchOn(join_setting, default_title);
}

[[nodiscard]] inline bool AnyWritebackOn() {
    static const int collector_setting = SwitchSetting("EDEN_NX2_GC_DOWNLOAD_OVERLAPS");
    static const int join_setting = SwitchSetting("EDEN_NX2_JOIN_WRITEBACK");
    return AnyWritebackOn(collector_setting, join_setting, DefaultTitle());
}

/// EDEN_NX2_UPLOAD_STALE: an image made over pages a dropped GPU-written image left stale loads
/// guest memory like any other, instead of keeping whatever its host memory held before. The
/// Duskbloods samples such images (small per-character render targets), which drew outfits,
/// blades and enemies in rainbow noise.
[[nodiscard]] inline bool UploadStale() {
    static const int setting = SwitchSetting("EDEN_NX2_UPLOAD_STALE");
    return TitleDefault(setting);
}

} // namespace VideoCommon::WritebackSwitches
