// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/uniform_stream_reuse.h"
#include "video_core/nx2_profile.h"

namespace {
namespace Profile = VideoCommon::Nx2Profile;
using Profile::BufferCount;
using Profile::BufferPart;

u64 Count(BufferCount counter) {
    return Profile::t_buffer.counts[static_cast<size_t>(counter)];
}

u64 Calls(BufferPart part) {
    return Profile::t_buffer.calls[static_cast<size_t>(part)];
}

u64 Ticks(BufferPart part) {
    return Profile::t_buffer.ticks[static_cast<size_t>(part)];
}

void Spin(int iterations) {
    volatile u64 sum = 0;
    for (int i = 0; i < iterations; ++i) {
        sum = sum + static_cast<u64>(i);
    }
}

struct ProfileOn {
    bool was = Profile::g_enabled;
    ProfileOn() {
        Profile::g_enabled = true;
        Profile::t_buffer = {};
    }
    ~ProfileOn() {
        Profile::g_enabled = was;
        Profile::t_buffer = {};
    }
};

struct Memory {
    std::array<u8, 256> bytes{};
    void ReadBlockUnsafe(u64 address, void* destination, u32 size) {
        std::memcpy(destination, bytes.data() + address, size);
    }
};

struct Runtime {
    struct StreamUniformRef {
        size_t allocation;
    };
    std::vector<std::vector<u8>> allocations;
    StreamUniformRef last{};
    u64 CurrentTick() const {
        return 1;
    }
    StreamUniformRef LastStreamUniformRef() const {
        return last;
    }
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        allocations.emplace_back(size);
        last = StreamUniformRef{allocations.size() - 1};
        return allocations.back();
    }
    void RebindStreamUniform(StreamUniformRef, u32) {}
};
} // namespace

TEST_CASE("Buffer profile parts count their own time, nested ones only in graphics parts",
          "[video_core]") {
    ProfileOn on;
    {
        Profile::BufferScope nested{BufferPart::Sync};
        Profile::Add(BufferCount::Syncs);
    }
    REQUIRE(Calls(BufferPart::Sync) == 0);
    REQUIRE(Count(BufferCount::Syncs) == 0);

    Profile::t_buffer.timed = true;
    {
        Profile::BufferScope outer{BufferPart::BindStorage};
        Spin(100);
        {
            Profile::BufferScope inner{BufferPart::Sync};
            Profile::Add(BufferCount::Syncs);
            Spin(200000);
        }
    }
    REQUIRE(Calls(BufferPart::BindStorage) == 1);
    REQUIRE(Calls(BufferPart::Sync) == 1);
    REQUIRE(Count(BufferCount::Syncs) == 1);
    REQUIRE(Ticks(BufferPart::Sync) > Ticks(BufferPart::BindStorage));
    REQUIRE(Profile::t_buffer.timed_scopes == 2);
    REQUIRE(Profile::t_buffer.depth == 0);
    REQUIRE(Profile::t_buffer.child == 0);

    Profile::t_buffer.timed = false;
    const u64 storage_ticks = Ticks(BufferPart::BindStorage);
    {
        Profile::BufferScope untimed{BufferPart::BindStorage};
        Spin(1000);
    }
    REQUIRE(Calls(BufferPart::BindStorage) == 2);
    REQUIRE(Ticks(BufferPart::BindStorage) == storage_ticks);

    Profile::g_enabled = false;
    {
        Profile::BufferScope off{BufferPart::BindStorage};
    }
    REQUIRE(Calls(BufferPart::BindStorage) == 2);
}

TEST_CASE("Buffer profile counts the stream uniform reuse outcomes", "[video_core]") {
    ProfileOn on;
    Memory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    const auto bind = [&] {
        Profile::BufferScope scope{BufferPart::BindUniform};
        REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 0, 64));
    };
    bind();
    bind();
    memory.bytes[3] = 7;
    bind();
    REQUIRE(Count(BufferCount::UniformCopied) == 2);
    REQUIRE(Count(BufferCount::UniformReused) == 1);
    REQUIRE(Count(BufferCount::UniformSameCopied) == 0);
    REQUIRE(Count(BufferCount::UniformCompareBytes) == 3 * 64);
    REQUIRE(Count(BufferCount::UniformCopyBytes) == 2 * 64);
    VideoCommon::ResetUniformStreamReuse<Runtime>();
}
