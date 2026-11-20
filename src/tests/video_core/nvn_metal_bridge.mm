// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>

#include "common/host_memory.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "nvn.h"
#include "nvn_direct_dispatch.h"

extern "C" void nvn_queue_test_hold(int on);
extern "C" u64 mtl_completed_fence();
extern "C" u64 mtl_last_fence();
extern "C" void mtl_drain();
extern "C" int mtl_addr(u64 address, u64 size, void** buffer, u64* offset);

namespace {

constexpr size_t Page = 16 * 1024;
constexpr size_t QueueMemorySize = 512 * 1024;
constexpr size_t ArenaSize = 8 * Page + QueueMemorySize;
constexpr u8 Guard = 0xA5;
constexpr u64 ObjectPage = Page;

struct Objects {
    NVNdeviceBuilder device_builder;
    NVNdevice device;
    NVNmemoryPoolBuilder pool_builder;
    NVNmemoryPool data_pool;
    NVNmemoryPool command_pool;
    NVNcommandBuffer command_buffer;
    NVNqueueBuilder queue_builder;
    NVNqueue queue;
    NVNsync sync;
    NVNcommandHandle handle;
    NVNtextureBuilder texture_builder;
    NVNtexture texture;
    u64 texture_address;
    std::array<float, 4> clear_color;
    NVNcopyRegion read_region;
};
static_assert(sizeof(Objects) <= 2 * Page);

void Check(bool ok, std::string_view step) {
    if (!ok) {
        std::fprintf(stderr, "NVN Metal bridge: %.*s\n", static_cast<int>(step.size()), step.data());
        std::exit(1);
    }
}

class GuestCalls final : public Dynarmic::A64::UserCallbacks {
public:
    explicit GuestCalls(u8* view) : arena{view}, jit{Config(this)} {
        constexpr std::array<u32, 2> code{0xd63f0200, 0xd4000001}; // blr x16; svc #0
        std::memcpy(arena + Entry, code.data(), sizeof(code));
        jit.SetSP(Page - 16);
    }

    u64 Invoke(std::string_view name, const std::array<u64, 8>& args) {
        int slot = 0;
        while (slot < nvn_direct_name_count() && name != nvn_direct_name(slot)) {
            ++slot;
        }
        Check(slot < nvn_direct_name_count() && nvn_direct_implemented(slot), name);
        for (size_t i = 0; i < args.size(); ++i) {
            jit.SetRegister(i, args[i]);
        }
        jit.SetRegister(16, Thunks + 4 * slot);
        jit.SetPC(Entry);
        Check(jit.Run() == Dynarmic::HaltReason::UserDefined1 && pending == slot &&
                  jit.GetPC() == Return && jit.GetRegister(30) == Return,
              "halt on the requested guest thunk and recover its return address");
        const auto x = jit.GetRegisters();
        const std::array<u64, 8> v{};
        u64 v0{};
        Check(!nvn_direct_float_args(pending), "fixture has no floating-point calls");
        const u64 result = nvn_direct_call(pending, x.data(), v.data(), jit.GetSP(), x[30], &v0);
        pending = -1;
        jit.SetRegister(0, result);
        Check(jit.Run() == Dynarmic::HaltReason::UserDefined2 && jit.GetPC() == Return + 4,
              "resume in guest code after the NVN call");
        return jit.GetRegister(0);
    }

private:
    static constexpr u64 Entry = 0x100;
    static constexpr u64 Return = Entry + 4;
    static constexpr u64 Thunks = 0x1000'0000;

    static Dynarmic::A64::UserConfig Config(GuestCalls* callbacks) {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = callbacks;
        config.enable_cycle_counting = false;
        config.block_entry_hook = [](u64 pc) {
            using Dynarmic::A64::BlockEntryHook;
            return pc >= Thunks && pc < Thunks + 4 * nvn_direct_name_count()
                       ? BlockEntryHook::Thunk
                       : BlockEntryHook::None;
        };
        return config;
    }

    std::optional<u32> MemoryReadCode(u64 pc) override {
        Check(pc < Thunks, "synthetic thunk must not fetch guest code");
        if (pc != Entry && pc != Return) {
            return std::nullopt;
        }
        u32 value;
        std::memcpy(&value, arena + pc, sizeof(value));
        return value;
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op,
                                         u64 pc) override {
        Check(op == Dynarmic::A64::InstructionCacheOperation::BlockEntryHook && pending == -1 &&
                  pc >= Thunks && (pc - Thunks) % 4 == 0 &&
                  (pc - Thunks) / 4 < static_cast<u64>(nvn_direct_name_count()),
              "unexpected guest thunk");
        pending = static_cast<int>((pc - Thunks) / 4);
        jit.SetRegister(16, jit.GetRegister(30));
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }

    void CallSVC(u32 number) override {
        Check(number == 0 && pending == -1, "unexpected guest SVC");
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {
        Check(false, "unexpected guest CPU exception");
    }
    u64 MemoryRead(u64, size_t) override { std::abort(); }
    Dynarmic::A64::Vector MemoryRead128(u64) override { std::abort(); }
    void MemoryWrite(u64, u64, size_t) override { std::abort(); }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override { std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }

    u8* const arena;
    Dynarmic::A64::Jit jit;
    int pending{-1};
};

void WaitForFence(GuestCalls& guest, u64 sync) {
    const u64 result = guest.Invoke("nvnSyncWait", {sync, 5'000'000'000});
    Check(result == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED ||
              result == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,
          "wait for Metal-backed NVN fence");
}

void RenderGuestTexture(GuestCalls& guest, u8* backing, u64 device, u64 pool, u64 queue,
                        u64 sync, u64 command_buffer, u64 handles, u64 readback) {
    constexpr u64 tb = ObjectPage + offsetof(Objects, texture_builder);
    constexpr u64 texture = ObjectPage + offsetof(Objects, texture);
    constexpr u64 texture_array = ObjectPage + offsetof(Objects, texture_address);
    constexpr u64 rgba = ObjectPage + offsetof(Objects, clear_color);
    constexpr u64 region = ObjectPage + offsetof(Objects, read_region);
    constexpr int side = 32;
    const std::array<float, 4> clear{1.0f, 0.0f, 0.0f, 1.0f};
    const NVNcopyRegion rect{0, 0, 0, side, side, 1};
    std::memcpy(backing + texture_array, &texture, sizeof(texture));
    std::memcpy(backing + rgba, clear.data(), sizeof(clear));
    std::memcpy(backing + region, &rect, sizeof(rect));

    guest.Invoke("nvnTextureBuilderSetDefaults", {tb});
    guest.Invoke("nvnTextureBuilderSetDevice", {tb, device});
    guest.Invoke("nvnTextureBuilderSetFlags", {tb, NVN_TEXTURE_FLAGS_DISPLAY});
    guest.Invoke("nvnTextureBuilderSetSize2D", {tb, side, side});
    guest.Invoke("nvnTextureBuilderSetFormat", {tb, NVN_FORMAT_RGBA8});
    const auto texture_size = guest.Invoke("nvnTextureBuilderGetStorageSize", {tb});
    Check(texture_size > 0 && texture_size <= 2 * Page, "fit swap texture in the data pool");
    guest.Invoke("nvnTextureBuilderSetStorage", {tb, pool, 0});
    Check(guest.Invoke("nvnTextureInitialize", {texture, tb}) == 1, "initialize swap texture");

    guest.Invoke("nvnCommandBufferBeginRecording", {command_buffer});
    guest.Invoke("nvnCommandBufferSetRenderTargets", {command_buffer, 1, texture_array, 0, 0, 0});
    guest.Invoke("nvnCommandBufferClearColor", {command_buffer, 0, rgba, NVN_CLEAR_COLOR_MASK_RGBA});
    const u64 handle = guest.Invoke("nvnCommandBufferEndRecording", {command_buffer});
    std::memcpy(backing + handles, &handle, sizeof(handle));
    guest.Invoke("nvnQueueSubmitCommands", {queue, 1, handles});
    guest.Invoke("nvnQueueFenceSync", {queue, sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0});
    WaitForFence(guest, sync);
    guest.Invoke("nvnTextureReadTexels", {texture, 0, region, readback});
    constexpr std::array<u8, 4> red{255, 0, 0, 255};
    for (int i = 0; i < side * side; ++i) {
        Check(std::memcmp(backing + readback + i * red.size(), red.data(), red.size()) == 0,
              "read back the cleared guest swap texture");
    }

    guest.Invoke("nvnTextureFinalize", {texture});
}

void RunGuestSubmissions() {
    Common::HostMemory memory(ArenaSize, ArenaSize);
    const Common::HostMemory::BackingRun run{.view_offset = 0, .host_offset = 0, .length = ArenaSize};
    u8* const view = memory.MapBackingView(ArenaSize, std::span{&run, 1});
    Check(view != nullptr, "map guest backing");
    u8* const backing = memory.BackingBasePointer();
    Check(view != backing, "use separate addresses for arena and backing");

    constexpr u64 data = 3 * Page;
    constexpr u64 command = 5 * Page;
    constexpr u64 control = 6 * Page;
    constexpr u64 queue_memory = 7 * Page;
    constexpr u64 readback = queue_memory + QueueMemorySize;
    constexpr u64 source = data + 128;
    constexpr u64 destination = data + Page + 256;
    constexpr size_t bytes = 64;
    std::array<u8, bytes> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(i * 3 + 1);
    }
    std::memset(backing + data, Guard, 2 * Page);
    std::memcpy(backing + source, pattern.data(), pattern.size());

    constexpr u64 obj = ObjectPage;
    constexpr u64 db = obj + offsetof(Objects, device_builder);
    constexpr u64 dev = obj + offsetof(Objects, device);
    constexpr u64 pb = obj + offsetof(Objects, pool_builder);
    constexpr u64 data_pool = obj + offsetof(Objects, data_pool);
    constexpr u64 cmd_pool = obj + offsetof(Objects, command_pool);
    constexpr u64 cb = obj + offsetof(Objects, command_buffer);
    constexpr u64 qb = obj + offsetof(Objects, queue_builder);
    constexpr u64 q = obj + offsetof(Objects, queue);
    constexpr u64 sync = obj + offsetof(Objects, sync);
    constexpr u64 handles = obj + offsetof(Objects, handle);

    nvn_direct_memory callbacks{};
    callbacks.backing_fd = -1;
    nvn_direct_init(view, ArenaSize, &callbacks, 1, 20);
    GuestCalls guest{view};
    const auto Call = [&guest](std::string_view name, const std::array<u64, 8>& args) {
        return guest.Invoke(name, args);
    };
    Call("nvnDeviceBuilderSetDefaults", {db});
    Check(Call("nvnDeviceInitialize", {dev, db}) == 1, "initialize device");

    const auto make_pool = [&](u64 pool, u64 storage, u64 size) {
        Call("nvnMemoryPoolBuilderSetDefaults", {pb});
        Call("nvnMemoryPoolBuilderSetFlags",
             {pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED});
        Call("nvnMemoryPoolBuilderSetStorage", {pb, storage, size});
        Check(Call("nvnMemoryPoolInitialize", {pool, pb}) == 1, "initialize pool");
        Check(Call("nvnMemoryPoolGetBufferAddress", {pool}) == storage, "return guest pool address");
    };
    make_pool(data_pool, data, 2 * Page);
    make_pool(cmd_pool, command, Page);
    __weak id<MTLBuffer> data_buffer;
    __weak id<MTLBuffer> command_buffer;
    @autoreleasepool {
        void* buffer{};
        u64 offset{};
        Check(mtl_addr(reinterpret_cast<u64>(view + data), 2 * Page, &buffer, &offset) != 0 &&
                  offset == 0,
              "resolve the Metal data buffer");
        data_buffer = (__bridge id<MTLBuffer>)buffer;
        Check(data_buffer.contents == view + data, "import data without copying");
        Check(mtl_addr(reinterpret_cast<u64>(view + command), Page, &buffer, &offset) != 0 &&
                  offset == 0,
              "resolve the Metal command buffer");
        command_buffer = (__bridge id<MTLBuffer>)buffer;
        Check(command_buffer.contents == view + command, "import command memory without copying");
    }

    Check(Call("nvnCommandBufferInitialize", {cb, dev}) == 1, "initialize command buffer");
    Call("nvnCommandBufferAddCommandMemory", {cb, cmd_pool, 0, Page});
    Call("nvnCommandBufferAddControlMemory", {cb, control, Page});
    Call("nvnCommandBufferBeginRecording", {cb});
    Call("nvnCommandBufferCopyBufferToBuffer", {cb, source, destination, bytes, 0});
    const u64 handle = Call("nvnCommandBufferEndRecording", {cb});
    Check(handle >= control && handle < control + Page, "return guest control-chain handle");
    std::memcpy(backing + handles, &handle, sizeof(handle));

    Call("nvnQueueBuilderSetDefaults", {qb});
    Call("nvnQueueBuilderSetDevice", {qb, dev});
    const u64 queue_size = Call("nvnQueueBuilderGetQueueMemorySize", {qb});
    Check(queue_size > 0 && queue_size <= QueueMemorySize, "size queue memory");
    Call("nvnQueueBuilderSetQueueMemory", {qb, queue_memory, queue_size});
    Check(Call("nvnQueueInitialize", {q, qb}) == 1, "initialize queue");
    Check(Call("nvnSyncInitialize", {sync, dev}) == 1, "initialize fence");

    const u64 before = mtl_last_fence();
    nvn_queue_test_hold(1);
    Call("nvnQueueSubmitCommands", {q, 1, handles});
    Call("nvnQueueFenceSync", {q, sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0});
    Check(Call("nvnSyncWait", {sync, 0}) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,
          "queued fence stays unsignaled before replay");
    Check(std::all_of(backing + destination, backing + destination + bytes,
                      [](u8 b) { return b == Guard; }),
          "guest backing stays unchanged before replay");
    nvn_queue_test_hold(0);
    WaitForFence(guest, sync);
    const u64 after = mtl_last_fence();
    Check(after > before && mtl_completed_fence() >= after, "Metal submission completed");
    Check(std::memcmp(backing + destination, pattern.data(), bytes) == 0, "GPU wrote guest backing");
    Check(std::memcmp(backing + source, pattern.data(), bytes) == 0, "source stayed unchanged");
    const auto guard = [](u8 b) { return b == Guard; };
    Check(std::all_of(backing + data, backing + source, guard) &&
              std::all_of(backing + source + bytes, backing + destination, guard) &&
              std::all_of(backing + destination + bytes, backing + data + 2 * Page, guard),
          "copy left all guards intact");

    RenderGuestTexture(guest, backing, dev, data_pool, q, sync, cb, handles, readback);
    Call("nvnSyncFinalize", {sync});
    Call("nvnQueueFinalize", {q});
    Call("nvnCommandBufferFinalize", {cb});
    Call("nvnMemoryPoolFinalize", {cmd_pool});
    Call("nvnMemoryPoolFinalize", {data_pool});
    Call("nvnDeviceFinalize", {dev});
    mtl_drain();
    for (int retries = 0; retries < 200 && (data_buffer || command_buffer); ++retries) {
        usleep(5'000);
    }
    Check(!data_buffer && !command_buffer, "Metal released the buffers before unmapping");
    memory.UnmapBackingView(view, ArenaSize);
    std::puts("NVN Metal bridge: A64 thunks, fenced 64-byte GPU copy and 1024 RGBA pixels passed");
}

} // namespace

int main() {
    @autoreleasepool {
        if (@available(macOS 26.0, *)) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device.hasUnifiedMemory || ![device supportsFamily:MTLGPUFamilyMetal4] ||
                getpagesize() != Page) {
                return 77;
            }
            alarm(15);
            setenv("DM_PSO_PREPARE", "0", 1);
            RunGuestSubmissions();
            return 0;
        }
        return 77;
    }
}
