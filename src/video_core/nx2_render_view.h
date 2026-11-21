// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"

// EDEN_NX2_RENDER_VIEW_VERIFY=1: GPURender's copy of each channel's 3D registers (the view), fed
// by what the GPU back sends at every draw, and compared with the back's engine there. Verify
// only: nothing reads the view yet. Design from Fritchii e18356afa, 58974c92c and 036fe4b23.
//
// The back notes the words its engine writes (ProcessDirtyRegisters, StoreStateRegisters) and the
// constant buffers it binds (ProcessCBBind). Each draw sends those words and bindings by value,
// the engine hint and macro mode when they change, the draw values, and the forwarded dirty flags
// raised and cleared since the previous draw. A draw also carries what the previous one cleared,
// which GPURender will consume on the view itself once it draws.
//
// Flags: the buffer cache alone reads IndexBuffer and VertexBuffer0..31, so they stay the back's;
// VertexBuffers is read on both sides; every other flag is forwarded. Flag 0 is the tables' "no
// flag" slot.
namespace VideoCommon::Nx2RenderView {

using Maxwell3D = Tegra::Engines::Maxwell3D;
using Flags = Maxwell3D::DirtyState::Flags;
inline constexpr u32 NumRegs = Maxwell3D::Regs::NUM_REGS;

/// The view runs with EDEN_NX2_RENDER_VIEW_VERIFY=1 or EDEN_NX2_RENDER_UNLEND.
[[nodiscard]] bool Enabled();
/// EDEN_NX2_RENDER_VIEW_VERIFY=1: every lent draw compares (with unlend, only forced ones else).
[[nodiscard]] bool VerifyAll();
void SetEnabledForTest(std::optional<bool> on);

/// Must-be-0 checks.
enum class Check : u32 {
    ViewWords,
    ViewFlags,
    ViewDraw,
    ViewPools,
    ViewValues,
    BuildPipeline,
    BuildImageViews,
    BuildDynamic,
    BuildDescriptors,
    BuildRenderTargets,
    Count,
};

void Note(Check check, std::string_view detail);
[[nodiscard]] u64 Noted(Check check);
[[nodiscard]] u64 Compared();
void ResetForTest();

[[nodiscard]] bool Watched(u32 index);
[[nodiscard]] const Flags& Forwarded();

struct RegPair {
    u32 index;
    u32 value;
};

struct DrawValues {
    Maxwell3D::Regs::PrimitiveTopology topology{};
    Maxwell3D::DrawManager::DrawMode draw_mode{};
    u32 draw_indexed{};
    u32 draw_element_count{};
    u32 base_index{};
    Maxwell3D::Regs::VertexBuffer vertex_buffer{};
    Maxwell3D::Regs::IndexBuffer index_buffer{};
    u32 base_instance{};
    u32 instance_count{};
    u32 drawn_instances{};
};

struct BindValue {
    u8 stage;
    u8 slot;
    u8 enabled;
    u32 size;
    GPUVAddr address;
};

struct EngineValues {
    Maxwell3D::EngineHint engine_state{};
    bool native{};

    bool operator==(const EngineValues&) const = default;
};

/// One draw's update of the view, by value.
struct Packet {
    std::vector<u32> seed; ///< the whole register file, when the view starts over
    std::vector<RegPair> words;
    std::vector<BindValue> binds;
    Flags consumed; ///< forwarded flags the previous draw cleared
    Flags raised;
    Flags cleared;
    DrawValues draw;
    EngineValues values;
    bool has_values{};
};

/// GPURender: the view takes the packet.
void Apply(Maxwell3D& view, const Packet& packet);

/// The words a draw reads, one bit per register: all but the constant buffer data words and the
/// write offset they advance, the firmware and shadow scratch (written all the time, read by no
/// draw).
inline constexpr std::array<u64, NumRegs / 64> WatchMask = [] {
    std::array<u64, NumRegs / 64> mask{};
    mask.fill(~u64{0});
    const auto unwatch = [&mask](u32 first, u32 count) {
        for (u32 i = first; i < first + count && i < NumRegs; ++i) {
            mask[i / 64] &= ~(u64{1} << (i % 64));
        }
    };
    unwatch(MAXWELL3D_REG_INDEX(const_buffer.offset), 1);
    unwatch(MAXWELL3D_REG_INDEX(const_buffer.buffer), Maxwell3D::Regs::NumCBData);
    unwatch(MAXWELL3D_REG_INDEX(falcon), 32);
    unwatch(MAXWELL3D_REG_INDEX(shadow_scratch), 0x100);
    return mask;
}();

/// The back's side for one 3D engine (Maxwell3D::render_view_writes).
class Writes {
public:
    explicit Writes(Maxwell3D& engine);
    ~Writes();

    void Note(u32 index) noexcept {
        if (index < NumRegs) {
            dirty[index / 64] |= WatchMask[index / 64] & (u64{1} << (index % 64));
        }
    }
    void NoteBind(size_t stage, u32 slot) noexcept;

    /// At a draw: what the view needs to match the engine now. With `events` (unlend) the flags
    /// set on the engine are raised on the view, the caller clearing what GPURender consumes.
    [[nodiscard]] Packet Pack(Maxwell3D& engine, bool events = false);
    /// After the draw: the forwarded flags it cleared go in the next packet.
    void DrawDone(const Maxwell3D& engine);

    /// Written only by Apply, on GPURender (inline without it).
    [[nodiscard]] Maxwell3D& View() {
        return *view;
    }

private:
    std::unique_ptr<Maxwell3D> view;
    std::array<u64, NumRegs / 64> dirty{};
    std::array<u64, 2> binds{};
    Flags after;
    Flags consumed;
    EngineValues sent;
    std::vector<u32> hle_sent;
    bool seeded{};
};

void NoteWrite(Writes& writes, u32 index) noexcept;

/// The view against the engine at a draw, GPURender idle: words, flags (not with unlend, where
/// each side consumes its own), draw, pools, values.
void Compare(Maxwell3D& view, Maxwell3D& engine);

/// At every draw on the back: sends the packet, lets GPURender apply it, compares.
class DrawCheck {
public:
    /// `verify`: compare even with unlend on (EDEN_NX2_RENDER_VERIFY_EVERY).
    explicit DrawCheck(Maxwell3D& engine, bool verify = false);
    ~DrawCheck();

    DrawCheck(const DrawCheck&) = delete;
    DrawCheck& operator=(const DrawCheck&) = delete;

    /// GPURender's view of the draw's engine (null when off), for the build compares.
    [[nodiscard]] Maxwell3D* View() const {
        return compared ? &writes->View() : nullptr;
    }

private:
    Maxwell3D* engine{};
    Writes* writes{};
    bool compared{};
};

/// Every count, once a draw compared.
void LogCounts(std::string_view when);

} // namespace VideoCommon::Nx2RenderView
