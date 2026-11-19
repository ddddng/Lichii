// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

#include "common/cpu_features.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/applet_manager.h"
#include "tests/core/arm_cpu_guest_layout.h"
#include "video_core/gpu.h"

extern "C" const char arm_cpu_guest_begin[ARM_GUEST_IMAGE_SIZE];

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    HeadlessWindow() {
        UpdateCurrentFramebufferLayout(1280, 720);
    }

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }

    bool IsShown() const override {
        return true;
    }
};

struct GuestReport {
    u32 stage;
    u32 mode;
    u64 thread_id;
    u64 frequency;
    u64 compute_ticks;
    u64 svc_ticks;
};
static_assert(offsetof(GuestReport, stage) == ARM_GUEST_STAGE);
static_assert(offsetof(GuestReport, mode) == ARM_GUEST_MODE);
static_assert(offsetof(GuestReport, thread_id) == ARM_GUEST_THREAD_ID);
static_assert(offsetof(GuestReport, frequency) == ARM_GUEST_FREQUENCY);
static_assert(offsetof(GuestReport, compute_ticks) == ARM_GUEST_COMPUTE_TICKS);
static_assert(offsetof(GuestReport, svc_ticks) == ARM_GUEST_SVC_TICKS);

struct ExitThread {
    u64 thread_id;
    u64 native_tls;
};

const char* StageName(u32 stage) {
    switch (stage) {
    case ARM_GUEST_CHECK_SVC:
        return "SVC arguments and results";
    case ARM_GUEST_CHECK_TLS:
        return "TLS registers";
    case ARM_GUEST_CHECK_TIMER:
        return "guest counter and timed sleep";
    case ARM_GUEST_CHECK_EXCLUSIVES:
        return "exclusive load/store";
    case ARM_GUEST_CHECK_PREEMPTION:
        return "same-core preemption and TLS isolation";
    case ARM_GUEST_CHECK_FAULT:
        return "unmapped load recovery";
    case ARM_GUEST_CHECK_BENCHMARK:
        return "benchmark";
    case ARM_GUEST_PASS:
        return "pass";
    default:
        return "before guest entry";
    }
}

int RunGuest(const fs::path& image, std::string_view backend, u32 mode) {
    const bool nce = backend == "nce";
    Settings::values.cpu_backend.SetValue(nce ? Settings::CpuBackend::Nce
                                              : Settings::CpuBackend::Dynarmic);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Null);
    Settings::values.use_multi_core.SetValue(true);
    Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);

    HeadlessWindow window;
    std::promise<ExitThread> exited;
    auto completion = exited.get_future();
    Core::System system;
    if (!system.Initialize()) {
        std::fputs("Failed to initialize the guest system\n", stderr);
        return EXIT_FAILURE;
    }

    Service::AM::FrontendAppletParameters params{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
    };
    const auto result = system.Load(window, image.string(), params);
    if (result != Core::SystemResultStatus::Success) {
        std::fprintf(stderr, "Failed to load synthetic NRO: %u\n", static_cast<u32>(result));
        return EXIT_FAILURE;
    }
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    auto* const process = system.Kernel().ApplicationProcess();
    if (process == nullptr || !process->IsApplication()) {
        std::fputs("NRO did not load as an application process\n", stderr);
        return EXIT_FAILURE;
    }
    const bool has_native_trampolines = !process->GetPostHandlers().empty();
    if (Settings::IsNceEnabled() != nce || has_native_trampolines != nce) {
        std::fprintf(stderr, "NRO loader did not select %s\n", backend.data());
        return EXIT_FAILURE;
    }
    auto& memory = process->GetMemory();
    const auto data = process->GetEntryPoint() + ARM_GUEST_DATA_OFFSET;
    memory.Write32(GetInteger(data) + ARM_GUEST_MODE, mode);
    system.RegisterExitCallback([&] {
        auto& thread = Kernel::GetCurrentThread(system.Kernel());
        exited.set_value({thread.GetThreadId(), thread.GetNativeExecutionParameters().tpidr_el0});
    });
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    system.Run();

    if (completion.wait_for(15s) != std::future_status::ready) {
        std::fprintf(stderr, "%s guest timed out before ExitProcess\n", backend.data());
        return EXIT_FAILURE;
    }
    const auto thread = completion.get();
    GuestReport report{};
    if (!memory.ReadBlock(data, &report, sizeof(report)) || report.stage != ARM_GUEST_PASS) {
        std::fprintf(stderr, "%s guest failed at %s\n", backend.data(), StageName(report.stage));
        return EXIT_FAILURE;
    }
    if (report.thread_id != thread.thread_id ||
        thread.native_tls != (nce ? ARM_GUEST_TLS_MARKER : 0)) {
        std::fprintf(stderr, "%s guest did not return through the expected backend and thread\n",
                     backend.data());
        return EXIT_FAILURE;
    }
    if (mode != ARM_GUEST_FAULT && report.frequency != Common::WallClock::CNTFRQ) {
        std::fprintf(stderr, "%s guest returned an unexpected counter frequency: %llu\n",
                     backend.data(), static_cast<unsigned long long>(report.frequency));
        return EXIT_FAILURE;
    }
    if (mode == ARM_GUEST_BENCHMARK) {
        if (report.compute_ticks == 0 || report.svc_ticks == 0) {
            std::fputs("Guest benchmark returned an empty interval\n", stderr);
            return EXIT_FAILURE;
        }
        const double ns_per_tick = 1e9 / static_cast<double>(report.frequency);
        std::printf("%s compute %.3f ns/iteration, GetSystemTick %.3f ns/round trip\n",
                    backend.data(),
                    static_cast<double>(report.compute_ticks) * ns_per_tick /
                        ARM_GUEST_COMPUTE_ITERATIONS,
                    static_cast<double>(report.svc_ticks) * ns_per_tick / ARM_GUEST_SVC_ITERATIONS);
    } else {
        std::printf("%s guest passed%s\n", backend.data(),
                    mode == ARM_GUEST_FAULT ? " unmapped load recovery" : "");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) try {
    const std::string_view backend = argc > 1 ? argv[1] : "";
    const std::string_view option = argc > 2 ? argv[2] : "";
    if (argc > 3 || (backend != "nce" && backend != "dynarmic") ||
        (!option.empty() && option != "--benchmark" && !(option == "--fault" && backend == "nce"))) {
        std::fputs("Usage: arm_cpu_guest_tests {nce|dynarmic} [--benchmark]\n"
                   "       arm_cpu_guest_tests nce --fault\n",
                   stderr);
        return EXIT_FAILURE;
    }
    if (sysconf(_SC_PAGESIZE) != ARM_GUEST_PAGE_SIZE) {
        std::fputs("Linux ARM64 guest tests require 4 KiB host pages\n", stderr);
        return EXIT_FAILURE;
    }

    auto scratch_template =
        fs::absolute(fs::temp_directory_path() / "lichii-arm-cpu-XXXXXX").string();
    if (mkdtemp(scratch_template.data()) == nullptr) {
        std::perror("mkdtemp");
        return EXIT_FAILURE;
    }
    const fs::path old_directory = fs::current_path();
    const fs::path scratch_dir{scratch_template};
    SCOPE_EXIT {
        std::error_code error;
        fs::current_path(old_directory, error);
        fs::remove_all(scratch_dir, error);
    };
    fs::current_path(scratch_dir);
    fs::create_directory(PORTABLE_DIR);
    if (Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) !=
        fs::current_path() / PORTABLE_DIR / NAND_DIR) {
        throw std::runtime_error{"Guest system did not select the scratch profile"};
    }

    const fs::path image = scratch_dir / "synthetic.nro";
    std::ofstream output{image, std::ios::binary};
    output.write(arm_cpu_guest_begin, sizeof(arm_cpu_guest_begin));
    output.close();
    if (!output) {
        throw std::runtime_error{"Cannot write the synthetic NRO"};
    }

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    SCOPE_EXIT {
        Common::Log::Stop();
    };
    const u32 mode = option == "--benchmark" ? ARM_GUEST_BENCHMARK
                     : option == "--fault"   ? ARM_GUEST_FAULT
                                             : ARM_GUEST_CHECKS;
    return RunGuest(image, backend, mode);
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return EXIT_FAILURE;
}
