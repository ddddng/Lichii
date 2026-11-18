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

FileSys::VirtualFile GetGameListApplicationFile(Core::System& system, u64 application_id) {
    const auto* const provider = GameListProvider(system);
    return provider != nullptr ? provider->GetApplicationFile(application_id) : nullptr;
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
    if (metadata.first != nullptr) {
        return metadata;
    }

    const auto file = GetGameListApplicationFile(system, application_id);
    const auto loader = file != nullptr ? Loader::GetLoader(system, file, application_id) : nullptr;
    if (loader == nullptr) {
        return metadata;
    }

    auto nacp = std::make_unique<FileSys::NACP>();
    if (loader->ReadControlData(*nacp) != Loader::ResultStatus::Success) {
        return metadata;
    }
    std::vector<u8> icon;
    FileSys::VirtualFile icon_file;
    if (loader->ReadIcon(icon) == Loader::ResultStatus::Success && !icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(std::move(icon), "icon");
    }
    return {std::move(nacp), std::move(icon_file)};
}

} // namespace Service::NS
