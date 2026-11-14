// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "core/hle/kernel/svc.h"
#include "core/arm/debug.h"
#include "core/arm/symbols.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nvn_hook.h"
#include "core/nvn_trace.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "video_core/nvn_header_count.h"
#include "video_core/nvn_hle_marker.h"
#ifdef LICHII_NVN_DIRECT
#include "video_core/nvn_direct/nvn_direct.h"
#endif

namespace Core::NvnHook {
namespace {

using Dynarmic::A64::BlockEntryHook;

constexpr u32 MaxSlots = 4096;
/// Synthetic address the proc-address lookups return to (their caller's LR is kept per thread).
constexpr u32 LookupReturnSlot = 0;
/// Synthetic address command-buffer calls return to under EDEN_NX2_NVN_EMITTED.
constexpr u32 EmittedReturnSlot = 1;
/// What the game gets for nvnDeviceGetProcAddress (not exported: games get it from
/// nvnBootstrapLoader): a lookup like nvnBootstrapLoader's.
constexpr u32 LookupSlot = 2;
/// Synthetic address calls whose return the trace wants (nvn_trace.h) return to.
constexpr u32 TraceReturnSlot = 3;
/// Synthetic address nvnDeviceGetInteger returns to when its answers are captured.
constexpr u32 DeviceIntegerReturnSlot = 4;
constexpr u32 FirstNameSlot = 5;
constexpr u32 GuestThreadSlots = 4096;

struct Slot {
    u64 driver{};
    bool command_buffer{};
    /// Direct mode: the front end's slot (-1: no implementation, the stub answers 0) and whether
    /// the call leaves the JIT first (it may block: HaltedCallSvc).
    s32 front = -1;
    bool halts{};
};

/// Direct mode: the calls that may block. They run from the kernel (RunHaltedCall), not inside
/// the JIT's callback: a kernel wait or signal there would switch fibers under a live Jit::Run.
constexpr std::array HaltingCalls{
    std::string_view{"nvnQueueSubmitCommands"}, std::string_view{"nvnQueuePresentTexture"},
    std::string_view{"nvnQueueFinish"},         std::string_view{"nvnSyncWait"},
    std::string_view{"nvnWindowAcquireTexture"}, std::string_view{"nvnQueueAcquireTexture"},
    std::string_view{"nvnMemoryPoolFinalize"},
};

struct DeviceIntegerFrame {
    u64 lr;
    u32 item;
    u64 out;
};

/// One host thread's counters (a core's thread runs one guest thread at a time: no atomics).
struct Counters {
    std::array<u64, MaxSlots> calls{};
    std::vector<u64> emitted_headers; ///< by key, EDEN_NX2_NVN_EMITTED only
    u64 decoded_calls{};
    u64 emitted_words{};
    u64 skipped_calls{}; ///< the write pointer moved to another chunk
    // EDEN_NX2_NVN_HLE=BindTexture
    u64 hle_calls{};      ///< written as a marker
    u64 hle_fallbacks{};  ///< left to the driver: compute/task stage, unlearned, no room
    u64 verify_checked{}; ///< EDEN_NX2_NVN_HLE_VERIFY: driver output compared with the marker's
    u64 verify_mismatches{};
};

struct LookupFrame {
    std::string name;
    u64 lr;
};

/// The words the driver's nvnCommandBufferBindTexture writes for a graphics stage, at most.
using BindTextureWords = u32[5];

struct TraceFrame {
    u64 lr;
    u64 seq;
    u32 slot;
    u64 output; ///< the guest address of the output the trace records at the return
};

struct CommandFrame {
    u64 lr;
    u64 command_buffer;
    u64 write_pointer;
    bool decode;               ///< EDEN_NX2_NVN_EMITTED, top level
    u32 learn_stage = ~0u;     ///< BindTexture: take macro 0xe1c's argument for this stage
    u32 expected_count{};      ///< BindTexture verify: the words the marker stands for
    BindTextureWords expected{};
};

constexpr u32 MaxStages = 16;
constexpr u32 ComputeStage = 5;
constexpr u32 TaskStage = 8; ///< the driver's other special case besides compute

struct State {
    bool direct{}; ///< DirectMode()
    bool emitted{};
    bool hle_bind_texture{};
    bool hle_verify{};
    u32 bind_texture_slot = ~0u;
    /// Macro 0xe1c's argument per stage, as the driver wrote it (0 until seen; bit 31 set)
    std::array<std::atomic<u32>, MaxStages> bind_texture_argument{};
    std::optional<u64> private0_value;
    u64 every = 300;
    std::FILE* out{};
    /// EDEN_NX2_NVN_DRIVER_FILE: the NVN driver's entry points and device integers, written in
    /// pass-through and read in direct mode.
    std::string driver_file_path;
    std::FILE* driver_file{};
    std::map<u64, std::string> modules; ///< by base
    u32 device_integer_slot = ~0u;
    std::unordered_map<u32, s32> device_integers; ///< captured (pass-through) or read (direct)
    std::array<std::vector<DeviceIntegerFrame>, GuestThreadSlots> device_integer_frames;
    // Direct mode
    bool direct_ready{};                     ///< nvn_direct_init ran
    std::vector<std::pair<u64, std::string>> driver_entries; ///< hooked to prove none runs
    std::atomic<u64> driver_entry_calls{};
    std::atomic<u64> halted_calls{};
    std::chrono::steady_clock::time_point last_present{};

    Kernel::KProcess* process{};
    u64 synthetic_base{};
    /// nvnBootstrapLoader, every module's export: LM3 (Switch 1) exports it from a subsdk
    /// (nnSdkEn) and from nnSdk (dotfiles-1gi).
    std::vector<u64> bootstrap_entries;
    u64 lookup_driver{};   ///< nvnDeviceGetProcAddress, as nvnBootstrapLoader returned it
    u64 private0_address{};
    u64 private0_size{};

    std::array<Slot, MaxSlots> slots{};
    std::array<std::atomic<u64>, MaxSlots> stub_calls{}; ///< direct mode
    std::atomic<u32> slot_count{FirstNameSlot};

    std::mutex mutex; ///< slots' creation, lookups, the counters' list, the output
    std::unordered_map<std::string, u32> slot_by_name;
    std::unordered_map<u32, std::vector<LookupFrame>> lookups; ///< by guest thread
    std::vector<Counters*> counters;
    /// EDEN_NX2_NVN_EMITTED: open command-buffer calls by guest thread (id modulo the size)
    std::array<std::vector<CommandFrame>, GuestThreadSlots> command_frames;
    /// EDEN_NX2_NVN_TRACE: calls whose return the trace wants, by guest thread
    std::array<std::vector<TraceFrame>, GuestThreadSlots> trace_frames;
};

bool EnvIsOne(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

State* Create() {
    if (!EnvIsOne("EDEN_NX2_NVN_HOOK") && !EnvIsOne("EDEN_NX2_NVN_DIRECT")) {
        return nullptr;
    }
    auto* state = new State;
    if (const char* path = std::getenv("EDEN_NX2_NVN_OUT")) {
        state->out = std::fopen(path, "w");
    }
    if (const char* every = std::getenv("EDEN_NX2_NVN_EVERY")) {
        state->every = std::max<u64>(std::strtoull(every, nullptr, 10), 1);
    }
    state->emitted = std::getenv("EDEN_NX2_NVN_EMITTED") != nullptr;
    VideoCommon::NvnHeaderCount::g_enabled = state->emitted;
    if (const char* hle = std::getenv("EDEN_NX2_NVN_HLE")) {
        state->hle_bind_texture = std::strstr(hle, "BindTexture") != nullptr;
    }
    state->hle_verify = std::getenv("EDEN_NX2_NVN_HLE_VERIFY") != nullptr;
    if (const char* path = std::getenv("EDEN_NX2_NVN_DRIVER_FILE")) {
        state->driver_file_path = path;
    }
    // Verify keeps the driver's words: no marker reaches the front.
    VideoCommon::NvnHleMarker::g_enabled = state->hle_bind_texture && !state->hle_verify;
    if (const char* value = std::getenv("EDEN_NX2_NVN_PRIVATE0")) {
        state->private0_value = std::strtoull(value, nullptr, 0);
    }
    return state;
}

State* Get() {
    static State* const state = Create();
    return state;
}

Counters& LocalCounters(State& state) {
    static thread_local Counters* counters = nullptr;
    if (counters == nullptr) {
        counters = new Counters;
        if (state.emitted) {
            counters->emitted_headers.resize(1u << 16);
        }
        std::scoped_lock lock{state.mutex};
        state.counters.push_back(counters);
    }
    return *counters;
}

u64 SyntheticAddress(const State& state, u32 slot) {
    return state.synthetic_base + u64{slot} * 4;
}

bool IsBootstrap(const State& state, u64 address) {
    return std::ranges::find(state.bootstrap_entries, address) != state.bootstrap_entries.end();
}

bool IsDriverEntry(const State& state, u64 address) {
    return std::ranges::binary_search(state.driver_entries, std::pair{address, std::string{}},
                                      [](const auto& a, const auto& b) { return a.first < b.first; });
}

BlockEntryHook HookOf(u64 pc) {
    const State& state = *Get();
    if (IsBootstrap(state, pc)) {
        // Direct mode: nvnBootstrapLoader is answered here and the driver's never runs.
        return state.direct ? BlockEntryHook::Thunk : BlockEntryHook::Call;
    }
    if (state.direct && IsDriverEntry(state, pc)) {
        return BlockEntryHook::Call;
    }
    if (pc >= state.synthetic_base && (pc & 3) == 0 && state.synthetic_base != 0 &&
        (pc - state.synthetic_base) / 4 < state.slot_count.load(std::memory_order_acquire)) {
        return BlockEntryHook::Thunk;
    }
    return BlockEntryHook::None;
}

/// The slot of `name`, made on first sight (under state.mutex).
u32 SlotOf(State& state, const std::string& name, u64 driver) {
    if (const auto it = state.slot_by_name.find(name); it != state.slot_by_name.end()) {
        if (state.slots[it->second].driver != driver) {
            LOG_WARNING(Core, "NVN hook: {} resolved to {:#x}, earlier {:#x}", name, driver,
                        state.slots[it->second].driver);
        }
        return it->second;
    }
    const u32 slot = state.slot_count.load(std::memory_order_relaxed);
    if (slot == MaxSlots) {
        return MaxSlots;
    }
    state.slots[slot] = {.driver = driver,
                         .command_buffer = name.starts_with("nvnCommandBuffer")};
    state.slot_by_name.emplace(name, slot);
    if (name == "nvnCommandBufferBindTexture") {
        state.bind_texture_slot = slot;
    }
    if (name == "nvnDeviceGetInteger") {
        state.device_integer_slot = slot;
    }
    state.slot_count.store(slot + 1, std::memory_order_release);
    if (state.driver_file != nullptr && driver != 0) {
        // The entry as module + offset: the driver's addresses in a later run of the same game.
        const auto module = state.modules.upper_bound(driver);
        if (module != state.modules.begin()) {
            const auto& [base, module_name] = *std::prev(module);
            fmt::print(state.driver_file, "e {} {} {:#x}\n", name, module_name, driver - base);
            std::fflush(state.driver_file);
        }
    }
    NvnTrace::OnName(slot, name.c_str());
    if (state.out != nullptr) {
        fmt::print(state.out, "n {} {:#x} {:#x} {}\n", slot, driver, SyntheticAddress(state, slot),
                   name);
        std::fflush(state.out);
    }
    return slot;
}

void WritePrivate0(const State& state, Core::Memory::Memory& memory) {
    if (!state.private0_value || state.private0_address == 0) {
        return;
    }
    const u64 value = *state.private0_value;
    switch (state.private0_size) {
    case 1:
        memory.Write8(state.private0_address, static_cast<u8>(value));
        break;
    case 2:
        memory.Write16(state.private0_address, static_cast<u16>(value));
        break;
    case 8:
        memory.Write64(state.private0_address, value);
        break;
    default:
        memory.Write32(state.private0_address, static_cast<u32>(value));
        break;
    }
}

u64 ReadPrivate0(const State& state, Core::Memory::Memory& memory) {
    switch (state.private0_size) {
    case 1:
        return memory.Read8(state.private0_address);
    case 2:
        return memory.Read16(state.private0_address);
    case 8:
        return memory.Read64(state.private0_address);
    default:
        return memory.Read32(state.private0_address);
    }
}

bool ReadGuest(Core::Memory::Memory& memory, u64 address, void* out, size_t size) {
    auto* bytes = static_cast<u8*>(out);
    while (size != 0) {
        const size_t chunk = std::min<size_t>(size, 0x1000 - (address & 0xfff));
        const u8* const pointer = memory.GetPointerSilent(Common::ProcessAddress{address});
        if (pointer == nullptr) {
            return false;
        }
        std::memcpy(bytes, pointer, chunk);
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return true;
}

/// The method headers a top-level command-buffer call wrote between `begin` and `end`.
void DecodeEmitted(Counters& counters, Core::Memory::Memory& memory, u64 begin, u64 end) {
    if (begin == 0 || end < begin || end - begin > 0x10000) {
        ++counters.skipped_calls;
        return;
    }
    ++counters.decoded_calls;
    const size_t count = static_cast<size_t>((end - begin) / 4);
    counters.emitted_words += count;
    if (count == 0) {
        return;
    }
    static thread_local std::vector<u32> words;
    words.resize(count);
    if (!ReadGuest(memory, begin, words.data(), count * 4)) {
        return;
    }
    for (size_t i = 0; i < count;) {
        const u32 header = words[i];
        const u32 mode = header >> 29;
        u32 data;
        if (mode == 1 || mode == 3 || mode == 5) {
            data = (header >> 16) & 0x1fff;
        } else if (mode == 4) {
            data = 0;
        } else {
            break;
        }
        // The header's method field is in words; subchannel bits 13-15.
        const u32 key = (((header >> 13) & 7) << 13) | (header & 0x1fff);
        ++counters.emitted_headers[key];
        VideoCommon::NvnHeaderCount::NoteWritten(
            memory.GetPointerSilent(Common::ProcessAddress{begin + i * 4}), header);
        i += 1 + data;
    }
}

/// nvnCommandBufferBindTexture(cmdbuf x0, stage w1, slot w2, handle x3). Returns whether the
/// call was done here (a marker written, see nvn_hle_marker.h); otherwise the driver runs it,
/// with `frame` (when set up) checked at its return.
bool HleBindTexture(State& state, Counters& counters, Dynarmic::A64::Jit& jit,
                    Core::Memory::Memory& memory, CommandFrame& frame, bool& want_return) {
    const u64 command_buffer = jit.GetRegister(0);
    const u32 stage = static_cast<u32>(jit.GetRegister(1));
    const u32 slot = static_cast<u32>(jit.GetRegister(2));
    const u32 handle = static_cast<u32>(jit.GetRegister(3));
    if (stage == ComputeStage || stage == TaskStage || stage >= MaxStages || slot > 0xff) {
        ++counters.hle_fallbacks;
        return false;
    }
    const u32 learned = state.bind_texture_argument[stage].load(std::memory_order_relaxed);
    if (learned == 0) {
        frame.learn_stage = stage;
        want_return = true;
        ++counters.hle_fallbacks;
        return false;
    }
    namespace Marker = VideoCommon::NvnHleMarker;
    // pipe_nop: the driver's IMMD 0x68b, written when cmdbuf+0x13 bit 3 is set (sdk BindTexture)
    const bool pipe_nop = (memory.Read8(command_buffer + 0x13) & 8) != 0;
    const u32 word0 = Marker::BindTextureWord0(slot, stage, pipe_nop, learned);
    if (state.hle_verify) {
        // What the front would replay from this very marker, against the driver's words.
        frame.expected_count = Marker::ExpandBindTexture(word0, handle, frame.expected);
        want_return = true;
        return false;
    }
    const u64 write_pointer = memory.Read64(command_buffer);
    const u64 end = memory.Read64(command_buffer + 8);
    if (write_pointer == 0 || write_pointer + 12 > end) {
        ++counters.hle_fallbacks; // the driver asks the game for more memory
        return false;
    }
    const std::array<u32, 3> words{Marker::Header(Marker::BindTexture, 2), word0, handle};
    memory.WriteBlock(write_pointer, words.data(), sizeof(words));
    memory.Write64(command_buffer, write_pointer + sizeof(words));
    ++counters.hle_calls;
    return true;
}

/// At a BindTexture's return: learn the stage's macro argument, or compare with the marker.
void CheckBindTexture(State& state, Counters& counters, Core::Memory::Memory& memory,
                      const CommandFrame& frame) {
    const u64 end = memory.Read64(frame.command_buffer);
    if (frame.write_pointer == 0 || end <= frame.write_pointer ||
        end - frame.write_pointer > 5 * 4) {
        return; // another chunk (the driver asked for memory first): not comparable
    }
    u32 written[5]{};
    const u32 count = static_cast<u32>((end - frame.write_pointer) / 4);
    ReadGuest(memory, frame.write_pointer, written, count * 4);
    if (frame.learn_stage != ~0u) {
        if ((written[0] & 0xe000ffffu) == 0x80000e1cu) {
            state.bind_texture_argument[frame.learn_stage].store(
                0x80000000u | ((written[0] >> 16) & 0x1fff), std::memory_order_relaxed);
        }
        return;
    }
    ++counters.verify_checked;
    if (count != frame.expected_count ||
        !std::equal(written, written + count, frame.expected)) {
        if (counters.verify_mismatches++ < 16) {
            LOG_ERROR(Core, "NVN HLE verify: BindTexture wrote {} words {:08x} {:08x} {:08x} {:08x} "
                            "{:08x}, the marker stands for {} words {:08x} {:08x} {:08x} {:08x} {:08x}",
                      count, written[0], written[1], written[2], written[3], written[4],
                      frame.expected_count, frame.expected[0], frame.expected[1],
                      frame.expected[2], frame.expected[3], frame.expected[4]);
        }
    }
}

/// Direct mode, at Install: a slot per name the front end knows (upstream's names.txt), the
/// driver's entry points from a pass-through run's capture, hooked only to count, and its
/// device integers.
void InstallDirect(State& state) {
#ifdef LICHII_NVN_DIRECT
    for (int front = 0; front < nvn_direct_name_count(); ++front) {
        const std::string name = nvn_direct_name(front);
        const u32 slot = state.slot_count.load(std::memory_order_relaxed);
        state.slots[slot] = {.command_buffer = name.starts_with("nvnCommandBuffer"), .front = front,
                             .halts = std::ranges::find(HaltingCalls, name) != HaltingCalls.end()};
        if (state.slots[slot].halts && nvn_direct_float_args(front)) {
            LOG_CRITICAL(Core, "NVN direct: {} takes floats, which a halted call loses", name);
        }
        state.slot_by_name.emplace(name, slot);
        state.slot_count.store(slot + 1, std::memory_order_release);
    }
    // The game's inline NVN encoding stays off: its commands come through the table (nvnPrivate0
    // is 1 only once the driver's lookup ran, which it never does now; written at each lookup).
    state.private0_value = state.private0_value.value_or(0);
    std::FILE* file = state.driver_file_path.empty()
                          ? nullptr
                          : std::fopen(state.driver_file_path.c_str(), "r");
    if (file == nullptr) {
        LOG_WARNING(Core, "NVN direct: no driver capture (EDEN_NX2_NVN_DRIVER_FILE): driver entries "
                          "are not watched, device integers are the front end's own");
    } else {
        char line[512];
        while (std::fgets(line, sizeof line, file) != nullptr) {
            char name[256], module[64];
            unsigned long long offset{};
            unsigned item{};
            int value{};
            if (std::sscanf(line, "e %255s %63s %llx", name, module, &offset) == 3) {
                for (const auto& [base, module_name] : state.modules) {
                    if (module_name == module) {
                        state.driver_entries.emplace_back(base + offset, name);
                    }
                }
            } else if (std::sscanf(line, "i %u %d", &item, &value) == 2) {
                state.device_integers[item] = value;
                nvn_direct_set_device_integer(item, value);
            }
        }
        std::fclose(file);
        std::ranges::sort(state.driver_entries);
    }
    LOG_INFO(Core,
             "NVN direct: the game's NVN driver is replaced: {} names in the front end, {} driver "
             "entries watched, {} device integers from the driver",
             state.slot_count.load() - FirstNameSlot, state.driver_entries.size(),
             state.device_integers.size());
#endif
}

#ifdef LICHII_NVN_DIRECT
/// Direct mode: the slot answering `name` (a stub slot for a name the front end lacks).
u32 DirectSlotOf(State& state, const std::string& name) {
    if (name == "nvnDeviceGetProcAddress") {
        return LookupSlot;
    }
    if (const auto it = state.slot_by_name.find(name); it != state.slot_by_name.end()) {
        return it->second;
    }
    const u32 slot = state.slot_count.load(std::memory_order_relaxed);
    if (slot == MaxSlots) {
        return MaxSlots;
    }
    state.slots[slot] = {.command_buffer = name.starts_with("nvnCommandBuffer")};
    state.slot_by_name.emplace(name, slot);
    state.slot_count.store(slot + 1, std::memory_order_release);
    LOG_WARNING(Core, "NVN direct: the game asks for {}, which the front end lacks: a stub", name);
    return slot;
}

/// The guest's x0-x7 and the low halves of v0-v7.
void ReadArguments(Dynarmic::A64::Jit& jit, std::array<u64, 8>& x, std::array<u64, 8>& v) {
    for (size_t i = 0; i < 8; ++i) {
        x[i] = jit.GetRegister(i);
        v[i] = jit.GetVector(i)[0];
    }
}

/// The slot of a call that halted the JIT (HaltedCallSvc), on the host thread that runs it: the
/// kernel runs it (RunHaltedCall) right after the JIT returns, on the same host thread.
thread_local u32 t_halted_slot = MaxSlots;
thread_local u64 t_halted_lr = 0;

/// Direct mode's block entries. Returns whether the JIT halts for the call (HaltedCallSvc).
bool DirectEntry(State& state, u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory) {
    const u64 lr = jit.GetRegister(30);
    if (IsDriverEntry(state, pc)) {
        const u64 calls = state.driver_entry_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (calls <= 8) {
            const auto it = std::ranges::lower_bound(state.driver_entries,
                                                     std::pair{pc, std::string{}});
            LOG_CRITICAL(Core, "NVN direct: the game's NVN driver ran: {} at {:#x} (from {:#x})",
                         it != state.driver_entries.end() ? it->second : "?", pc, lr);
        }
        return false;
    }
    const bool bootstrap = IsBootstrap(state, pc);
    const u32 slot = bootstrap ? MaxSlots : static_cast<u32>((pc - state.synthetic_base) / 4);
    if (bootstrap || slot == LookupSlot) {
        // nvnBootstrapLoader(name) or nvnDeviceGetProcAddress(device, name): the name's
        // synthetic address, without the driver.
        const std::string name = memory.ReadCString(jit.GetRegister(bootstrap ? 0 : 1), 128);
        std::scoped_lock lock{state.mutex};
        if (!state.direct_ready) {
            auto& page_table = state.process->GetPageTable();
            nvn_direct_init(page_table.GetImpl().fastmem_arena,
                            u64{1} << page_table.GetAddressSpaceWidth());
            state.direct_ready = true;
        }
        const u32 found = DirectSlotOf(state, name);
        jit.SetRegister(0, found == MaxSlots ? 0 : SyntheticAddress(state, found));
        WritePrivate0(state, memory);
        jit.SetRegister(16, lr);
        return false;
    }
    ++LocalCounters(state).calls[slot];
    const Slot& target = state.slots[slot];
    jit.SetRegister(16, lr);
    if (target.front < 0 || !nvn_direct_implemented(target.front)) {
        if (state.stub_calls[slot].fetch_add(1, std::memory_order_relaxed) == 0) {
            std::scoped_lock lock{state.mutex};
            for (const auto& [name, index] : state.slot_by_name) {
                if (index == slot) {
                    LOG_WARNING(Core, "NVN direct: {} has no implementation: answered 0", name);
                }
            }
        }
        jit.SetRegister(0, 0);
        return false;
    }
    if (target.halts) {
        t_halted_slot = slot;
        t_halted_lr = lr;
        return true;
    }
    std::array<u64, 8> x, v;
    ReadArguments(jit, x, v);
    u64 v0 = 0;
    jit.SetRegister(0, nvn_direct_call(target.front, x.data(), v.data(), lr, &v0));
    if (nvn_direct_float_args(target.front)) {
        jit.SetVector(0, {v0, 0});
    }
    return false;
}
#endif

} // namespace

bool Enabled() {
    return Get() != nullptr;
}

bool DirectMode() {
    const State* const state = Get();
    return state != nullptr && state->direct;
}

void Install(Kernel::KProcess& process) {
    State* const state_pointer = Get();
    if (state_pointer == nullptr) {
        return;
    }
    State& state = *state_pointer;
    auto& memory = process.GetMemory();
    const auto modules = Core::FindModules(&process);
    for (const auto& [base, module] : modules) {
        state.modules.emplace(base, module);
        const auto symbols = Core::Symbols::GetSymbols(base, memory, true);
        const auto find = [&](std::string_view name) -> std::pair<u64, u64> {
            const auto it = symbols.find(name);
            if (it == symbols.end() || it->second.first == 0) {
                return {0, 0};
            }
            return {base + it->second.first, it->second.second};
        };
        if (const auto [address, size] = find("nvnBootstrapLoader"); address != 0) {
            state.bootstrap_entries.push_back(address);
            LOG_INFO(Core, "NVN hook: nvnBootstrapLoader in module {} at {:#x}: {:#x}", module,
                     base, address);
        }
        if (const auto [address, size] = find("nvnPrivate0"); address != 0) {
            state.private0_address = address;
            state.private0_size = size;
        }
    }
    // The synthetic addresses: in the first unmapped range from 64 KiB that holds every slot.
    // Nothing is ever mapped executable there, and the JIT reads no guest code for them.
    auto& page_table = process.GetPageTable();
    for (u64 address = 0x10000;;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        if (page_table.QueryInfo(std::addressof(info), std::addressof(page), address).IsError()) {
            break;
        }
        const auto svc = info.GetSvcMemoryInfo();
        const u64 end = svc.base_address + svc.size;
        if (svc.state == Kernel::Svc::MemoryState::Free && end - address >= u64{MaxSlots} * 4) {
            state.synthetic_base = address;
            break;
        }
        if (end <= address) {
            break;
        }
        address = end;
    }
    state.process = &process;
#ifdef LICHII_NVN_DIRECT
    // One launch per process: the direct renderer's state lives as long as the process does.
    state.direct = EnvIsOne("EDEN_NX2_NVN_DIRECT") && !state.bootstrap_entries.empty();
#endif
    if (state.direct) {
        InstallDirect(state);
    } else if (!state.driver_file_path.empty()) {
        state.driver_file = std::fopen(state.driver_file_path.c_str(), "w");
    }
    NvnTrace::Install(process);
    LOG_INFO(Core,
             "NVN hook: {} nvnBootstrapLoader exports, nvnPrivate0 {:#x} ({} bytes), synthetic "
             "addresses from {:#x}",
             state.bootstrap_entries.size(), state.private0_address,
             state.private0_size, state.synthetic_base);
}

Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc) {
    return Get() != nullptr ? &HookOf : nullptr;
}

bool OnBlockEntry(u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
                  Kernel::KProcess* process, u32 thread_id) {
    State& state = *Get();
    if (process != state.process) {
        return false;
    }
#ifdef LICHII_NVN_DIRECT
    if (state.direct) {
        return DirectEntry(state, pc, jit, memory);
    }
#endif
    const u32 slot = IsBootstrap(state, pc)
                         ? MaxSlots
                         : static_cast<u32>((pc - state.synthetic_base) / 4);
    if (slot == LookupSlot) {
        ++LocalCounters(state).calls[LookupSlot];
    }
    if (slot == MaxSlots || slot == LookupSlot) {
        // nvnBootstrapLoader(name) or nvnDeviceGetProcAddress(device, name): return through
        // LookupReturnSlot, which swaps the driver's address for the name's synthetic one.
        const u64 name_address = jit.GetRegister(slot == LookupSlot ? 1 : 0);
        std::scoped_lock lock{state.mutex};
        state.lookups[thread_id].push_back(
            {.name = memory.ReadCString(name_address, 128), .lr = jit.GetRegister(30)});
        jit.SetRegister(30, SyntheticAddress(state, LookupReturnSlot));
        if (slot == LookupSlot) {
            jit.SetRegister(16, state.lookup_driver);
        }
        return false;
    }
    if (slot == LookupReturnSlot) {
        std::scoped_lock lock{state.mutex};
        auto& frames = state.lookups[thread_id];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: lookup return without a lookup on thread {}", thread_id);
            return false;
        }
        const LookupFrame frame = std::move(frames.back());
        frames.pop_back();
        const u64 driver = jit.GetRegister(0);
        // A loader export that forwards to another hooked one returns its synthetic address.
        const bool synthetic =
            driver >= state.synthetic_base &&
            (driver - state.synthetic_base) / 4 < state.slot_count.load(std::memory_order_relaxed);
        if (!synthetic && frame.name == "nvnDeviceGetProcAddress" && driver != 0) {
            if (state.lookup_driver != 0 && state.lookup_driver != driver) {
                LOG_WARNING(Core, "NVN hook: a second nvnDeviceGetProcAddress {:#x}", driver);
            }
            if (state.lookup_driver == 0 && state.out != nullptr) {
                fmt::print(state.out, "n {} {:#x} {:#x} {}\n", LookupSlot, driver,
                           SyntheticAddress(state, LookupSlot), frame.name);
            }
            state.lookup_driver = driver;
            jit.SetRegister(0, SyntheticAddress(state, LookupSlot));
        } else if (!synthetic && driver != 0 && !IsBootstrap(state, driver)) {
            const u32 name_slot = SlotOf(state, frame.name, driver);
            if (name_slot != MaxSlots) {
                jit.SetRegister(0, SyntheticAddress(state, name_slot));
            }
        }
        WritePrivate0(state, memory);
        jit.SetRegister(16, frame.lr);
        return false;
    }
    if (slot == TraceReturnSlot) {
        auto& frames = state.trace_frames[thread_id % GuestThreadSlots];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: trace return without a call on thread {}", thread_id);
            return false;
        }
        const TraceFrame frame = frames.back();
        frames.pop_back();
        NvnTrace::OnReturn(frame.seq, frame.slot, frame.output, jit, memory);
        jit.SetRegister(16, frame.lr);
        return false;
    }
    if (slot == DeviceIntegerReturnSlot) {
        auto& frames = state.device_integer_frames[thread_id % GuestThreadSlots];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: GetInteger return without a call on thread {}", thread_id);
            return false;
        }
        const DeviceIntegerFrame frame = frames.back();
        frames.pop_back();
        const s32 value = static_cast<s32>(memory.Read32(frame.out));
        std::scoped_lock lock{state.mutex};
        if (state.device_integers.emplace(frame.item, value).second) {
            fmt::print(state.driver_file, "i {} {}\n", frame.item, value);
            std::fflush(state.driver_file);
        }
        jit.SetRegister(16, frame.lr);
        return false;
    }
    if (slot == EmittedReturnSlot) {
        auto& frames = state.command_frames[thread_id % GuestThreadSlots];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: command return without a call on thread {}", thread_id);
            return false;
        }
        const CommandFrame frame = frames.back();
        frames.pop_back();
        Counters& counters = LocalCounters(state);
        if (frame.decode && frames.empty()) {
            DecodeEmitted(counters, memory, frame.write_pointer, memory.Read64(frame.command_buffer));
        }
        if (frame.learn_stage != ~0u || frame.expected_count != 0) {
            CheckBindTexture(state, counters, memory, frame);
        }
        jit.SetRegister(16, frame.lr);
        return false;
    }
    Counters& counters = LocalCounters(state);
    ++counters.calls[slot];
    const Slot& target = state.slots[slot];
    CommandFrame frame{.lr = jit.GetRegister(30), .decode = state.emitted && target.command_buffer};
    bool want_return = frame.decode;
    if (slot == state.bind_texture_slot && state.hle_bind_texture &&
        HleBindTexture(state, counters, jit, memory, frame, want_return)) {
        jit.SetRegister(16, frame.lr); // done: back to the caller
        return false;
    }
    if (slot == state.device_integer_slot && state.driver_file != nullptr) {
        // nvnDeviceGetInteger(device, item, int* out): the answer at the return (pass-through
        // runs only; the trace misses these calls then).
        state.device_integer_frames[thread_id % GuestThreadSlots].push_back(
            {.lr = jit.GetRegister(30),
             .item = static_cast<u32>(jit.GetRegister(1)),
             .out = jit.GetRegister(2)});
        jit.SetRegister(30, SyntheticAddress(state, DeviceIntegerReturnSlot));
        jit.SetRegister(16, target.driver);
        return false;
    }
    if (u64 seq = 0; NvnTrace::OnCall(slot, jit, memory, thread_id, seq)) {
        // The trace's return frame stands alone: a trace run sets neither EDEN_NX2_NVN_EMITTED
        // nor EDEN_NX2_NVN_HLE.
        const u32 output = NvnTrace::OutputArgument(slot);
        state.trace_frames[thread_id % GuestThreadSlots].push_back(
            {.lr = jit.GetRegister(30),
             .seq = seq,
             .slot = slot,
             .output = output != ~0u ? jit.GetRegister(output) : 0});
        jit.SetRegister(30, SyntheticAddress(state, TraceReturnSlot));
        jit.SetRegister(16, target.driver);
        return false;
    }
    if (want_return) {
        frame.command_buffer = jit.GetRegister(0);
        frame.write_pointer = memory.Read64(frame.command_buffer);
        state.command_frames[thread_id % GuestThreadSlots].push_back(frame);
        jit.SetRegister(30, SyntheticAddress(state, EmittedReturnSlot));
    }
    jit.SetRegister(16, target.driver);
    return false;
}

void RunHaltedCall(Core::System& system, std::array<u64, 8>& args) {
#ifdef LICHII_NVN_DIRECT
    State& state = *Get();
    const u32 slot = std::exchange(t_halted_slot, MaxSlots);
    if (slot == MaxSlots) {
        LOG_CRITICAL(Core, "NVN direct: a halted call without its slot");
        return;
    }
    state.halted_calls.fetch_add(1, std::memory_order_relaxed);
    const Slot& target = state.slots[slot];
    // No halting call takes or returns floats (checked at the first one).
    const std::array<u64, 8> v{};
    u64 v0 = 0;
    args[0] = nvn_direct_call(target.front, args.data(), v.data(), t_halted_lr, &v0);
    if (std::string_view{nvn_direct_name(target.front)} != "nvnQueuePresentTexture") {
        return;
    }
    // The present's bookkeeping, as nx2:vi2's PresentClientBuffer does it for the driver's
    // presents, and pacing to 60 Hz with a kernel sleep (the null backend shows nothing yet).
    const u64 present = Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (present <= 4 || present % 600 == 0) {
        LOG_INFO(Core, "NVN direct: present #{}", present);
    }
    auto& thread = Kernel::GetCurrentThread(system.Kernel());
    if (Kernel::Nx2ThreadStats::g_enabled) {
        Kernel::Nx2ThreadStats::OnPresent(&thread);
    }
    OnPresent(present);
    constexpr auto Period = std::chrono::nanoseconds{16'666'667};
    const auto now = std::chrono::steady_clock::now();
    const auto due = state.last_present + Period;
    if (now < due) {
        Kernel::Svc::SleepThread(system, (due - now).count());
        state.last_present = due;
    } else {
        state.last_present = now;
    }
#else
    (void)system;
    (void)args;
#endif
}

void OnPresent(u64 present) {
    NvnTrace::OnPresent(present);
    State* const state_pointer = Get();
    if (state_pointer != nullptr && state_pointer->direct && present % 600 == 0) {
        // Direct mode: the stub census (names called without an implementation) and proof the
        // driver did not run.
        State& state = *state_pointer;
        std::string census;
        std::scoped_lock lock{state.mutex};
        for (const auto& [name, slot] : state.slot_by_name) {
            if (const u64 calls = state.stub_calls[slot].load(std::memory_order_relaxed); calls) {
                census += fmt::format(" {}={}", name, calls);
            }
        }
        LOG_INFO(Core,
                 "NVN direct: present {}: driver entries run {}, halted calls {}, stubs called:{}",
                 present, state.driver_entry_calls.load(), state.halted_calls.load(),
                 census.empty() ? " none" : census);
    }
    if (state_pointer == nullptr || state_pointer->out == nullptr ||
        present % state_pointer->every != 0) {
        return;
    }
    State& state = *state_pointer;
    std::string text = fmt::format(
        "t {} {}\n", present,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::scoped_lock lock{state.mutex};
    const u32 slots = state.slot_count.load(std::memory_order_acquire);
    for (u32 slot = LookupSlot; slot < slots; ++slot) {
        u64 calls = 0;
        for (const Counters* counters : state.counters) {
            calls += counters->calls[slot];
        }
        if (calls != 0) {
            text += fmt::format("c {} {} {}\n", present, slot, calls);
        }
    }
    if (state.emitted) {
        u64 decoded = 0, words = 0, skipped = 0;
        for (const Counters* counters : state.counters) {
            decoded += counters->decoded_calls;
            words += counters->emitted_words;
            skipped += counters->skipped_calls;
        }
        text += fmt::format("ew {} {} {} {}\n", present, decoded, words, skipped);
        for (u32 key = 0; key < (1u << 16); ++key) {
            u64 headers = 0;
            for (const Counters* counters : state.counters) {
                headers += counters->emitted_headers[key];
            }
            if (headers != 0) {
                text += fmt::format("e {} {:#x} {}\n", present, key, headers);
            }
            if (const u64 front = VideoCommon::NvnHeaderCount::g_headers[key]; front != 0) {
                text += fmt::format("f {} {:#x} {} {}\n", present, key, front,
                                    VideoCommon::NvnHeaderCount::g_table_headers[key]);
            }
        }
    }
    if (state.hle_bind_texture) {
        u64 hle = 0, fallbacks = 0, checked = 0, mismatches = 0;
        for (const Counters* counters : state.counters) {
            hle += counters->hle_calls;
            fallbacks += counters->hle_fallbacks;
            checked += counters->verify_checked;
            mismatches += counters->verify_mismatches;
        }
        text += fmt::format("h {} {} {} {} {}\n", present, hle, fallbacks, checked, mismatches);
    }
    if (state.private0_address != 0 && state.process != nullptr) {
        text += fmt::format("p0 {} {}\n", present,
                            ReadPrivate0(state, state.process->GetMemory()));
    }
    std::fwrite(text.data(), 1, text.size(), state.out);
    std::fflush(state.out);
}

} // namespace Core::NvnHook
