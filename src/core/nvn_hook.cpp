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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/arm/symbols.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nvn_hook.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "video_core/nvn_header_count.h"
#include "video_core/nvn_hle_marker.h"

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
constexpr u32 FirstNameSlot = 3;
constexpr u32 GuestThreadSlots = 4096;

struct Slot {
    u64 driver{};
    bool command_buffer{};
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
using BindTextureWords = std::array<u32, 5>;

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
    bool emitted{};
    bool hle_bind_texture{};
    bool hle_verify{};
    u32 bind_texture_slot = ~0u;
    /// Macro 0xe1c's argument per stage, as the driver wrote it (0 until seen; bit 31 set)
    std::array<std::atomic<u32>, MaxStages> bind_texture_argument{};
    std::optional<u64> private0_value;
    u64 every = 300;
    std::FILE* out{};

    Kernel::KProcess* process{};
    u64 synthetic_base{};
    u64 bootstrap_entry{}; ///< nvnBootstrapLoader (exported)
    u64 lookup_driver{};   ///< nvnDeviceGetProcAddress, as nvnBootstrapLoader returned it
    u64 private0_address{};
    u64 private0_size{};

    std::array<Slot, MaxSlots> slots{};
    std::atomic<u32> slot_count{FirstNameSlot};

    std::mutex mutex; ///< slots' creation, lookups, the counters' list, the output
    std::unordered_map<std::string, u32> slot_by_name;
    std::unordered_map<u32, std::vector<LookupFrame>> lookups; ///< by guest thread
    std::vector<Counters*> counters;
    /// EDEN_NX2_NVN_EMITTED: open command-buffer calls by guest thread (id modulo the size)
    std::array<std::vector<CommandFrame>, GuestThreadSlots> command_frames;
};

State* Create() {
    const char* hook = std::getenv("EDEN_NX2_NVN_HOOK");
    if (hook == nullptr || std::strcmp(hook, "1") != 0) {
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

BlockEntryHook HookOf(u64 pc) {
    const State& state = *Get();
    if (pc == state.bootstrap_entry) {
        return pc != 0 ? BlockEntryHook::Call : BlockEntryHook::None;
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
    state.slot_count.store(slot + 1, std::memory_order_release);
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
        i += 1 + data;
    }
}

/// The driver's words for BindTexture on a graphics stage (sdk BindTexture, MKW and OoT: IMMD
/// 0xe1c = argument, IMMD 0x68b if cmdbuf+0x13 bit 3, INCR 0x8e3 {0x20 + 8 * slot, handle}).
u32 BindTextureExpansion(BindTextureWords& words, u32 argument, bool pipe_nop, u32 slot,
                         u32 handle) {
    u32 count = 0;
    words[count++] = 0x80000e1cu | (argument << 16);
    if (pipe_nop) {
        words[count++] = 0x8000068bu;
    }
    words[count++] = 0x200208e3u;
    words[count++] = 0x20 + 8 * slot;
    words[count++] = handle;
    return count;
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
    const u32 argument = learned & 0xffff;
    const bool pipe_nop = (memory.Read8(command_buffer + 0x13) & 8) != 0;
    if (state.hle_verify) {
        frame.expected_count = BindTextureExpansion(frame.expected, argument, pipe_nop, slot, handle);
        want_return = true;
        return false;
    }
    const u64 write_pointer = memory.Read64(command_buffer);
    const u64 end = memory.Read64(command_buffer + 8);
    if (write_pointer == 0 || write_pointer + 12 > end) {
        ++counters.hle_fallbacks; // the driver asks the game for more memory
        return false;
    }
    namespace Marker = VideoCommon::NvnHleMarker;
    const std::array<u32, 3> words{Marker::Header(Marker::BindTexture, 2),
                                   slot | (stage << 8) | (pipe_nop ? 1u << 12 : 0) |
                                       (argument << 16),
                                   handle};
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
    BindTextureWords written{};
    const u32 count = static_cast<u32>((end - frame.write_pointer) / 4);
    ReadGuest(memory, frame.write_pointer, written.data(), count * 4);
    if (frame.learn_stage != ~0u) {
        if ((written[0] & 0xe000ffffu) == 0x80000e1cu) {
            state.bind_texture_argument[frame.learn_stage].store(
                0x80000000u | (written[0] >> 16), std::memory_order_relaxed);
        }
        return;
    }
    ++counters.verify_checked;
    if (count != frame.expected_count ||
        !std::equal(written.begin(), written.begin() + count, frame.expected.begin())) {
        if (counters.verify_mismatches++ < 16) {
            LOG_ERROR(Core, "NVN HLE verify: BindTexture wrote {} words {:08x} {:08x} {:08x} {:08x} "
                            "{:08x}, the marker stands for {} words {:08x} {:08x} {:08x} {:08x} {:08x}",
                      count, written[0], written[1], written[2], written[3], written[4],
                      frame.expected_count, frame.expected[0], frame.expected[1],
                      frame.expected[2], frame.expected[3], frame.expected[4]);
        }
    }
}

} // namespace

bool Enabled() {
    return Get() != nullptr;
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
        const auto symbols = Core::Symbols::GetSymbols(base, memory, true);
        const auto find = [&](std::string_view name) -> std::pair<u64, u64> {
            const auto it = symbols.find(name);
            if (it == symbols.end() || it->second.first == 0) {
                return {0, 0};
            }
            return {base + it->second.first, it->second.second};
        };
        if (const auto [address, size] = find("nvnBootstrapLoader"); address != 0) {
            state.bootstrap_entry = address;
            LOG_INFO(Core, "NVN hook: nvnBootstrapLoader in module {} at {:#x}", module, base);
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
    LOG_INFO(Core,
             "NVN hook: nvnBootstrapLoader {:#x}, nvnPrivate0 {:#x} ({} bytes), synthetic "
             "addresses from {:#x}",
             state.bootstrap_entry, state.private0_address,
             state.private0_size, state.synthetic_base);
}

Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc) {
    return Get() != nullptr ? &HookOf : nullptr;
}

void OnBlockEntry(u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
                  Kernel::KProcess* process, u32 thread_id) {
    State& state = *Get();
    if (process != state.process) {
        return;
    }
    const u32 slot = pc == state.bootstrap_entry
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
        return;
    }
    if (slot == LookupReturnSlot) {
        std::scoped_lock lock{state.mutex};
        auto& frames = state.lookups[thread_id];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: lookup return without a lookup on thread {}", thread_id);
            return;
        }
        const LookupFrame frame = std::move(frames.back());
        frames.pop_back();
        const u64 driver = jit.GetRegister(0);
        if (frame.name == "nvnDeviceGetProcAddress" && driver != 0) {
            if (state.lookup_driver != 0 && state.lookup_driver != driver) {
                LOG_WARNING(Core, "NVN hook: a second nvnDeviceGetProcAddress {:#x}", driver);
            }
            if (state.lookup_driver == 0 && state.out != nullptr) {
                fmt::print(state.out, "n {} {:#x} {:#x} {}\n", LookupSlot, driver,
                           SyntheticAddress(state, LookupSlot), frame.name);
            }
            state.lookup_driver = driver;
            jit.SetRegister(0, SyntheticAddress(state, LookupSlot));
        } else if (driver != 0 && driver != state.bootstrap_entry) {
            const u32 name_slot = SlotOf(state, frame.name, driver);
            if (name_slot != MaxSlots) {
                jit.SetRegister(0, SyntheticAddress(state, name_slot));
            }
        }
        WritePrivate0(state, memory);
        jit.SetRegister(16, frame.lr);
        return;
    }
    if (slot == EmittedReturnSlot) {
        auto& frames = state.command_frames[thread_id % GuestThreadSlots];
        if (frames.empty()) {
            LOG_CRITICAL(Core, "NVN hook: command return without a call on thread {}", thread_id);
            return;
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
        return;
    }
    Counters& counters = LocalCounters(state);
    ++counters.calls[slot];
    const Slot& target = state.slots[slot];
    CommandFrame frame{.lr = jit.GetRegister(30), .decode = state.emitted && target.command_buffer};
    bool want_return = frame.decode;
    if (slot == state.bind_texture_slot && state.hle_bind_texture &&
        HleBindTexture(state, counters, jit, memory, frame, want_return)) {
        jit.SetRegister(16, frame.lr); // done: back to the caller
        return;
    }
    if (want_return) {
        frame.command_buffer = jit.GetRegister(0);
        frame.write_pointer = memory.Read64(frame.command_buffer);
        state.command_frames[thread_id % GuestThreadSlots].push_back(frame);
        jit.SetRegister(30, SyntheticAddress(state, EmittedReturnSlot));
    }
    jit.SetRegister(16, target.driver);
}

void OnPresent(u64 present) {
    State* const state_pointer = Get();
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
                text += fmt::format("f {} {:#x} {}\n", present, key, front);
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
