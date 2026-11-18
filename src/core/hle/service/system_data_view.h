// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "core/file_sys/vfs/vfs.h"

namespace Service {

enum class SystemDataAsset {
    FirmwareVersion,
    FontExtension,
    FontStandard,
    FontKorean,
    FontChineseTraditional,
    FontChineseSimple,
};

class SystemDataView final {
public:
    using Records = std::map<SystemDataAsset, std::shared_ptr<const FileSys::VfsDirectory>>;

    explicit SystemDataView(Records immutable_romfs) : records{std::move(immutable_romfs)} {}

    [[nodiscard]] std::shared_ptr<const FileSys::VfsFile> OpenFile(
        SystemDataAsset asset, std::string_view name) const {
        const auto record = records.find(asset);
        if (record == records.end() || !record->second) {
            return nullptr;
        }
        return record->second->GetFile(name);
    }

private:
    const Records records;
};

} // namespace Service
