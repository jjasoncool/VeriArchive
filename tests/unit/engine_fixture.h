#pragma once
// Registration/verification fixture: a fake archive volume (FakeFileSystem +
// FakeMediaEnumerator) and a real temporary repository.
#include "fakes.h"
#include "test_repository.h"

#include "core/engine/recorded_manifest.h"
#include "core/engine/register_engine.h"
#include "core/engine/transfer.h"
#include "core/engine/verify_engine.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <optional>

namespace test {

// How a registration attempt stops when it finds the revision it creates
// recorded, by another attempt or another operation (decision 0023).
inline std::string already_recorded_message(const va::Uuid& archive_uuid, std::uint32_t revision)
{
    return "revision " + std::to_string(revision) + " of archive " + archive_uuid +
           " is already recorded (by another attempt of this registration or by another operation, such as a "
           "relocation); this attempt recorded nothing";
}

// How a manifest that is missing, cannot be read or has changed is fixed
// (decision 0028): put `it` back with `import` of its file (point 7), and
// where nothing holds it, for the archive's latest revision (`latest`)
// register the next revision over the lost manifest (point 8), or, where the
// device is gone as well, retire the archive (point 9), for an earlier one
// leave it out of the index with `repo rebuild-index` (point 6). `it` names
// the manifest: "it" in the plain message, "its manifest" in a verification
// by root that stops on it (point 4).
inline std::string put_back_steps(const va::Uuid& archive_uuid, std::uint32_t revision, bool latest = true,
    const std::string& it = "it")
{
    auto text = "put " + it + " back with `import <folder>/" + std::format("r{:04}.vam", revision) +
                "` from a backup of the repository, an export of revision " + std::to_string(revision) +
                ", or the media copy in the archive's .veriarchive folder, where registration wrote one";
    if (latest) {
        text += "; if none of these holds it, register revision " + std::to_string(revision + 1) +
                " from what the archive's root holds now with `register <root_path> --lost-manifest " + archive_uuid +
                "`; if the device that held the archive is gone as well, retire the archive with `archive retire " +
                archive_uuid + " --confirm`";
    } else {
        text += "; if none of these holds it, leave revision " + std::to_string(revision) +
                " out of the index with `repo rebuild-index`";
    }
    return text;
}

// How the manifest of `revision` of a retired archive is put back (decision
// 0028, point 7), without the last resort, as a retired archive takes no new
// revision (point 9): what comes after what is wrong in its plain message.
inline std::string put_back_only(std::uint32_t revision)
{
    return "; put it back with `import <folder>/" + std::format("r{:04}.vam", revision) +
           "` from a backup of the repository, an export of revision " + std::to_string(revision) +
           ", or the media copy in the archive's .veriarchive folder, where registration wrote one";
}

// How the plain message of such a manifest says to fix it: what comes after
// what is wrong.
inline std::string put_back(const va::Uuid& archive_uuid, std::uint32_t revision, bool latest = true)
{
    return "; " + put_back_steps(archive_uuid, revision, latest);
}

// `manifest` with the type of its first file entry made "x", and the body's
// BLAKE3 in its header made the one of the body so changed (sample bytes):
// its body matches its header, and an entry does not parse.
inline std::string with_an_entry_that_does_not_parse(const std::string& manifest)
{
    const auto newline = manifest.find('\n');
    auto header = manifest.substr(0, newline + 1);
    auto body = manifest.substr(newline + 1);
    const auto body_hash = header.find(va::blake3_hex(body));
    const auto entry_type = body.find("\"t\":\"f\"");
    CHECK(body_hash != std::string::npos && entry_type != std::string::npos);
    if (body_hash == std::string::npos || entry_type == std::string::npos) {
        return manifest;
    }
    body[entry_type + 5] = 'x';
    header.replace(body_hash, 64, va::blake3_hex(body));
    return header + body;
}

// SQL text: `text` between single quotes, a quote in it doubled.
inline std::string sql_text(const std::string& text)
{
    std::string quoted = "'";
    for (const char character : text) {
        quoted += character;
        if (character == '\'') {
            quoted += '\'';
        }
    }
    return quoted + "'";
}

// The index's record of `revision` of the archive made to name the manifest
// whose bytes are `manifest` (sample damage): its line 1 as the index's copy
// of the header, the BLAKE3 of that line, and the body's BLAKE3 that the line
// names, as if the revision had been recorded so.
inline void record_as_written(
    const TestRepository& repository, const va::Uuid& archive_uuid, std::uint32_t revision, const std::string& manifest)
{
    const auto line = manifest.substr(0, manifest.find('\n'));
    const auto header = nlohmann::json::parse(line, nullptr, false);
    CHECK(header.is_object() && header.contains("body"));
    if (!header.is_object() || !header.contains("body")) {
        return;
    }
    repository.execute(("UPDATE archive_revisions SET header_json = " + sql_text(line) + ", header_blake3 = " +
                        sql_text(va::blake3_hex(line)) + ", body_blake3 = " +
                        sql_text(header["body"]["blake3"].get<std::string>()) + " WHERE archive_uuid = " +
                        sql_text(archive_uuid) + " AND revision = " + std::to_string(revision))
                           .c_str());
}

inline va::VolumeInfo fake_volume(const std::filesystem::path& mount_point)
{
    va::VolumeInfo volume;
    volume.mount_point = mount_point;
    volume.filesystem_type = "ntfs";
    volume.filesystem_uuid = "1111222233334444";
    volume.filesystem_uuid_short = "33334444";
    volume.volume_label = "ARCHIVE";
    volume.capacity_bytes = 1000000;
    va::PhysicalDeviceInfo device;
    device.model = "Test Disk";
    device.serial = "SERIAL-1";
    device.rotational = true;
    volume.devices.push_back(device);
    return volume;
}

struct EngineFixture {
    TestRepository repository;
    FakeFileSystem archive;
    FakeMediaEnumerator media;
    va::ArchiveStore store = repository.store();
    va::CancelToken cancel;
    NullProgress progress;
    va::HostInfo host{"windows", "test-host", "0.1.0"};
    FakeMediaWriter media_writer{archive};
    FakeProcessTable processes;  // the engines run in its `self` (decision 0024)
    // How the engines read the media: through `archive` itself unless a test
    // puts a view of it here, such as a device that goes away (RemovableDevice).
    va::IFileSystemAccess* media_files = nullptr;
    // How the file system names a path (RegisterServices::final_path); none:
    // the paths' text alone is compared.
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)> final_path;

    // An archive volume whose root, and mount point, is `root`.
    explicit EngineFixture(std::filesystem::path root = "/vol") : archive(std::move(root))
    {
        media.volume = fake_volume(archive.root());
    }

    va::IFileSystemAccess& archive_files() { return media_files ? *media_files : archive; }

    std::expected<va::RegisterOutcome, va::RegisterFailure> register_root(va::RegisterRequest request)
    {
        va::RegisterServices services{archive_files(), *repository.files, *repository.writer, media_writer, media,
            store, repository.context.repo, host, processes, final_path};
        return va::register_archive(services, request, cancel, progress);
    }

    std::expected<va::RegisterOutcome, va::RegisterFailure> register_root(const std::filesystem::path& root)
    {
        va::RegisterRequest request;
        request.root = root;
        return register_root(request);
    }

    std::expected<va::VerifyOutcome, va::VerifyStop> verify(va::VerifyRequest request)
    {
        va::VerifyServices services{archive_files(), *repository.files, media, store, repository.context.repo,
            repository.context.repository_uuid, host, *repository.writer, processes};
        return va::verify_full(services, request, cancel, progress);
    }

    std::expected<va::VerifyOutcome, va::VerifyStop> verify(const std::filesystem::path& root)
    {
        va::VerifyRequest request;
        request.root = root;
        return verify(request);
    }
};

// Imports revision `revision` of the archive `archive_uuid` through `store`,
// as the operator imports an export of another repository that recorded the
// revisions between (sample data): the header and entries of the archive's
// current revision here, renumbered and written by "another-repository" into
// a folder of exports beside the repository. The archive then has a later
// revision than an unfinished registration or relocation of it here was made
// for (decision 0030).
inline void import_later_revision(
    EngineFixture& fixture, va::ArchiveStore& store, const va::Uuid& archive_uuid, std::uint32_t revision)
{
    const auto archive = store.find_archive(archive_uuid);
    if (!archive || !*archive) {
        CHECK(false);
        return;
    }
    const auto current = va::load_recorded_manifest(
        store, *fixture.repository.files, fixture.repository.context.repo, archive_uuid, (*archive)->current_revision);
    CHECK(current.has_value());
    if (!current) {
        return;
    }
    const auto directory = fixture.repository.context.repo.root.parent_path() / "exports" / std::to_string(revision);
    std::filesystem::create_directories(directory);
    const auto path = directory / std::format("r{:04}.vam", revision);
    va::ManifestWriter writer(*fixture.repository.writer, *fixture.repository.files, path, 0);
    for (const auto& entry : current->entries) {
        CHECK(writer.add(entry).has_value());
    }
    auto header = current->header;
    header.revision = revision;
    header.supersedes = revision - 1;
    header.host = "another-repository";
    const auto written = writer.finish(header);
    CHECK(written && written->file->commit().has_value());
    const auto imported = va::import_manifest(
        store, *fixture.repository.files, *fixture.repository.writer, fixture.repository.context.repo, path, false, test::not_cancelled(), test::no_progress());
    CHECK(imported && imported->revision == revision && !imported->new_archive);
    if (!imported) {
        std::cerr << "  import of revision " << revision << ": " << imported.error().message << '\n';
    }
}

} // namespace test
