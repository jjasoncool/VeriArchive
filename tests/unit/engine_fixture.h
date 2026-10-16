#pragma once
// Registration/verification fixture: a fake archive volume (FakeFileSystem +
// FakeMediaEnumerator) and a real temporary repository.
#include "fakes.h"
#include "test_repository.h"

#include "core/engine/recorded_manifest.h"
#include "core/engine/register_engine.h"
#include "core/engine/transfer.h"
#include "core/engine/verify_engine.h"
#include "core/manifest/manifest_writer.h"

#include <format>

namespace test {

// How a registration attempt stops when it finds the revision it creates
// recorded, by another attempt or another operation (decision 0023).
inline std::string already_recorded_message(const va::Uuid& archive_uuid, std::uint32_t revision)
{
    return "revision " + std::to_string(revision) + " of archive " + archive_uuid +
           " is already recorded (by another attempt of this registration or by another operation, such as a "
           "relocation); this attempt recorded nothing";
}

// How the plain message of a manifest that is missing, cannot be read or has
// changed says to fix it (decision 0028): put it back with `import` of its
// file (point 7), and for the archive's latest revision (`latest`), where
// nothing holds it, register the next revision over the lost manifest
// (point 8). What comes after what is wrong.
inline std::string put_back(const va::Uuid& archive_uuid, std::uint32_t revision, bool latest = true)
{
    auto text = "; put it back with `import <folder>/" + std::format("r{:04}.vam", revision) +
                "` from a backup of the repository, an export of revision " + std::to_string(revision) +
                ", or the media copy in the archive's .veriarchive folder, where registration wrote one";
    if (latest) {
        text += "; if none of these holds it, register revision " + std::to_string(revision + 1) +
                " from what the archive's root holds now with `register <root_path> --lost-manifest " + archive_uuid +
                "`";
    }
    return text;
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

    // An archive volume whose root, and mount point, is `root`.
    explicit EngineFixture(std::filesystem::path root = "/vol") : archive(std::move(root))
    {
        media.volume = fake_volume(archive.root());
    }

    std::expected<va::RegisterOutcome, va::RegisterFailure> register_root(va::RegisterRequest request)
    {
        va::RegisterServices services{archive, *repository.files, *repository.writer, media_writer, media,
            store, repository.context.repo, host, processes};
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
        va::VerifyServices services{archive, *repository.files, media, store, repository.context.repo,
            repository.context.repository_uuid, host};
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
    va::ManifestWriter writer(*fixture.repository.writer, *fixture.repository.files, path);
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
        store, *fixture.repository.files, *fixture.repository.writer, fixture.repository.context.repo, path, false);
    CHECK(imported && imported->revision == revision && !imported->new_archive);
    if (!imported) {
        std::cerr << "  import of revision " << revision << ": " << imported.error().message << '\n';
    }
}

} // namespace test
