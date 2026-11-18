// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// A page the GPU back thread untracks keeps its cached count, and so its write protection, for up
/// to two frames (ported from the-cashman/Lichii f7c9386239 and 2cb6c05960). Ocarina of Time's
/// village untracks pages for guest writes applied at fences and through the write hand-off, then
/// tracks them again at the next draw's upload: an unprotect and a protect each time, ~10% of
/// GPUBack. When the page is tracked again while held, the held count is used instead.
///
/// The holder lives in DeviceMemoryManager, which owns the counts. A held page is a count drop
/// not applied yet: tracking it again cancels the drop (Consume), and every other hold is applied
/// at the frame tick once HoldFrames frames old, or before a Map or Unmap of its device page
/// changes the backing the count protects (Take). Until then the page is protected while nothing
/// tracks it, so guest writes to it trap and find it untracked, the state OnCPUWrite's deferral
/// already leaves for a moment.
class Nx2LazyUntrack {
public:
    using Runs = std::vector<std::pair<u64, size_t>>;
    static constexpr u64 PageBits = 12;
    static constexpr u64 PageSize = u64{1} << PageBits;
    static constexpr u64 HoldFrames = 2;

    /// EDEN_NX2_LAZY_UNTRACK=0 turns it off, for the A/B before it merges.
    [[nodiscard]] static bool Enabled() noexcept {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_LAZY_UNTRACK");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    /// Set on the GPU back thread (GpuSplit::BackLoop).
    static inline thread_local bool t_hold{};

    [[nodiscard]] bool Any() const noexcept {
        return held_count.load(std::memory_order_acquire) != 0;
    }

    /// Holds the pages of `runs` instead of dropping their counts. Returns the pages held already,
    /// whose drop the caller applies now.
    Runs Hold(std::span<const std::pair<u64, size_t>> runs) {
        Runs now;
        std::scoped_lock lock{mutex};
        for (const auto& [addr, size] : runs) {
            for (u64 page = addr >> PageBits; page < PageEnd(addr, size); ++page) {
                if (held.try_emplace(page, frame).second) {
                    ++stats[Held];
                } else {
                    AppendPage(now, page);
                }
            }
        }
        held_count.store(held.size(), std::memory_order_release);
        return now;
    }

    /// Takes the held pages out of `runs`, which the caller is about to track: their count is
    /// there already. Returns what is left to track.
    Runs Consume(std::span<const std::pair<u64, size_t>> runs) {
        Runs left;
        std::scoped_lock lock{mutex};
        for (const auto& [addr, size] : runs) {
            u64 run_begin = addr;
            for (u64 page = addr >> PageBits; page < PageEnd(addr, size); ++page) {
                const auto it = held.find(page);
                if (it == held.end()) {
                    continue;
                }
                held.erase(it);
                ++stats[TrackedAgain];
                const u64 page_addr = page << PageBits;
                if (page_addr > run_begin) {
                    left.emplace_back(run_begin, page_addr - run_begin);
                }
                run_begin = page_addr + PageSize;
            }
            if (addr + size > run_begin) {
                left.emplace_back(run_begin, addr + size - run_begin);
            }
        }
        held_count.store(held.size(), std::memory_order_release);
        return left;
    }

    /// Takes the held pages of [addr, addr + size) out, for the caller to drop their counts.
    Runs Take(u64 addr, u64 size) {
        const u64 first = addr >> PageBits;
        const u64 end = PageEnd(addr, size);
        std::scoped_lock lock{mutex};
        return TakeIf([&](u64 page, u64) { return page >= first && page < end; }, ReleasedByMap);
    }

    /// Frame tick: takes the pages held for HoldFrames frames, for the caller to drop their
    /// counts. With `log_stats`, logs the counters every 600 frames.
    Runs Tick(bool log_stats) {
        std::scoped_lock lock{mutex};
        const u64 now = ++frame;
        Runs aged = TakeIf([now](u64, u64 held_frame) { return held_frame + HoldFrames <= now; },
                           ReleasedByAge);
        if (now % 600 == 0) {
            if (log_stats) {
                LOG_INFO(HW_GPU,
                         "Lazy untrack in 600 frames: {} pages held, {} tracked again, {} "
                         "released by map or unmap, {} by age; {} held now",
                         stats[Held], stats[TrackedAgain], stats[ReleasedByMap],
                         stats[ReleasedByAge], held.size());
            }
            stats = {};
        }
        return aged;
    }

private:
    enum Stat : size_t { Held, TrackedAgain, ReleasedByMap, ReleasedByAge, StatCount };

    static u64 PageEnd(u64 addr, u64 size) noexcept {
        return (addr + size + PageSize - 1) >> PageBits;
    }

    static void AppendPage(Runs& runs, u64 page) {
        const u64 page_addr = page << PageBits;
        if (!runs.empty() && runs.back().first + runs.back().second == page_addr) {
            runs.back().second += PageSize;
        } else {
            runs.emplace_back(page_addr, PageSize);
        }
    }

    template <class Pred>
    Runs TakeIf(Pred&& pred, Stat stat) {
        std::vector<u64> pages;
        for (auto it = held.begin(); it != held.end();) {
            if (pred(it->first, it->second)) {
                pages.push_back(it->first);
                it = held.erase(it);
            } else {
                ++it;
            }
        }
        held_count.store(held.size(), std::memory_order_release);
        stats[stat] += pages.size();
        std::sort(pages.begin(), pages.end());
        Runs runs;
        for (const u64 page : pages) {
            AppendPage(runs, page);
        }
        return runs;
    }

    std::mutex mutex;
    std::unordered_map<u64, u64> held; ///< device page -> frame it was held on
    std::atomic<size_t> held_count{};
    u64 frame{};
    std::array<u64, StatCount> stats{};
};

} // namespace VideoCommon
