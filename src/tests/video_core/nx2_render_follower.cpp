// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_render_follower.h"

TEST_CASE("Render follower keeps a dequeued packet pending until it plays",
          "[nx2_render_follower]") {
    VideoCommon::Nx2RenderFollower follower;
    std::latch executing{1};
    std::latch release{1};
    std::vector<size_t> played;
    std::jthread render{[&] {
        while (auto packet = follower.Take()) {
            packet();
            packet = {};
            follower.Retire();
        }
    }};
    const auto engine_id = std::this_thread::get_id();
    bool on_render = false;
    follower.Publish([&] {
        on_render = std::this_thread::get_id() != engine_id;
        executing.count_down();
        release.wait();
        played.push_back(0);
    });
    executing.wait();
    const bool pending_while_executing = follower.Pending();
    release.count_down();
    follower.Wait();
    const bool pending_after_wait = follower.Pending();
    for (size_t sequence = 1; sequence <= 256; ++sequence) {
        follower.Publish([&, sequence] { played.push_back(sequence); });
    }
    follower.Publish({});
    render.join();

    REQUIRE(on_render);
    REQUIRE(pending_while_executing);
    REQUIRE_FALSE(pending_after_wait);
    REQUIRE(played.size() == 257);
    for (size_t sequence = 0; sequence < played.size(); ++sequence) {
        CHECK(played[sequence] == sequence);
    }
}

TEST_CASE("Render follower hands back-to-back packets over without parking while it polls",
          "[nx2_render_follower]") {
    VideoCommon::Nx2RenderFollower follower{std::chrono::seconds{1}};
    std::vector<size_t> played;
    std::jthread render{[&] {
        while (auto packet = follower.Take()) {
            packet();
            packet = {};
            follower.Retire();
        }
    }};
    size_t parked = 0;
    for (size_t sequence = 0; sequence < 4096; ++sequence) {
        follower.Publish([&, sequence] { played.push_back(sequence); });
        parked += follower.Wait() ? 1 : 0;
    }
    follower.Publish({});
    render.join();

    CHECK(parked == 0);
    REQUIRE(played.size() == 4096);
    for (size_t sequence = 0; sequence < played.size(); ++sequence) {
        CHECK(played[sequence] == sequence);
    }
}

TEST_CASE("Render follower wakes a parked peer on both sides", "[nx2_render_follower]") {
    VideoCommon::Nx2RenderFollower follower{std::chrono::nanoseconds{0}};
    std::latch executing{1};
    std::latch release{1};
    size_t played = 0;
    std::jthread render{[&] {
        while (auto packet = follower.Take()) {
            packet();
            packet = {};
            follower.Retire();
        }
    }};
    follower.Publish([&] {
        executing.count_down();
        release.wait();
        ++played;
    });
    executing.wait();
    std::jthread releaser{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        release.count_down();
    }};
    const bool parked = follower.Wait();
    for (size_t sequence = 0; sequence < 1024; ++sequence) {
        if (sequence % 64 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
        follower.Publish([&] { ++played; });
        follower.Wait();
    }
    follower.Publish({});
    render.join();

    CHECK(parked);
    CHECK(played == 1025);
}
