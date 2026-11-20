// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "testenv.h"

namespace {

constexpr u64 thunk = 0x1000;
constexpr u64 resume = 0x1004;

class HookEnv final : public A64TestEnv {
public:
    std::optional<u32> MemoryReadCode(u64 pc) override {
        if (pc == thunk) {
            ++thunk_reads;
        }
        return A64TestEnv::MemoryReadCode(pc);
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op,
                                         u64 pc) override {
        REQUIRE(op == Dynarmic::A64::InstructionCacheOperation::BlockEntryHook);
        REQUIRE(pc == thunk);
        ++calls;
        jit->SetRegister(0, 41);
        jit->SetRegister(16, resume);
        if (halt_in_hook) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }

    void CallSVC(u32) override {
        ++svcs;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    Dynarmic::A64::Jit* jit{};
    u32 thunk_reads{};
    u32 calls{};
    u32 svcs{};
    bool halt_in_hook{};
};

} // namespace

TEST_CASE("A64: block-entry thunk dispatches without reading guest code", "[a64][block_entry_hook]") {
    using namespace Dynarmic;
    using namespace oaknut::util;

    HookEnv env;
    env.code_mem_start_address = thunk;
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.MOVZ(X0, 10);
    code.ADD(X0, X0, 1);
    code.SVC(0);

    bool enable_hook = true;
    SECTION("no hook") { enable_hook = false; }
    SECTION("hook continues immediately") {}
    SECTION("hook returns to the host before continuing") { env.halt_in_hook = true; }

    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    if (enable_hook) {
        config.block_entry_hook = [](u64 pc) {
            return pc == thunk ? A64::BlockEntryHook::Thunk : A64::BlockEntryHook::None;
        };
    }
    A64::Jit jit{config};
    env.jit = &jit;

    for (u32 run = 1; run <= 2; ++run) {
        jit.SetPC(thunk);
        jit.SetRegister(0, 0);
        const HaltReason reason = jit.Run();
        if (env.halt_in_hook) {
            REQUIRE(reason == HaltReason::UserDefined1);
            REQUIRE(jit.GetPC() == resume);
            REQUIRE(jit.GetRegister(0) == 41);
            REQUIRE(env.svcs == run - 1);
            REQUIRE(jit.Run() == HaltReason::UserDefined2);
        } else {
            REQUIRE(reason == HaltReason::UserDefined2);
        }
        REQUIRE(jit.GetRegister(0) == (enable_hook ? 42 : 11));
        REQUIRE(env.svcs == run);
        REQUIRE(env.calls == (enable_hook ? run : 0));
        REQUIRE((env.thunk_reads == 0) == enable_hook);
    }
}
