// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/ns/application_manager_interface.h"
#include "core/hle/service/ns/game_list_applications.h"
#include "tests/core/system_test_context.h"

using Service::AM::AppletProgramId;
using Service::AM::IsAppletProgramId;

TEST_CASE("Applet program ids cover the HOME Menu, overlayDisp and the LLE applets only",
          "[core][am][home_menu]") {
    CHECK(IsAppletProgramId(static_cast<u64>(AppletProgramId::QLaunch)));
    CHECK(IsAppletProgramId(static_cast<u64>(AppletProgramId::OverlayDisplay)));
    CHECK(IsAppletProgramId(static_cast<u64>(AppletProgramId::Starter)));
    CHECK(IsAppletProgramId(static_cast<u64>(AppletProgramId::MaxProgramId)));
    CHECK_FALSE(IsAppletProgramId(0x0100000000000FFFULL));
    CHECK_FALSE(IsAppletProgramId(0x0100000000002000ULL));
    CHECK_FALSE(IsAppletProgramId(0x0100152000022000ULL));
    CHECK_FALSE(IsAppletProgramId(0x0400C3F00006E000ULL));
}

TEST_CASE("A HOME Menu launch in a new session reaches the frontend that can boot it",
          "[core][am][home_menu]") {
    ScopedSystemTestContext environment;
    Core::System system;
    constexpr u64 Switch2Title = 0x0400C3F00006E000ULL;

    CHECK_FALSE(system.LaunchInNewSession(Switch2Title));

    std::optional<u64> launched;
    system.RegisterLaunchInNewSessionCallback([&](u64 program_id) { launched = program_id; });
    CHECK(system.LaunchInNewSession(Switch2Title));
    CHECK(launched == Switch2Title);
}

TEST_CASE("The HOME Menu lists game list applications next to installed ones",
          "[core][ns][home_menu]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    std::optional<Kernel::KThread> thread;
    thread.emplace(system.Kernel());
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*thread, nullptr).IsSuccess());
    system.Kernel().SetCurrentEmuThread(&*thread);
    system.Kernel().Initialize();

    constexpr u64 Installed = 0x0100152000022000ULL;
    constexpr u64 InstalledSubProgram = 0x0100152000022001ULL;
    constexpr u64 Switch2Game = 0x0400C3F00006E000ULL;
    constexpr u64 Switch2Edition = 0x04004D300C5AE001ULL;

    FileSys::ManualContentProvider provider;
    const auto file = std::make_shared<FileSys::VectorVfsFile>();
    provider.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                      Installed, file);
    provider.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                      InstalledSubProgram, file);
    provider.AddApplicationFile(Installed, file);
    provider.AddApplicationFile(Switch2Game, file);
    provider.AddApplicationFile(Switch2Edition, file);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &provider);

    CHECK(Service::NS::GetGameListApplicationFile(system, Switch2Game) == file);
    CHECK(Service::NS::GetGameListApplicationFile(system, 0x0400000000000000ULL) == nullptr);

    std::vector<u64> listed;
    {
        Service::NS::IApplicationManagerInterface ns{system};
        std::array<Service::NS::ApplicationRecord, 8> records{};
        s32 count{};
        REQUIRE(ns.ListApplicationRecord(std::span<Service::NS::ApplicationRecord>{records}, &count, 0).IsSuccess());
        for (s32 i = 0; i < count; ++i) {
            listed.push_back(records[i].application_id);
        }
    }
    std::ranges::sort(listed);
    CHECK(listed == std::vector<u64>{Installed, Switch2Edition, Switch2Game});

    provider.ClearAllEntries();
    CHECK(Service::NS::ListGameListApplications(system).empty());

    system.CoreTiming().ClearPendingEvents();
    system.ShutdownMainProcess();
    system.Kernel().SetCurrentEmuThread(nullptr);
    system.Kernel().UnregisterKernelObject(&*thread);
    thread.reset();
}
