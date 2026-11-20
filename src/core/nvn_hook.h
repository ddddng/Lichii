// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN proc-table hook for the NVN direct renderer. Games get every NVN function but
// nvnBootstrapLoader by name, through nvnBootstrapLoader (exported by the NVN driver module,
// `sdk`) or nvnDeviceGetProcAddress (which they get from nvnBootstrapLoader). The hook stands at
// nvnBootstrapLoader (found by its export name, in any game) and hands the game a synthetic
// address per name instead of the driver's; the JIT translates a block at a synthetic address as
// a host call that runs the direct renderer's front end (video_core/nvn_direct) and returns to
// the caller. The same boundary nxnative (seedds, MIT) and Switch2Recomp's census hook stand at;
// this code is Lichii's own.
//
// Chosen per boot by Install from the nvn_direct_renderer setting (EDEN_NX2_NVN_DIRECT=0/1
// overrides it; see nvn_direct_choice.h). The game's NVN driver never runs: names the front end
// lacks get a stub that answers 0. Calls that may block halt the JIT and run from the kernel
// (RunHaltedCall). Every 600 presents a line counts the halted calls and the stubs called.

#pragma once

#include <array>
#include <string_view>

#include "common/common_types.h"
#include "core/nvn_direct_choice.h"

namespace Dynarmic::A64 {
enum class BlockEntryHook : std::uint8_t;
} // namespace Dynarmic::A64

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Common {
struct PageTable;
}

namespace Kernel {
class KProcess;
}

namespace Core::NvnHook {

/// NVN direct mode: the game's NVN calls run in the direct renderer
/// (video_core/nvn_direct), not in its NVN driver. Decided once per boot by Install; read by
/// the Vulkan device, created after Install.
[[nodiscard]] bool DirectMode();

/// Native Metal NVN mode (LICHII_NVN_METAL, EDEN_NX2_NVN_METAL=1, an NVN2 game whose shaders pass
/// the local preflight): the game's NVN calls run in nxnative's Metal backend, presenting on the
/// frontend's CAMetalLayer, and no Vulkan device is made. Decided once per boot by Install.
[[nodiscard]] bool MetalMode();

/// Metal mode, CreateGPU (before the game runs): the boot cannot present natively. It runs as it
/// would without Metal mode. Logged.
void MetalFailed(std::string_view reason);

/// Metal mode, ShutdownMainProcess once the guest cores stopped (no NVN call runs): drains the
/// replay and its presents, frees the pool pins and unbinds the layer, before guest memory or the
/// window go.
void DrainMetal();

/// A boot of this process took Metal mode: Metal may hold views of the guest backing until the
/// process exits, so the backing stays.
[[nodiscard]] bool MetalRan();

/// Direct mode's title (0 otherwise): the direct renderer keeps its caches per title
/// (renderer_vulkan.cpp, DM_TEMP).
[[nodiscard]] u64 DirectProgramId();

/// Direct mode, the emulator shutting down: the game's threads waiting on the
/// host inside a halted NVN call return, so the kernel can suspend them. Before the cores are
/// suspended (System::Pause, ShutdownMainProcess). Metal mode: nxnative's permanent stop, so never
/// on a pause that resumes. Nothing otherwise.
void StopDirect();

/// Decides the boot's renderer (Choice) and, in direct mode, finds the NVN driver's
/// exports in the application's modules (after they are loaded). Logs the decision.
void Install(Core::System& system, Kernel::KProcess& process);

/// The Vulkan replay started successfully, before the guest runs.
void DirectStarted();

/// Direct mode, the Vulkan renderer being made (before the game runs): the direct renderer could
/// not start on the device. The boot falls back to GPU emulation, and so does every later boot
/// in this process that asks for the direct renderer. Logged.
void DirectFailed();

void TraceStartup(std::string_view phase);

/// The boot is over (the end of System::ShutdownMainProcess): its hook state goes.
void Uninstall();

/// The renderer Install chose for the current (or last) boot.
[[nodiscard]] RendererChoice Choice();

/// The next boot, under the current settings, asks for the direct renderer, and an earlier boot
/// of this process ran it: its replay starts once per process, so the boot would fall back
/// (RendererChoice::EarlierBoot) unless a fresh process runs it. With Metal mode built: any boot
/// after a Metal one (MetalRan).
[[nodiscard]] bool DirectNeedsFreshProcess();

/// EDEN_NX2_NVN_METAL=1 asks this launch for native Metal NVN (Install may still refuse it).
[[nodiscard]] bool MetalRequested();

/// UserConfig::block_entry_hook (null unless the direct renderer is asked for).
Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc);

/// Whether guest code reaching pc runs the hook instead (a synthetic address or
/// nvnBootstrapLoader): the Hypervisor backend asks at the fault or BRK that stops it there.
[[nodiscard]] bool IsThunk(u64 pc);

/// The guest registers a thunk reads and writes: the JIT's or a Hypervisor vCPU's.
class ThunkRegisters {
public:
    virtual u64 GetRegister(int index) = 0;
    virtual void SetRegister(int index, u64 value) = 0;
    /// The low 64 bits of v<index>; a set clears the high ones.
    virtual u64 GetVector(int index) = 0;
    virtual void SetVector(int index, u64 value) = 0;
    virtual u64 GetSP() = 0;
    virtual void SetSP(u64 value) = 0;

protected:
    ~ThunkRegisters() = default;
};

/// Guest code reached a thunk at pc (IsThunk): runs it and sets X16 to where the guest goes on.
/// Returns whether the core must halt for a call that may block: it then runs from the kernel as
/// SVC HaltedCallSvc.
[[nodiscard]] bool OnBlockEntry(u64 pc, ThunkRegisters& registers, Core::Memory::Memory& memory,
                                Kernel::KProcess* process);

/// The SVC number a halted NVN call takes (no real SVC is above 0xffff).
constexpr u32 HaltedCallSvc = 0x10000;

/// Direct mode: runs the NVN call that halted the JIT on this host thread, from Svc::Call (the
/// kernel may wait or switch threads here). args: x0-x7, x0 the result.
void RunHaltedCall(Core::System& system, std::array<u64, 8>& args);

/// NVN2 direct mode: a guest write reached write-tracked pool memory [address, address + size)
/// (Core::Memory's rasterizer write path): the replay's textures over it are uploaded again.
void OnDirectCpuWrite(u64 address, u64 size);

/// The guest unmaps [address, address + size) of a page table (Core::Memory::UnmapRegion): in
/// direct mode the application's pool memory imported there is given back first, and the
/// replay's readers of guest memory wait until OnGuestUnmapDone. Returns whether the caller
/// must call OnGuestUnmapDone once the pages are unmapped.
bool OnGuestUnmap(const Common::PageTable& page_table, u64 address, u64 size);
void OnGuestUnmapDone();

} // namespace Core::NvnHook
