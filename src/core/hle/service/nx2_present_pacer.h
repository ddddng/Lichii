// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Service {

/**
 * Returns the interval between paced presents: the vblank the game asked for, lengthened when an
 * explicit FPS limit is below the rate that vblank allows.
 *
 * @param vblank_interval The interval the game's present word asks for, already scaled by the
 *                        speed limit.
 * @param fps_limit       Frames per second to hold, or 0 to leave the game's interval alone.
 * @return The longer of the two intervals.
 */
[[nodiscard]] inline std::chrono::nanoseconds PacedInterval(std::chrono::nanoseconds vblank_interval,
                                                            std::uint32_t fps_limit) {
    if (fps_limit == 0) {
        return vblank_interval;
    }
    const auto limited_interval = std::chrono::nanoseconds{1'000'000'000LL / fps_limit};
    return (std::max)(vblank_interval, limited_interval);
}

class Nx2PresentPacer {
public:
    using Clock = std::chrono::steady_clock;

    struct Step {
        Clock::time_point deadline;
        bool reset;
    };

    /**
     * Returns the deadline for the present arriving at @p now: the previous deadline plus the
     * interval, re-anchored to @p now once the caller has fallen more than one interval behind.
     *
     * @param now      When the present reached the pacer.
     * @param interval Present interval, normally the value from PacedInterval.
     * @return The deadline to hold until, and whether the pacer re-anchored because it was late.
     */
    Step Advance(Clock::time_point now, std::chrono::nanoseconds interval) {
        const auto deadline = last_deadline ? *last_deadline + interval : now;
        const bool reset = !last_deadline || now > deadline + interval;
        last_deadline = reset ? now : deadline;
        return {*last_deadline, reset};
    }

    /**
     * Returns how long to hold a present that asks for more than one vblank. Mario Kart World
     * picks its interval each frame from how far its frame times ran past the vblanks it asked for
     * (main 0x69cc40 in 1.8.0: debt += last frame time - interval vblanks, at most 4 vblanks;
     * interval = 1 + debt / vblank, at most 2) and advances its clock by the interval, so interval
     * 2 there means "behind, catch up". Holding those presents for two vblanks made the next frame
     * time match the interval, so the debt never shrank and the game kept asking for two. This
     * keeps the same debt and holds one vblank while it says the game is behind; a game that runs
     * at 30 fps on time (Fire Emblem: Fortune's Weave) falls below it within a frame or two and
     * keeps its two vblanks.
     *
     * @param interval      The interval the present asks for, already scaled by the speed limit.
     * @param swap_interval Vblanks in @p interval.
     * @param may_catch_up  False to always hold the whole interval, as when it is forced.
     * @return One vblank while catching up, else @p interval.
     */
    std::chrono::nanoseconds Hold(std::chrono::nanoseconds interval, std::uint32_t swap_interval,
                                  bool may_catch_up) {
        const auto vblank = interval / swap_interval;
        if (previous_gap) {
            frame_debt = std::clamp(frame_debt + *previous_gap - interval, -4 * vblank, 4 * vblank);
        }
        return may_catch_up && swap_interval > 1 && frame_debt >= -vblank ? vblank : interval;
    }

    /// Records when a present returned to the game, for the frame time Hold measures.
    void Released(Clock::time_point when) {
        if (previous_release) {
            previous_gap =
                std::chrono::duration_cast<std::chrono::nanoseconds>(when - *previous_release);
        }
        previous_release = when;
    }

private:
    std::optional<Clock::time_point> last_deadline;
    std::optional<Clock::time_point> previous_release;
    std::optional<std::chrono::nanoseconds> previous_gap; // between the last two releases
    std::chrono::nanoseconds frame_debt{};                // how far frame times ran past intervals
};

} // namespace Service
