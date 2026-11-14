// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN2 proc-table hook (dotfiles-o72, lab branch nvn-hle). Games get every NVN function but
// nvnBootstrapLoader by name, through nvnBootstrapLoader (exported by the NVN driver module,
// `sdk`) or nvnDeviceGetProcAddress (which they get from nvnBootstrapLoader). The hook stands at
// nvnBootstrapLoader (found by its export name, in any game) and at what it returns for
// nvnDeviceGetProcAddress, and hands the game a synthetic address per name instead of
// the driver's; the JIT translates a block at a synthetic address as a host call that counts
// the call and continues in the driver's function (pass-through), or runs a host implementation
// and returns to the caller. The same boundary nxnative (seedds, MIT) and Switch2Recomp's census
// hook stand at; this code is Lichii's own.
//
// Off unless EDEN_NX2_NVN_HOOK=1 or EDEN_NX2_NVN_DIRECT=1. Output to the file EDEN_NX2_NVN_OUT names:
//   "n <slot> <driver address> <synthetic address> <name>" once per name the game resolved;
//   every EDEN_NX2_NVN_EVERY presents (default 300): "t <present> <steady ns>", then
//   cumulative "c <present> <slot> <calls>" for each slot called, and with
//   EDEN_NX2_NVN_EMITTED=1 "e <present> <key> <headers>" (headers the game's top-level
//   command-buffer calls wrote, key = subchannel << 13 | method), "ew <present> <calls>
//   <words> <skipped>" and "f <present> <key> <headers>" (headers the GPU front parsed);
//   "p0 <present> <value>" (nvnPrivate0).
// EDEN_NX2_NVN_PRIVATE0=<n>: nvnPrivate0 is set to n after every proc-address lookup
// (dotfiles-o72.2: 0 makes the game's inlined NVN encoding call the proc table instead).
// EDEN_NX2_NVN_HLE=BindTexture (dotfiles-o72.3): nvnCommandBufferBindTexture on graphics stages
// runs here and writes a 3-word marker (video_core/nvn_hle_marker.h) instead of the driver's
// 4-5 words; the front replays the driver's method calls from it without parsing them. Macro
// 0xe1c's argument per stage is taken from the driver's first call on that stage. With
// EDEN_NX2_NVN_HLE_VERIFY=1 the driver writes every call and its words are compared with what
// the marker would have stood for. Counts: "h <present> <hle> <fallbacks> <checked> <mismatches>".

#pragma once

#include "common/common_types.h"

namespace Dynarmic::A64 {
class Jit;
enum class BlockEntryHook : std::uint8_t;
} // namespace Dynarmic::A64

namespace Core::Memory {
class Memory;
}

namespace Kernel {
class KProcess;
}

namespace Core::NvnHook {

[[nodiscard]] bool Enabled();

/// NVN2 direct mode (dotfiles-25x): the game's NVN calls run in the direct renderer
/// (video_core/nvn_direct), not in its NVN driver. Decided once per process by Install
/// (EDEN_NX2_NVN_DIRECT=1, an NVN2 game); read by the Vulkan device, created after Install.
[[nodiscard]] bool DirectMode();

/// Finds the NVN driver's exports in the application's modules (after they are loaded).
void Install(Kernel::KProcess& process);

/// UserConfig::block_entry_hook (null when the hook is off).
Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc);

/// InstructionCacheOperationRaised(BlockEntryHook, pc) from a core's JIT.
void OnBlockEntry(u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
                  Kernel::KProcess* process, u32 thread_id);

/// A present (nx2:vi2), numbered from 1.
void OnPresent(u64 present);

} // namespace Core::NvnHook
