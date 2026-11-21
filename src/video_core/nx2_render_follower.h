// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/common_types.h"
#include "common/unique_function.h"

namespace VideoCommon {

/// One draw packet at a time from the GPU back to GPURender. A packet records in about a
/// microsecond and warm draws come a few microseconds apart, so both sides poll for `spin` before
/// they park: a futex wake-up costs more than the packet. Each side notifies only a parked peer.
class Nx2RenderFollower {
public:
    using Work = Common::UniqueFunction<void>;

    explicit Nx2RenderFollower(std::chrono::nanoseconds spin_ = DefaultSpin) : spin{spin_} {}

    static constexpr std::chrono::nanoseconds DefaultSpin = std::chrono::microseconds{30};

    [[nodiscard]] bool Pending() const {
        return submitted.load(std::memory_order_relaxed) != played.load(std::memory_order_acquire);
    }

    /// Waits for the packet in flight. Returns whether the caller parked.
    bool Wait() const {
        const u64 target = submitted.load(std::memory_order_relaxed);
        if (Poll([&] { return played.load(std::memory_order_acquire) == target; })) {
            return false;
        }
        waiter_parked.store(true, std::memory_order_seq_cst);
        for (u64 done = played.load(std::memory_order_seq_cst); done != target;
             done = played.load(std::memory_order_seq_cst)) {
            played.wait(done, std::memory_order_seq_cst);
        }
        waiter_parked.store(false, std::memory_order_relaxed);
        return true;
    }

    void Publish(Work work) {
        Wait();
        packet = std::move(work);
        submitted.store(submitted.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
        if (taker_parked.load(std::memory_order_seq_cst)) {
            submitted.notify_one();
        }
    }

    [[nodiscard]] Work Take() {
        const u64 last = played.load(std::memory_order_relaxed);
        if (!Poll([&] { return submitted.load(std::memory_order_acquire) != last; })) {
            taker_parked.store(true, std::memory_order_seq_cst);
            submitted.wait(last, std::memory_order_seq_cst);
            taker_parked.store(false, std::memory_order_relaxed);
        }
        return std::move(packet);
    }

    void Retire() {
        played.store(played.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
        if (waiter_parked.load(std::memory_order_seq_cst)) {
            played.notify_one();
        }
    }

private:
    static void Relax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }

    template <typename Ready>
    bool Poll(Ready&& ready) const {
        if (ready() || spin.count() <= 0) {
            return ready();
        }
        const auto deadline = std::chrono::steady_clock::now() + spin;
        for (;;) {
            for (u32 i = 0; i < 64; ++i) {
                if (ready()) {
                    return true;
                }
                Relax();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return ready();
            }
        }
    }

    std::chrono::nanoseconds spin;
    alignas(64) std::atomic<u64> submitted{};
    std::atomic<bool> taker_parked{};
    Work packet;
    alignas(64) std::atomic<u64> played{};
    mutable std::atomic<bool> waiter_parked{};
};

} // namespace VideoCommon
