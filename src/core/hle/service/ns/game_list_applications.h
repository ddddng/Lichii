// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

namespace Service::NS {

// The HOME Menu lists the frontend's game list next to installed applications, as forwarders:
// games in game directories, NUP and NX files included, get an application record, a name and an
// icon without being installed.

/// The game list's file for an application, or null when the game list doesn't show it.
FileSys::VirtualFile GetGameListApplicationFile(Core::System& system, u64 application_id);

/// The applications the game list shows.
std::vector<u64> ListGameListApplications(Core::System& system);

/// An application's NACP and icon: from its installed control data, else from its game list file.
FileSys::PatchManager::Metadata GetApplicationControlMetadata(Core::System& system,
                                                              u64 application_id);

} // namespace Service::NS
