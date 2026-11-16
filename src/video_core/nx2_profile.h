// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/spin_recursive_mutex.h"
#include "common/thread_cpu_clock.h"
#include "video_core/nx2_stutter_trace.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    GraphicsPipelineMiss, ///< A graphics pipeline built on the GPU thread
    ComputePipelineMiss,  ///< A compute pipeline built on the GPU thread
    PipelineWait,         ///< VulkanWorker: a draw or dispatch waits for its pipeline's build
    ShaderCapture,        ///< A program the shader cache sees first: captured, lowered, hashed
    TextureCache,         ///< Texture cache downloads, joins, blits and async flushes and decodes
    GpuWait,              ///< Waiting for a GPU tick
    Present,              ///< Swapchain frame acquire and present
    WorkerIdle,           ///< VulkanWorker waiting for work
    PresentWait,          ///< Present waits for the Vulkan worker or a free presentation frame
    QueueSubmit,          ///< vkQueueSubmit
    ComputeStorage,       ///< A dispatch reads and sizes its storage buffer bindings
    ComputeImages,        ///< A dispatch's texture handles, image views and texture buffers
    ComputeBuffers,       ///< A dispatch's buffer lookups, uploads, write marks and binds
    ComputeDownloads,     ///< Early read downloads recorded right behind a dispatch
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait", "graphics pipeline miss",
    "compute pipeline miss", "pipeline wait", "shader capture", "texture cache", "GPU wait",
    "present", "worker idle",
    "present wait", "queue submit", "compute storage", "compute images", "compute buffers",
    "compute downloads",
};

static_assert(static_cast<size_t>(Zone::Count) <= StutterTrace::ZoneSlots);

/// The EDEN_NX2_STUTTER_TRACE category of each zone's exclusive time (None: counted as other).
inline constexpr auto stutter_categories = [] {
    using StutterTrace::Category;
    std::array<Category, static_cast<size_t>(Zone::Count)> categories;
    categories.fill(Category::None);
    const auto set = [&categories](Zone zone, Category category) {
        categories[static_cast<size_t>(zone)] = category;
    };
    set(Zone::Idle, Category::Idle);
    set(Zone::PendingSleep, Category::Idle);
    set(Zone::SplitDrain, Category::Idle);
    set(Zone::SplitWait, Category::Idle);
    set(Zone::WriterWait, Category::Idle);
    set(Zone::WorkerIdle, Category::Idle);
    set(Zone::GraphicsPipelineMiss, Category::GraphicsMiss);
    set(Zone::ComputePipelineMiss, Category::ComputeMiss);
    set(Zone::PipelineWait, Category::PipelineWait);
    set(Zone::ShaderCapture, Category::ShaderCapture);
    set(Zone::SyncDescriptors, Category::TextureCache);
    set(Zone::FillImageViews, Category::TextureCache);
    set(Zone::RenderTargets, Category::TextureCache);
    set(Zone::TextureUpload, Category::TextureCache);
    set(Zone::TextureCache, Category::TextureCache);
    set(Zone::TextureBuffers, Category::BufferCache);
    set(Zone::UpdateBuffers, Category::BufferCache);
    set(Zone::BindStageBuffers, Category::BufferCache);
    set(Zone::Fence, Category::QueryFence);
    set(Zone::ListSubmitWait, Category::QueryFence);
    set(Zone::ListGpuWait, Category::QueryFence);
    set(Zone::GpuWait, Category::QueryFence);
    set(Zone::Present, Category::Present);
    set(Zone::PresentWait, Category::Present);
    return categories;
}();

// A plain flag: Enabled() runs several times per method write (no guard to check).
inline bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
/// Whether zones measure: for the profile or for EDEN_NX2_STUTTER_TRACE.
inline bool g_zones_enabled = g_enabled || StutterTrace::ReadEnv();

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
    StutterTrace::ReadSwitch();
    g_zones_enabled = g_enabled || StutterTrace::Enabled();
    Common::g_count_spin_contention = g_enabled;
}

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 8> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    /// GPU waits by the zone they happened in (Other: outside every zone)
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_count{};
    Zone current{Zone::Other};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{g_zones_enabled} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        parent = t_data.current;
        t_data.current = zone;
        start = Ticks();
        traced = StutterTrace::Enabled();
        if (traced) {
            outer = StutterTrace::Open(stutter_categories[static_cast<size_t>(zone)], start,
                                       static_cast<size_t>(zone));
        }
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 end = Ticks();
        const u64 elapsed = end - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        const u64 exclusive = elapsed - data.child_ns;
        data.exclusive_ns[index] += exclusive;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
        data.current = parent;
        if (zone == Zone::GpuWait) {
            data.gpu_wait_ns[static_cast<size_t>(parent)] += elapsed;
            ++data.gpu_wait_count[static_cast<size_t>(parent)];
        }
        if (traced) {
            // Time outside every zone is no zone's: it may be a wait no zone covers.
            StutterTrace::Close(outer, end,
                                parent == Zone::Other ? StutterTrace::ZoneSlots
                                                      : static_cast<size_t>(parent));
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    Zone parent{Zone::Other};
    bool enabled;
    bool traced{};
    StutterTrace::Category outer{StutterTrace::Category::None};
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
};

/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)", static_cast<double>((*same)[method]) * 1e9 / window);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// EDEN_NX2_GPU_PROFILE: the buffer cache's work for draws, by part, logged as "<thread> buffer
/// work per second". Parts count their own time (nested parts excluded) and leave the zones as
/// they are. The parts below FindBuffer are opened by the graphics paths; the others nest in them
/// and are not counted elsewhere (compute dispatches have the compute zones). Parts are timed in
/// one draw in EDEN_NX2_BUFFER_PROFILE_EVERY (default 8) and scaled by draws over timed draws;
/// calls and amounts are counted in every draw.
enum class BufferPart : u32 {
    UniformAddress, ///< Constant buffer bind methods: the guest address translation
    StorageAddress, ///< BindGraphicsStorageBuffer: the cbuf pointer read and its range
    TextureAddress, ///< BindGraphicsTextureBuffer
    UniformKeeps,   ///< BeginGraphicsUniforms: which streamed uniform buffers a draw may keep
    ResolveIndex,   ///< UpdateIndexBuffer
    ResolveVertex,  ///< UpdateVertexBuffers
    ResolveUniform, ///< UpdateUniformBuffers
    ResolveStorage, ///< UpdateStorageBuffers
    ResolveTexture, ///< UpdateTextureBuffers
    ResolveOther,   ///< Transform feedback, indirect draw, multi-range refresh
    BindIndex,      ///< BindHostIndexBuffer
    BindVertex,     ///< BindHostVertexBuffers
    BindUniform,    ///< BindHostGraphicsUniformBuffers
    BindStorage,    ///< BindHostGraphicsStorageBuffers
    BindTexture,    ///< BindHostGraphicsTextureBuffers
    BindOther,      ///< Transform feedback and indirect draw buffers
    FindBuffer,     ///< Page table lookups (CreateBuffer excluded)
    CreateBuffer,
    VirtualRanges, ///< Multi-range storage: virtual range queries
    Sync,          ///< SynchronizeBuffer: the memory tracker's dirty words
    Upload,        ///< Staging, guest memory reads and copy commands of the uploads
    MarkWritten,   ///< MarkWrittenBuffer
    UsageMark,     ///< Buffer::MarkUsage
    UniformStream, ///< Small uniform buffers read (and compared) into the stream buffer
    HostBind,      ///< The runtime's binds: descriptor entries, vertex and index buffer binds
    Count,
};

inline constexpr size_t FirstNestedBufferPart = static_cast<size_t>(BufferPart::FindBuffer);

inline constexpr std::array<const char*, static_cast<size_t>(BufferPart::Count)> buffer_part_names{
    "uniform address", "storage address", "texture address", "uniform keeps",   "resolve index",
    "resolve vertex",  "resolve uniform", "resolve storage", "resolve texture", "resolve other",
    "bind index",      "bind vertex",     "bind uniform",    "bind storage",    "bind texture",
    "bind other",      "find buffer",     "create buffer",   "virtual ranges",  "sync",
    "upload",          "mark written",    "usage mark",      "uniform stream",  "host bind",
};
static_assert(buffer_part_names.back() != nullptr);

/// Amounts of the buffer work, per second.
enum class BufferCount : u32 {
    Draws,
    TimedDraws,
    DeltaDraws, ///< Of Draws: the fast draw delta path (GraphicsPipeline::ConfigureDelta)
    UniformBinds,
    UniformKept,       ///< Bound from the previous draw's allocation without reading memory
    UniformReused,     ///< Same bytes in the same command buffer: allocation bound again
    UniformCopied,     ///< Compared, then copied into a new allocation
    UniformSameCopied, ///< Of UniformCopied: same bytes, but another command buffer
    UniformDirect,     ///< Copied without comparing (slots whose bytes change every bind)
    UniformPlain,      ///< Streamed without the reuse path
    UniformCompareBytes,
    UniformCopyBytes,   ///< Bytes written to the stream buffer
    UniformRootTable,   ///< Bank 17 streamed from root table 0
    UniformNativeBank,  ///< 64 KiB native banks uploaded again (first bind in the frame)
    UniformCached,      ///< Bound from the cached host buffer
    UniformCachedBinds, ///< Of UniformCached: descriptor bound (others skipped as unchanged)
    StorageBinds,
    StorageWritten,
    StorageMultiRange,
    TextureBufferBinds,
    VertexSlots,   ///< Enabled vertex buffers seen per draw
    VertexRebinds, ///< Of VertexSlots: dirty, so bound to the host again
    VertexBindCalls,
    IndexBinds,
    IndexWidened, ///< 8-bit indices widened on the host
    IndexInline,
    GeometryScans, ///< SynchronizeGeometryBuffer calls
    GeometryScansSkipped,
    Syncs,
    SyncsUploaded,
    UploadCopies,
    UploadBytes,
    WrittenMarks,
    WrittenBytes,
    UsageMarks,
    FindBuffers,
    CreatedBuffers,
    LruTouches,       ///< TouchBuffer calls on cached buffers
    UniformAddresses, ///< Constant buffer bind methods
    SameUniform,      ///< Of the draw's uniform bindings, those the previous draw also had
    UniformBindings,
    SameStorage,
    StorageBindings,
    SameVertex,
    VertexBindings,
    SameIndex,
    IndexBindings,
    AllSameDraws,      ///< Draws whose buffer bindings were all the previous draw's
    AllSameCleanDraws, ///< Of AllSameDraws: nothing uploaded or marked written
    Count,
};

struct BufferData {
    std::array<u64, static_cast<size_t>(BufferPart::Count)> ticks{};
    std::array<u64, static_cast<size_t>(BufferPart::Count)> calls{};
    std::array<u64, static_cast<size_t>(BufferCount::Count)> counts{};
    u64 timed_scopes{};
    u64 child{};
    u32 depth{};
    bool timed{};
    u64 draw_index{};
};

inline thread_local BufferData t_buffer;

/// EDEN_NX2_BUFFER_PROFILE_EVERY: time the buffer parts in one draw in this many.
inline u64 BufferTimingInterval() {
    static const u64 interval = [] {
        const char* const value = std::getenv("EDEN_NX2_BUFFER_PROFILE_EVERY");
        const u64 parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 8;
        return parsed == 0 ? u64{1} : parsed;
    }();
    return interval;
}

/// A draw begins its buffer work: whether its parts are timed.
inline void BeginBufferDraw() {
    if (!g_enabled) {
        return;
    }
    auto& data = t_buffer;
    // A scrambled draw index: a fixed stride would follow a frame's repeating draw order.
    u64 x = ++data.draw_index * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    data.timed = x % BufferTimingInterval() == 0;
    ++data.counts[static_cast<size_t>(BufferCount::Draws)];
    data.counts[static_cast<size_t>(BufferCount::TimedDraws)] += data.timed ? 1 : 0;
}

/// Counts an amount of graphics buffer work (inside a part: compute's own calls stay out).
inline void Add(BufferCount counter, u64 amount = 1) {
    if (g_enabled && t_buffer.depth != 0) {
        t_buffer.counts[static_cast<size_t>(counter)] += amount;
    }
}

/// Counts an amount outside the parts (per draw).
inline void AddDrawCount(BufferCount counter, u64 amount = 1) {
    if (g_enabled) {
        t_buffer.counts[static_cast<size_t>(counter)] += amount;
    }
}

class BufferScope {
public:
    explicit BufferScope(BufferPart part_) : part{part_} {
        auto& data = t_buffer;
        enabled =
            g_enabled && (static_cast<size_t>(part) < FirstNestedBufferPart || data.depth != 0);
        if (!enabled) {
            return;
        }
        ++data.depth;
        ++data.calls[static_cast<size_t>(part)];
        timed = data.timed;
        if (!timed) {
            return;
        }
        parent_child = data.child;
        data.child = 0;
        start = Ticks();
    }

    ~BufferScope() {
        if (!enabled) {
            return;
        }
        auto& data = t_buffer;
        --data.depth;
        if (!timed) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        data.ticks[static_cast<size_t>(part)] += elapsed - data.child;
        data.child = data.depth == 0 ? 0 : parent_child + elapsed;
        ++data.timed_scopes;
    }

    BufferScope(const BufferScope&) = delete;
    BufferScope& operator=(const BufferScope&) = delete;

private:
    BufferPart part;
    bool enabled{};
    bool timed{};
    u64 start{};
    u64 parent_child{};
};

/// Ticks one BufferScope's two timer reads cost.
inline u64 BufferScopeCost() {
    static const u64 cost = [] {
        u64 best = ~u64{0};
        for (int round = 0; round < 16; ++round) {
            const u64 begin = Ticks();
            for (int i = 0; i < 256; ++i) {
                (void)Ticks();
            }
            best = (std::min)(best, (Ticks() - begin) / 256);
        }
        return best * 2;
    }();
    return cost;
}

inline void ReportBuffers(const char* thread_name, u64 window, u64 window_ticks) {
    auto& data = t_buffer;
    const auto count = [&](BufferCount counter) {
        return data.counts[static_cast<size_t>(counter)];
    };
    const u64 draws = count(BufferCount::Draws);
    if (draws == 0) {
        data.counts = {};
        data.calls = {};
        data.ticks = {};
        data.timed_scopes = 0;
        return;
    }
    const auto per_second = [&](u64 amount) { return static_cast<double>(amount) * 1e9 / window; };
    const auto kib = [&](u64 bytes) { return per_second(bytes) / 1024.0; };
    const auto c = [&](BufferCount counter) { return per_second(count(counter)); };
    const u64 timed_draws = (std::max<u64>)(count(BufferCount::TimedDraws), 1);
    const double scale = static_cast<double>(draws) / timed_draws;
    const auto ms = [&](u64 ticks) {
        return static_cast<double>(ticks) * scale * 1000.0 / window_ticks;
    };
    std::string line;
    u64 total{};
    for (size_t i = 0; i < data.ticks.size(); ++i) {
        total += data.ticks[i];
        if (data.calls[i] != 0) {
            line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", buffer_part_names[i],
                                ms(data.ticks[i]), per_second(data.calls[i]));
        }
    }
    const double scope_ns = static_cast<double>(BufferScopeCost()) * window / window_ticks;
    line += fmt::format(" | parts {:.1f} ms, timers about {:.1f} ms ({:.0f} ns a part, timed 1 "
                        "draw in {:.1f})",
                        ms(total), ms(data.timed_scopes * BufferScopeCost()), scope_ns, scale);
    line += fmt::format(" | draws {:.0f} ({:.0f} delta)", c(BufferCount::Draws),
                        c(BufferCount::DeltaDraws));
    line += fmt::format(
        " | uniform binds {:.0f}: {:.0f} kept, {:.0f} reused, {:.0f} copied ({:.0f} same bytes), "
        "{:.0f} direct, {:.0f} plain, {:.0f} root table, {:.0f} cached ({:.0f} bound); "
        "{:.0f} KiB compared, {:.0f} KiB copied; {:.0f} native bank uploads; {:.0f} cbuf "
        "address binds",
        c(BufferCount::UniformBinds), c(BufferCount::UniformKept), c(BufferCount::UniformReused),
        c(BufferCount::UniformCopied), c(BufferCount::UniformSameCopied),
        c(BufferCount::UniformDirect), c(BufferCount::UniformPlain),
        c(BufferCount::UniformRootTable), c(BufferCount::UniformCached),
        c(BufferCount::UniformCachedBinds), kib(count(BufferCount::UniformCompareBytes)),
        kib(count(BufferCount::UniformCopyBytes)), c(BufferCount::UniformNativeBank),
        c(BufferCount::UniformAddresses));
    line += fmt::format(" | storage binds {:.0f} ({:.0f} written, {:.0f} multi-range) | texture "
                        "buffer binds {:.0f}",
                        c(BufferCount::StorageBinds), c(BufferCount::StorageWritten),
                        c(BufferCount::StorageMultiRange), c(BufferCount::TextureBufferBinds));
    line += fmt::format(" | vertex slots {:.0f} ({:.0f} rebound, {:.0f} bind calls) | index binds "
                        "{:.0f} ({:.0f} widened, {:.0f} inline) | geometry scans {:.0f} ({:.0f} "
                        "skipped)",
                        c(BufferCount::VertexSlots), c(BufferCount::VertexRebinds),
                        c(BufferCount::VertexBindCalls), c(BufferCount::IndexBinds),
                        c(BufferCount::IndexWidened), c(BufferCount::IndexInline),
                        c(BufferCount::GeometryScans), c(BufferCount::GeometryScansSkipped));
    line += fmt::format(
        " | syncs {:.0f} ({:.0f} uploaded, {:.0f} copies, {:.0f} KiB) | written "
        "marks {:.0f} ({:.0f} KiB) | usage marks {:.0f} | LRU touches {:.0f} | find "
        "buffer {:.0f} ({:.0f} created)",
        c(BufferCount::Syncs), c(BufferCount::SyncsUploaded), c(BufferCount::UploadCopies),
        kib(count(BufferCount::UploadBytes)), c(BufferCount::WrittenMarks),
        kib(count(BufferCount::WrittenBytes)), c(BufferCount::UsageMarks),
        c(BufferCount::LruTouches), c(BufferCount::FindBuffers), c(BufferCount::CreatedBuffers));
    line += fmt::format(" | same as the previous draw: uniform {:.0f} of {:.0f}, storage {:.0f} of "
                        "{:.0f}, vertex {:.0f} of {:.0f}, index {:.0f} of {:.0f}; draws all same "
                        "{:.0f} ({:.0f} with nothing uploaded or written)",
                        c(BufferCount::SameUniform), c(BufferCount::UniformBindings),
                        c(BufferCount::SameStorage), c(BufferCount::StorageBindings),
                        c(BufferCount::SameVertex), c(BufferCount::VertexBindings),
                        c(BufferCount::SameIndex), c(BufferCount::IndexBindings),
                        c(BufferCount::AllSameDraws), c(BufferCount::AllSameCleanDraws));
    LOG_INFO(HW_GPU, "{} buffer work per second:{}", thread_name, line);
    const u32 depth = data.depth;
    const u64 child = data.child;
    const bool timed = data.timed;
    const u64 draw_index = data.draw_index;
    data = {};
    data.depth = depth;
    data.child = child;
    data.timed = timed;
    data.draw_index = draw_index;
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
/// `report_all_contention`: also log (and reset) every thread's spin mutex contention.
inline void Report(const char* thread_name = "GPU thread", bool report_all_contention = false) {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window_ticks;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / window_ticks,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    std::string gpu_waits;
    for (size_t i = 0; i < data.gpu_wait_count.size(); ++i) {
        if (data.gpu_wait_count[i] != 0) {
            gpu_waits += fmt::format(" {} {:.1f} ms ({:.0f}/s),", zone_names[i],
                                     static_cast<double>(data.gpu_wait_ns[i]) * 1000.0 / window_ticks,
                                     static_cast<double>(data.gpu_wait_count[i]) * 1e9 / window);
        }
    }
    if (!gpu_waits.empty()) {
        gpu_waits.pop_back();
        line += " | GPU waits by zone:" + gpu_waits;
    }
    {
        auto& own = Common::t_spin_contention;
        auto& all = Common::g_spin_contention;
        line += fmt::format(
            " | spin mutex contention: this thread {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
            static_cast<double>(own.count) * 1e9 / window, static_cast<double>(own.ns) / 1e6 * 1e9 / window,
            static_cast<double>(own.sleeps) * 1e9 / window);
        own = {};
        if (report_all_contention) {
            const u64 count = all.count.exchange(0, std::memory_order_relaxed);
            const u64 ns = all.ns.exchange(0, std::memory_order_relaxed);
            const u64 sleeps = all.sleeps.exchange(0, std::memory_order_relaxed);
            line += fmt::format(", all threads {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
                                static_cast<double>(count) * 1e9 / window,
                                static_cast<double>(ns) / 1e6 * 1e9 / window,
                                static_cast<double>(sleeps) * 1e9 / window);
        }
    }
    if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
        line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                            static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window,
                            static_cast<double>(window) / 1e6);
    } else {
        line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                            static_cast<double>(window) / 1e6);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
        LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
    }
    if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
        LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
    }
    ReportBuffers(thread_name, window, window_ticks);
    t_method_count = {};
    t_method_same = {};
    t_dma_reads = {};
    const Zone current = data.current;
    data = {};
    data.current = current;
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
}

} // namespace VideoCommon::Nx2Profile
