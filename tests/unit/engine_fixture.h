#pragma once
// Registration/verification fixture: a fake archive volume (FakeFileSystem +
// FakeMediaEnumerator) and a real temporary repository.
#include "fakes.h"
#include "test_repository.h"

#include "core/engine/register_engine.h"
#include "core/engine/verify_engine.h"

namespace test {

// How a registration attempt stops when it finds the revision it creates
// recorded, by another attempt or another operation (decision 0023).
inline std::string already_recorded_message(const va::Uuid& archive_uuid, std::uint32_t revision)
{
    return "revision " + std::to_string(revision) + " of archive " + archive_uuid +
           " is already recorded (by another attempt of this registration or by another operation, such as a "
           "relocation); this attempt recorded nothing";
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
    FakeFileSystem archive{"/vol"};
    FakeMediaEnumerator media;
    va::ArchiveStore store = repository.store();
    va::CancelToken cancel;
    NullProgress progress;
    va::HostInfo host{"windows", "test-host", "0.1.0"};
    FakeMediaWriter media_writer{archive};
    FakeProcessTable processes;  // the engines run in its `self` (decision 0024)

    EngineFixture() { media.volume = fake_volume(archive.root()); }

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

} // namespace test
