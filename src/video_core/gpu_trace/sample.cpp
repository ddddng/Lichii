// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "video_core/dma_pusher.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_trace/sample.h"
#include "video_core/gpu_trace/trace_file.h"
#include "video_core/pte_kind.h"
#include "video_core/textures/decoders.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::GpuTrace {
namespace {
using Tegra::Engines::Maxwell3D;

constexpr u64 DeviceBase = 0x40000000;
constexpr u64 PhysicalBase = 0x40000000;
constexpr u64 GpuBase = 0x200000000;
constexpr u64 Size = 16ULL << 20;
constexpr u64 PushOffset = 0;
constexpr u64 ImageOffset = 0x100000;
constexpr u64 TargetOffset = 0x200000;
constexpr u32 Width = 256;
constexpr u32 Height = 144;
constexpr s32 Channel = 1;

struct Rect {
    u32 x0, y0, x1, y1;
};

class Commands {
public:
    void Method(u32 method, std::initializer_list<u32> arguments) {
        Tegra::CommandHeader header{};
        header.method.Assign(method);
        header.subchannel.Assign(0);
        header.arg_count.Assign(static_cast<u32>(arguments.size()));
        header.mode.Assign(Tegra::SubmissionMode::Increasing);
        words.push_back(header.argument);
        words.insert(words.end(), arguments);
    }

    void Clear(std::array<f32, 4> color, const Rect* scissor) {
        if (scissor != nullptr) {
            Method(MAXWELL3D_REG_INDEX(scissor_test),
                   {1, scissor->x0 | (scissor->x1 << 16), scissor->y0 | (scissor->y1 << 16)});
        }
        Method(MAXWELL3D_REG_INDEX(clear_control), {scissor != nullptr ? 0x100u : 0u});
        Method(MAXWELL3D_REG_INDEX(clear_color),
               {std::bit_cast<u32>(color[0]), std::bit_cast<u32>(color[1]),
                std::bit_cast<u32>(color[2]), std::bit_cast<u32>(color[3])});
        // R, G, B and A of render target 0, layer 0.
        Method(MAXWELL3D_REG_INDEX(clear_surface), {0x3c});
        if (scissor != nullptr) {
            Method(MAXWELL3D_REG_INDEX(scissor_test), {0});
        }
    }

    std::vector<u32> words;
};

std::span<const u8> AsBytes(const auto& values) {
    return {reinterpret_cast<const u8*>(values.data()), values.size() * sizeof(values[0])};
}

/// Presented images and render targets are block linear with 16-GOB blocks, as a game's are.
constexpr u32 BlockHeightLog2 = 4;

std::vector<u32> Image(u32 seed) {
    std::vector<u32> pixels(Width * Height);
    for (u32 y = 0; y < Height; ++y) {
        for (u32 x = 0; x < Width; ++x) {
            const u32 r = (x * 255 / (Width - 1)) ^ (seed * 0x3f);
            const u32 g = (y * 255 / (Height - 1));
            const u32 b = ((x / 16 + y / 16 + seed) % 2) * 0xff;
            pixels[y * Width + x] = (r & 0xff) | (g << 8) | (b << 16) | 0xff000000;
        }
    }
    std::vector<u32> tiled(Tegra::Texture::CalculateSize(true, 4, Width, Height, 1,
                                                         BlockHeightLog2, 0) /
                           sizeof(u32));
    Tegra::Texture::SwizzleTexture(
        std::span<u8>{reinterpret_cast<u8*>(tiled.data()), tiled.size() * sizeof(u32)},
        AsBytes(pixels), 4, Width, Height, 1, BlockHeightLog2, 0, 1, true);
    return tiled;
}

void Present(Writer& writer, u64 offset, u64 time_ns) {
    PresentLayer layer{};
    layer.address = DeviceBase + offset;
    layer.width = Width;
    layer.height = Height;
    layer.stride = Width;
    layer.pixel_format = 1; // Rgba8888
    layer.crop_right = static_cast<s32>(Width);
    layer.crop_bottom = static_cast<s32>(Height);
    layer.layer_stack_mask = 1;
    layer.native_tiling = 1;
    const PresentRecord header{time_ns, 1, 0};
    const std::array<std::span<const u8>, 2> parts{
        std::span<const u8>{reinterpret_cast<const u8*>(&header), sizeof(header)},
        std::span<const u8>{reinterpret_cast<const u8*>(&layer), sizeof(layer)}};
    writer.Write(RecordType::Present, parts);
}

/// The commands go to memory first (a CPU write), as a game writes them.
void Submit(Writer& writer, u64 offset, const Commands& commands) {
    writer.WriteMemory(PhysicalBase + offset, AsBytes(commands.words));
    Tegra::CommandListHeader entry{};
    entry.addr.Assign(GpuBase + offset);
    entry.size.Assign(commands.words.size());
    const SubmitRecord header{Channel, 1, 0, 0, 0, 0};
    const std::array<std::span<const u8>, 2> parts{
        std::span<const u8>{reinterpret_cast<const u8*>(&header), sizeof(header)},
        std::span<const u8>{reinterpret_cast<const u8*>(&entry.raw), sizeof(entry.raw)}};
    writer.Write(RecordType::Submit, parts);
}

Commands BindTarget() {
    Commands commands;
    const u64 address = GpuBase + TargetOffset;
    commands.Method(MAXWELL3D_REG_INDEX(rt),
                    {static_cast<u32>(address >> 32), static_cast<u32>(address), Width, Height,
                     static_cast<u32>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM),
                     BlockHeightLog2 << 4, 1, 0, 0});
    commands.Method(MAXWELL3D_REG_INDEX(rt_control), {1});
    commands.Method(MAXWELL3D_REG_INDEX(surface_clip), {Width << 16, Height << 16});
    return commands;
}
} // Anonymous namespace

bool WriteSampleTrace(const std::filesystem::path& path) {
    Writer writer{path};
    if (!writer.IsOpen()) {
        return false;
    }
    const nlohmann::json config{
        {"program_id", "0400000000000001"}, {"switch2", true},  {"synthetic", true},
        {"memory_layout", 0},               {"width", Width},   {"height", Height},
    };
    const std::string text = config.dump();
    writer.Write(RecordType::Config, AsBytes(text));
    writer.WritePod(RecordType::AddressSpace, AddressSpaceRecord{0, 40, 1ULL << 34, 16, 12});
    writer.WritePod(RecordType::SmmuMap, SmmuMapRecord{DeviceBase, PhysicalBase, Size});
    writer.WritePod(RecordType::GmmuMap,
                    GmmuMapRecord{0, static_cast<u32>(Tegra::PTEKind::PITCH), GpuBase, DeviceBase,
                                  Size, 0, 0});
    writer.WritePod(RecordType::Channel, ChannelRecord{Channel, 0, 0x0400000000000001ULL});
    writer.Write(RecordType::SnapshotEnd, std::span<const u8>{});

    constexpr u64 Frame = 16'666'667;
    writer.WriteMemory(PhysicalBase + ImageOffset, AsBytes(Image(0)));
    Present(writer, ImageOffset, Frame);

    Commands first = BindTarget();
    const Rect square{64, 36, 192, 108};
    first.Clear({1.0f, 0.0f, 0.0f, 1.0f}, nullptr);
    first.Clear({0.0f, 1.0f, 0.0f, 1.0f}, &square);
    Submit(writer, PushOffset, first);
    Present(writer, TargetOffset, Frame * 2);

    writer.WriteMemory(PhysicalBase + ImageOffset, AsBytes(Image(1)));
    Present(writer, ImageOffset, Frame * 3);

    Commands second;
    const Rect left{16, 16, 96, 128};
    const Rect right{160, 16, 240, 128};
    second.Clear({0.0f, 0.0f, 1.0f, 1.0f}, nullptr);
    second.Clear({1.0f, 1.0f, 0.0f, 1.0f}, &left);
    second.Clear({1.0f, 0.0f, 1.0f, 1.0f}, &right);
    Submit(writer, PushOffset + 0x8000, second);
    Present(writer, TargetOffset, Frame * 4);
    return writer.Finish();
}

namespace {
constexpr u64 ChurnSize = 512ULL << 20;
constexpr u64 ChurnPushOffset = 0;
constexpr u64 ChurnPushStride = 0x1000;
constexpr u64 ChurnPresentOffset = 0x800000;
constexpr u64 ChurnTargetOffset = 0x1000000;
constexpr u64 TicPoolOffset = 0x900000;
constexpr u32 TicPoolLimit = 1023;
constexpr u32 TargetSlots = 256;
constexpr u64 SlotStride = 0x40000;
constexpr u32 TargetsPerFrame = 4;
constexpr u64 ShadowOffset = 0x10000000;
constexpr u64 ShadowStride = 0x200000;
constexpr u32 TableLights = 3;
constexpr u32 TableEntries = 64;
constexpr u32 LightTic = 10;
constexpr u32 StaleTic0 = 100;
constexpr u32 StaleWritesPerFrame = 6;

struct ChurnTarget {
    bool depth;
    u64 offset;
    u32 width;
    u32 height;
    u32 format;
};

std::vector<ChurnTarget> ChurnFrame(std::string_view pattern, u32 frame) {
    constexpr u32 Z32 = static_cast<u32>(Tegra::DepthFormat::Z32_FLOAT);
    const u64 base = ChurnTargetOffset;
    if (pattern == "steady") {
        return {{true, base, 512, 512, Z32}};
    }
    if (pattern == "resize") {
        const u32 size = 256 + 16 * (frame % 32);
        return {{true, base, size, size, Z32}};
    }
    if (pattern == "resize-unique") {
        const u32 size = 256 + 4 * frame;
        return {{true, base, size, size, Z32}};
    }
    if (pattern == "color-resize-unique") {
        const u32 size = 256 + 4 * frame;
        return {{false, base, size, size,
                 static_cast<u32>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM)}};
    }
    if (pattern == "formats") {
        using F = Tegra::RenderTargetFormat;
        static constexpr std::array<std::pair<F, u32>, 4> shapes{{
            {F::A8B8G8R8_UNORM, 512},
            {F::R32_FLOAT, 512},
            {F::R16G16B16A16_FLOAT, 256},
            {F::A2B10G10R10_UNORM, 1024},
        }};
        const auto& [format, size] = shapes[frame % shapes.size()];
        return {{false, base, size, size, static_cast<u32>(format)}};
    }
    if (pattern == "drift") {
        return {{true, base + (frame % 64) * 0x10000, 512, 512, Z32}};
    }
    if (pattern == "heap") {
        // Transient render targets placed at shifting offsets in one 96 MiB heap: 4 MiB colour
        // targets 1.25 MiB apart, so each overlaps those placed before it.
        std::vector<ChurnTarget> targets;
        for (u32 j = 0; j < 3; ++j) {
            const u64 slot = (frame * 3 + j) % 76;
            targets.push_back({false, base + slot * 0x140000, 1024, 1024,
                               static_cast<u32>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM)});
        }
        return targets;
    }
    if (pattern == "heap-drift") {
        // The same heap, but each target lands 4 MiB + 64 KiB past the last, so offsets wrap
        // around the heap without repeating: every target is a new image over older ones.
        std::vector<ChurnTarget> targets;
        for (u32 j = 0; j < 3; ++j) {
            const u64 offset = (u64{frame * 3 + j} * 0x410000) % (96ULL << 20);
            targets.push_back({false, base + offset, 1024, 1024,
                               static_cast<u32>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM)});
        }
        return targets;
    }
    if (pattern == "table") {
        std::vector<ChurnTarget> targets;
        for (u64 light = 0; light < TableLights; ++light) {
            targets.push_back({true, ShadowOffset + light * ShadowStride, 512, 512, Z32});
        }
        for (u32 j = 0; j < TargetsPerFrame; ++j) {
            targets.push_back({false,
                               base + ((frame * TargetsPerFrame + j) % TargetSlots) * SlotStride,
                               256, 256,
                               static_cast<u32>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM)});
        }
        return targets;
    }
    return {};
}

Tegra::Texture::TICEntry Tic(u64 offset, Tegra::Texture::TextureFormat format,
                             Tegra::Texture::ComponentType type, u32 width, u32 height) {
    using namespace Tegra::Texture;
    TICEntry tic{};
    const u64 address = GpuBase + offset;
    tic.format.Assign(format);
    tic.r_type.Assign(type);
    tic.g_type.Assign(type);
    tic.b_type.Assign(type);
    tic.a_type.Assign(type);
    tic.x_source.Assign(SwizzleSource::R);
    tic.y_source.Assign(SwizzleSource::G);
    tic.z_source.Assign(SwizzleSource::B);
    tic.w_source.Assign(SwizzleSource::A);
    tic.address_low = static_cast<u32>(address);
    tic.address_high.Assign(static_cast<u32>(address >> 32));
    tic.header_version.Assign(TICHeaderVersion::BlockLinear);
    tic.block_height.Assign(BlockHeightLog2);
    tic.width_minus_one.Assign(width - 1);
    tic.texture_type.Assign(TextureType::Texture2D);
    tic.height_minus_1.Assign(height - 1);
    tic.normalized_coords.Assign(1);
    return tic;
}

/// What a stale light-table entry describes at a frame: a colour image of another shape over
/// the render-target slots, so no image there matches it.
Tegra::Texture::TICEntry StaleTic(u32 frame, u32 entry) {
    using namespace Tegra::Texture;
    const u64 offset = ChurnTargetOffset + ((frame * 7 + entry * 13) % TargetSlots) * SlotStride;
    switch ((frame + entry) % 3) {
    case 0:
        return Tic(offset, TextureFormat::R16G16B16A16, ComponentType::FLOAT, 256, 256);
    case 1:
        return Tic(offset, TextureFormat::A8B8G8R8, ComponentType::UNORM, 256, 128);
    default:
        return Tic(offset, TextureFormat::R32, ComponentType::FLOAT, 512, 256);
    }
}

Commands ChurnCommands(const ChurnTarget& target, u32 frame) {
    Commands commands;
    const u64 address = GpuBase + target.offset;
    if (target.depth) {
        commands.Method(MAXWELL3D_REG_INDEX(rt_control), {0});
        commands.Method(MAXWELL3D_REG_INDEX(zeta),
                        {static_cast<u32>(address >> 32), static_cast<u32>(address), target.format,
                         BlockHeightLog2 << 4, 0});
        commands.Method(MAXWELL3D_REG_INDEX(zeta_size), {target.width, target.height, 1});
        commands.Method(MAXWELL3D_REG_INDEX(zeta_enable), {1});
    } else {
        commands.Method(MAXWELL3D_REG_INDEX(zeta_enable), {0});
        commands.Method(MAXWELL3D_REG_INDEX(rt),
                        {static_cast<u32>(address >> 32), static_cast<u32>(address), target.width,
                         target.height, target.format, BlockHeightLog2 << 4, 1, 0, 0});
        commands.Method(MAXWELL3D_REG_INDEX(rt_control), {1});
    }
    commands.Method(MAXWELL3D_REG_INDEX(surface_clip), {target.width << 16, target.height << 16});
    commands.Method(MAXWELL3D_REG_INDEX(clear_control), {0});
    if (target.depth) {
        commands.Method(MAXWELL3D_REG_INDEX(clear_depth),
                        {std::bit_cast<u32>(static_cast<f32>(frame % 7) / 7.0f)});
        commands.Method(MAXWELL3D_REG_INDEX(clear_surface), {0x1});
    } else {
        const f32 value = static_cast<f32>(frame % 7) / 7.0f;
        commands.Method(MAXWELL3D_REG_INDEX(clear_color),
                        {std::bit_cast<u32>(value), std::bit_cast<u32>(1.0f - value),
                         std::bit_cast<u32>(0.5f), std::bit_cast<u32>(1.0f)});
        commands.Method(MAXWELL3D_REG_INDEX(clear_surface), {0x3c});
    }
    return commands;
}
} // Anonymous namespace

bool IsChurnPattern(std::string_view pattern) {
    return !ChurnFrame(pattern, 0).empty();
}

bool WriteChurnTrace(const std::filesystem::path& path, std::string_view pattern, u32 frames) {
    if (!IsChurnPattern(pattern)) {
        return false;
    }
    Writer writer{path};
    if (!writer.IsOpen()) {
        return false;
    }
    const nlohmann::json config{
        {"program_id", "0400000000000002"}, {"switch2", true},  {"synthetic", true},
        {"memory_layout", 0},               {"width", Width},   {"height", Height},
    };
    const std::string text = config.dump();
    writer.Write(RecordType::Config, AsBytes(text));
    writer.WritePod(RecordType::AddressSpace, AddressSpaceRecord{0, 40, 1ULL << 34, 16, 12});
    writer.WritePod(RecordType::SmmuMap, SmmuMapRecord{DeviceBase, PhysicalBase, ChurnSize});
    writer.WritePod(RecordType::GmmuMap,
                    GmmuMapRecord{0, static_cast<u32>(Tegra::PTEKind::PITCH), GpuBase, DeviceBase,
                                  ChurnSize, 0, 0});
    writer.WritePod(RecordType::Channel, ChannelRecord{Channel, 0, 0x0400000000000002ULL});
    writer.Write(RecordType::SnapshotEnd, std::span<const u8>{});

    constexpr u64 Frame = 16'666'667;
    writer.WriteMemory(PhysicalBase + ChurnPresentOffset, AsBytes(Image(0)));
    const bool table = pattern == "table";
    const auto write_tic = [&](u32 index, const Tegra::Texture::TICEntry& tic) {
        writer.WriteMemory(PhysicalBase + TicPoolOffset + index * sizeof(tic),
                           std::span<const u8>{reinterpret_cast<const u8*>(&tic), sizeof(tic)});
    };
    if (table) {
        for (u32 light = 0; light < TableLights; ++light) {
            write_tic(LightTic + light,
                      Tic(ShadowOffset + light * ShadowStride, Tegra::Texture::TextureFormat::Z32,
                          Tegra::Texture::ComponentType::FLOAT, 512, 512));
        }
        for (u32 entry = 0; entry < TableEntries - TableLights; ++entry) {
            write_tic(StaleTic0 + entry, StaleTic(0, entry));
        }
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        Commands commands;
        if (table) {
            const u64 pool = GpuBase + TicPoolOffset;
            commands.Method(MAXWELL3D_REG_INDEX(tex_header),
                            {static_cast<u32>(pool >> 32), static_cast<u32>(pool), TicPoolLimit});
        }
        for (const ChurnTarget& target : ChurnFrame(pattern, frame)) {
            const Commands one = ChurnCommands(target, frame);
            commands.words.insert(commands.words.end(), one.words.begin(), one.words.end());
        }
        Submit(writer, ChurnPushOffset + (frame % 2048) * ChurnPushStride, commands);
        if (table) {
            // The game rewrites a few pool slots a frame, which the table still names.
            for (u32 j = 0; j < StaleWritesPerFrame; ++j) {
                const u32 entry = (frame * StaleWritesPerFrame + j) % (TableEntries - TableLights);
                write_tic(StaleTic0 + entry, StaleTic(frame + 1, entry));
            }
            std::vector<u32> visit{1};
            for (u32 light = 0; light < TableLights; ++light) {
                visit.push_back(LightTic + light);
            }
            for (u32 entry = 0; entry < TableEntries - TableLights; ++entry) {
                visit.push_back(StaleTic0 + entry);
            }
            writer.Write(RecordType::TextureVisit, AsBytes(visit));
        }
        Present(writer, ChurnPresentOffset, Frame * (frame + 1));
    }
    return writer.Finish();
}

} // namespace VideoCommon::GpuTrace
