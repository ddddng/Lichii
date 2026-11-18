// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/api_version.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/system_data_view.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using Service::SystemDataAsset;
using Service::SystemDataView;
using Service::NS::IPlatformServiceManager;
using Service::NS::SharedFontType;
using Service::Set::FirmwareVersionFormat;
using Service::Set::ISystemSettingsServer;

constexpr u64 VersionTitle = 0x0100000000000809;

FileSys::VirtualDir SingleFile(std::vector<u8> bytes, std::string name) {
    return std::make_shared<FileSys::VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), std::move(name)),
    });
}

FileSys::VirtualDir VersionRomFS(u8 major, u8 revision_minor) {
    FirmwareVersionFormat version{};
    version.major = major;
    version.revision_minor = revision_minor;
    std::vector<u8> bytes(sizeof(version));
    std::memcpy(bytes.data(), &version, bytes.size());
    return SingleFile(std::move(bytes), "file");
}

FileSys::VirtualDir FontRomFS(size_t words, size_t font_index) {
    std::vector<u8> bytes((words + 2) * sizeof(u32));
    size_t offset = 0;
    Service::NS::EncryptSharedFont(std::vector<u32>(words, 0x12345678), bytes, offset);
    return SingleFile(std::move(bytes), Service::NS::SHARED_FONTS[font_index].second);
}

void WriteLegacyRecord(const FileSys::VirtualDir& registered, u64 title,
                       FileSys::VirtualDir directory, u8 id) {
    using namespace FileSys;
    const auto romfs = CreateRomFS(std::move(directory));
    REQUIRE(romfs);

    NcaHeader header{};
    header.magic = NcaHeader::MagicDecrypted;
    header.content_type = NcaHeader::ContentType::Data;
    header.program_id = title;
    header.sdk_addon_version = 0x000C0000;
    header.fs_info[0].start_sector = 6;
    header.fs_info[0].end_sector = 6 + static_cast<u32>((romfs->GetSize() + 0x1FF) / 0x200);
    header.content_size = header.fs_info[0].end_sector * 0x200ULL;
    NcaFsHeader fs{};
    fs.version = 2;
    fs.fs_type = NcaFsHeader::FsType::RomFs;
    fs.hash_type = NcaFsHeader::HashType::None;
    fs.encryption_type = NcaFsHeader::EncryptionType::None;
    std::vector<u8> bytes(header.content_size);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + NcaHeader::Size, &fs, sizeof(fs));
    REQUIRE(romfs->Read(bytes.data() + 0xC00, romfs->GetSize()) == romfs->GetSize());

    const NcaID nca_id{id};
    const auto name = Common::HexToString(nca_id, false);
    const auto nca = registered->CreateFile(name + ".nca");
    REQUIRE(nca);
    REQUIRE(nca->WriteBytes(bytes) == bytes.size());

    CNMTHeader metadata{};
    metadata.title_id = title;
    metadata.type = TitleType::SystemDataArchive;
    metadata.number_content_entries = 1;
    const auto meta_dir = GetOrCreateDirectoryRelative(registered, "yuzu_meta");
    REQUIRE(meta_dir);
    const auto meta = meta_dir->CreateFile(name + ".cnmt");
    REQUIRE(meta);
    const auto cnmt =
        CNMT{metadata, {}, {ContentRecord{{}, nca_id, {}, ContentRecordType::Data, 0}}, {}}
            .Serialize();
    REQUIRE(meta->WriteBytes(cnmt) == cnmt.size());
}

} // namespace

TEST_CASE("System data is selected before NS loads fonts and SET serves firmware version",
          "[core][native_home_system_data]") {
    SysmixTests::Context context;
    auto& system = context.system;
    auto& kernel = system.Kernel();
    auto& fsc = system.GetFileSystemController();
    const auto original_sd = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir,
                           Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    SCOPE_EXIT {
        Tests::RestoreEdenPath(Common::FS::EdenPath::SDMCDir, original_sd);
    };
    fsc.CreateFactories(*system.GetFilesystem());

    ISystemSettingsServer legacy_set{system};
    FirmwareVersionFormat version{};
    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == HLE::ApiVersion::HOS_VERSION_MAJOR);

    const auto registered =
        fsc.GetSystemNANDContentDirectory()->GetSubdirectory("registered");
    REQUIRE(registered);
    WriteLegacyRecord(registered, VersionTitle, VersionRomFS(7, 5), 1);
    WriteLegacyRecord(registered, static_cast<u64>(Service::NS::FontArchives::Standard),
                      FontRomFS(1, 0), 2);
    fsc.GetSystemNANDContents()->Refresh();

    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &*context.process).IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("font-data-test", [&] { server.LoopProcess(); }));
    const auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    const auto font_query = [&](IPlatformServiceManager& ns, u32 command, SharedFontType type) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(9);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = 0x49434653;
        words[6] = command;
        words[8] = static_cast<u32>(type);

        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        ns.InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        if (command == 2010) {
            CHECK((request.CommandBuffer()[8] != 0) == (request.CommandBuffer()[11] != 0));
            return u32{request.CommandBuffer()[11]};
        }
        return u32{request.CommandBuffer()[8]};
    };

    constexpr auto standard = SharedFontType::JapanUSEuropeStandard;
    constexpr auto korean = SharedFontType::KoreanHangul;
    IPlatformServiceManager legacy_ns{system, "pl:u"};
    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 7);
    CHECK(version.revision_minor == 5);
    CHECK(font_query(legacy_ns, 2, standard) == 4);
    CHECK(font_query(legacy_ns, 2010, korean) > 0);

    auto isolated = std::make_shared<const SystemDataView>(SystemDataView::Records{
        {SystemDataAsset::FirmwareVersion, VersionRomFS(23, 9)},
        {SystemDataAsset::FontStandard, FontRomFS(3, 0)},
        {SystemDataAsset::FontKorean, std::make_shared<FileSys::VectorVfsDirectory>()},
    });
    IPlatformServiceManager native_ns{system, "pl:u", isolated.get()};
    ISystemSettingsServer native_set{system, std::move(isolated)};
    REQUIRE(native_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 23);
    CHECK(version.revision_minor == 9);
    REQUIRE(native_set.GetFirmwareVersion(&version).IsSuccess());
    CHECK(version.major == 23);
    CHECK(version.revision_minor == 0);
    CHECK(font_query(native_ns, 2010, standard) == 12);
    CHECK(font_query(native_ns, 2010, korean) == 0);

    auto missing = std::make_shared<const SystemDataView>(SystemDataView::Records{
        {SystemDataAsset::FontKorean, FontRomFS(2, 4)},
    });
    IPlatformServiceManager missing_ns{system, "pl:u", missing.get()};
    ISystemSettingsServer missing_set{system, std::move(missing)};
    CHECK(missing_set.GetFirmwareVersion2(&version) == FileSys::ResultTargetNotFound);
    CHECK(version.major == 23);
    CHECK(font_query(missing_ns, 2010, standard) == 0);
    CHECK(font_query(missing_ns, 2010, korean) == 8);
    CHECK(font_query(missing_ns, 2, standard) == 0);
    CHECK(font_query(missing_ns, 2, korean) == 8);

    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 7);
    CHECK(font_query(legacy_ns, 2, standard) == 4);
}
