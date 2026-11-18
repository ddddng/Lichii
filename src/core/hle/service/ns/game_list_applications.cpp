// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/ns/game_list_applications.h"
#include "core/loader/loader.h"

namespace Service::NS {

namespace {

const FileSys::ManualContentProvider* GameListProvider(Core::System& system) {
    return static_cast<const FileSys::ManualContentProvider*>(
        system.GetContentProviderUnion().GetSlotProvider(
            FileSys::ContentProviderUnionSlot::FrontendManual));
}

} // namespace

std::optional<FileSys::GameListApplication> GetGameListApplication(Core::System& system,
                                                                 u64 application_id) {
    const auto* const provider = GameListProvider(system);
    return provider != nullptr ? provider->GetApplicationFile(application_id) : std::nullopt;
}

std::vector<u64> ListGameListApplications(Core::System& system) {
    const auto* const provider = GameListProvider(system);
    return provider != nullptr ? provider->ListApplicationFiles() : std::vector<u64>{};
}

FileSys::PatchManager::Metadata GetApplicationControlMetadata(Core::System& system,
                                                              u64 application_id) {
    const FileSys::PatchManager pm{application_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    auto metadata = pm.GetControlMetadata();
    if (metadata.first != nullptr && metadata.second != nullptr) {
        return metadata;
    }

    const auto application = GetGameListApplication(system, application_id);
    const auto loader =
        application ? Loader::GetLoader(system, application->file, application_id) : nullptr;
    if (loader == nullptr) {
        return metadata;
    }

    if (metadata.first == nullptr) {
        auto nacp = std::make_unique<FileSys::NACP>();
        if (loader->ReadControlData(*nacp) == Loader::ResultStatus::Success) {
            metadata.first = std::move(nacp);
        }
    }
    if (metadata.second == nullptr) {
        std::vector<u8> icon;
        if (loader->ReadIcon(icon) == Loader::ResultStatus::Success && !icon.empty()) {
            metadata.second = std::make_shared<FileSys::VectorVfsFile>(std::move(icon), "icon");
        }
    }
    return metadata;
}

} // namespace Service::NS
