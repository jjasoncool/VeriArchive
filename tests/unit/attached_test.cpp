// Which attached volume holds which archive (core/engine/attached.h, decision
// 0046). Every volume, device and archive below is sample data made up for
// the test (SAMPLE-* labels, models and serials, c0ffee00-... uuids, mount
// points under /media/SAMPLE-*); none is of a real volume, device or archive.
#include "test_support.h"

#include "core/engine/attached.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using va::MatchResult;

const va::Uuid photos = "c0ffee00-0000-4000-8000-000000000a01";
const va::Uuid weak = "c0ffee00-0000-4000-8000-000000000a02";
const va::Uuid moved = "c0ffee00-0000-4000-8000-000000000a03";
const va::Uuid cloned = "c0ffee00-0000-4000-8000-000000000a04";
const va::Uuid absent = "c0ffee00-0000-4000-8000-000000000a05";
const va::Uuid reformatted = "c0ffee00-0000-4000-8000-000000000a06";
const va::Uuid twin_one = "c0ffee00-0000-4000-8000-000000000a07";
const va::Uuid twin_two = "c0ffee00-0000-4000-8000-000000000a08";
const va::Uuid retired = "c0ffee00-0000-4000-8000-000000000a09";

// A sample volume: its filesystem id, and one device with `serial` (none: no
// serial, as behind an enclosure that hides it).
va::VolumeInfo volume(std::string mount_point, std::string filesystem_uuid, std::string serial)
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = std::move(filesystem_uuid);
    info.volume_label = "SAMPLE-VOLUME";
    info.capacity_bytes = 1'000'000'000'000;
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = std::move(serial);
    device.capacity_bytes = 1'000'204'886'016;
    info.devices.push_back(device);
    return info;
}

va::ArchiveCandidate candidate(const va::Uuid& uuid, va::VolumeInfo identity, std::string root_subpath)
{
    va::ArchiveCandidate archive;
    archive.archive_uuid = uuid;
    archive.identity = std::move(identity);
    archive.identity.mount_point.clear();  // a recorded identity has no mount point (§B4.3)
    archive.root_subpath = std::move(root_subpath);
    return archive;
}

const va::ArchiveAttachment& of(const va::Attachments& found, const va::Uuid& archive)
{
    for (const auto& attachment : found.archives) {
        if (attachment.archive_uuid == archive) {
            return attachment;
        }
    }
    static const va::ArchiveAttachment none;
    return none;
}

std::vector<va::Uuid> on_volume(const va::Attachments& found, std::size_t volume)
{
    std::vector<va::Uuid> archives;
    for (const auto& root : found.volumes[volume]) {
        archives.push_back(root.archive_uuid);
    }
    return archives;
}

void matched_archives_are_attached_at_their_roots()
{
    const std::vector volumes{
        volume("/media/SAMPLE-A", "1111222233334444", "SAMPLE-SERIAL-1"),
        volume("/media/SAMPLE-B", "5555666677778888", ""),
    };
    const std::vector candidates{
        candidate(photos, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "Photos/2024"),
        // Recorded with a serial, seen without one: MATCH_WEAK.
        candidate(weak, volume("", "5555666677778888", "SAMPLE-SERIAL-2"), ""),
        candidate(absent, volume("", "9999aaaabbbbcccc", "SAMPLE-SERIAL-9"), "Elsewhere"),
    };
    const auto found = va::where_attached(volumes, candidates);
    CHECK(found.archives.size() == 3 && found.volumes.size() == 2);

    const auto& first = of(found, photos);
    CHECK(first.result == MatchResult::Match);
    CHECK(first.places.size() == 1 && first.places.front().volume == 0);
    CHECK(first.places.front().root_subpath == "Photos/2024");
    CHECK(first.places.front().root == std::filesystem::path("/media/SAMPLE-A") / "Photos" / "2024");

    // At the volume root: the mount point itself.
    const auto& second = of(found, weak);
    CHECK(second.result == MatchResult::MatchWeak);
    CHECK(second.places.size() == 1 && second.places.front().volume == 1);
    CHECK(second.places.front().root == std::filesystem::path("/media/SAMPLE-B"));

    // On no attached volume.
    const auto& third = of(found, absent);
    CHECK(third.result == MatchResult::Unknown && third.places.empty());

    CHECK(on_volume(found, 0) == std::vector<va::Uuid>{photos});
    CHECK(on_volume(found, 1) == std::vector<va::Uuid>{weak});
}

void another_device_with_the_filesystem_is_migrated()
{
    // The filesystem recorded, on a device with another serial: a new
    // enclosure, or a sector-by-sector clone. Never attached, but its volume
    // lists the root.
    const std::vector volumes{volume("/media/SAMPLE-C", "1111222233334444", "SAMPLE-SERIAL-7")};
    const std::vector candidates{
        candidate(moved, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "Photos"),
        // The device recorded, with another filesystem on it: reformatted
        // (MISMATCH), which holds no archive.
        candidate(reformatted, volume("", "5555666677778888", "SAMPLE-SERIAL-7"), ""),
    };
    const auto found = va::where_attached(volumes, candidates);
    const auto& attachment = of(found, moved);
    CHECK(attachment.result == MatchResult::Migrated);
    CHECK(attachment.places.size() == 1 && attachment.places.front().match == MatchResult::Migrated);
    CHECK(attachment.places.front().root == std::filesystem::path("/media/SAMPLE-C") / "Photos");
    CHECK(of(found, reformatted).result == MatchResult::Unknown);
    CHECK(on_volume(found, 0) == std::vector<va::Uuid>{moved});
}

void an_archive_on_several_volumes_is_ambiguous()
{
    // A device and a sector-by-sector clone of it, both without a serial:
    // both MATCH_WEAK, which a verification by the archive's uuid cannot tell
    // apart. The other device of a clone with a serial is MIGRATED and does
    // not make it ambiguous.
    const std::vector volumes{
        volume("/media/SAMPLE-D1", "1111222233334444", ""),
        volume("/media/SAMPLE-D2", "1111222233334444", ""),
    };
    const std::vector candidates{candidate(cloned, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "Data")};
    auto found = va::where_attached(volumes, candidates);
    CHECK(of(found, cloned).result == MatchResult::Ambiguous);
    CHECK(of(found, cloned).places.size() == 2);
    CHECK(on_volume(found, 0) == std::vector<va::Uuid>{cloned} && on_volume(found, 1) == std::vector<va::Uuid>{cloned});

    const std::vector serials{
        volume("/media/SAMPLE-D1", "1111222233334444", "SAMPLE-SERIAL-1"),
        volume("/media/SAMPLE-D2", "1111222233334444", "SAMPLE-SERIAL-8"),
    };
    found = va::where_attached(serials, candidates);
    CHECK(of(found, cloned).result == MatchResult::Match);
    CHECK(of(found, cloned).places.size() == 1 && of(found, cloned).places.front().volume == 0);
    // The clone's volume still names the root as MIGRATED.
    CHECK(found.volumes[1].size() == 1 && found.volumes[1].front().match == MatchResult::Migrated);
}

void twins_and_retired_archives_are_each_found()
{
    // The same folder registered on purpose as two archives (R-97): each is
    // matched on its own, so each is attached there.
    const std::vector volumes{volume("/media/SAMPLE-E", "1111222233334444", "SAMPLE-SERIAL-1")};
    auto retired_candidate = candidate(retired, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "Old");
    retired_candidate.retired = true;
    const std::vector candidates{
        candidate(twin_one, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "Twins"),
        candidate(twin_two, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "twins"),
        retired_candidate,
    };
    const auto found = va::where_attached(volumes, candidates);
    CHECK(of(found, twin_one).result == MatchResult::Match);
    CHECK(of(found, twin_two).result == MatchResult::Match);
    CHECK(on_volume(found, 0) == (std::vector<va::Uuid>{twin_one, twin_two, retired}));
    // A retired archive is still recognised (decision 0003), and says so.
    const auto& old = of(found, retired);
    CHECK(old.result == MatchResult::Match && old.places.size() == 1);
    CHECK(std::ranges::find(old.places.front().details, "retired") != old.places.front().details.end());
}

void volumes_without_a_mount_point_hold_nothing()
{
    const std::vector volumes{volume("", "1111222233334444", "SAMPLE-SERIAL-1")};
    const std::vector candidates{candidate(photos, volume("", "1111222233334444", "SAMPLE-SERIAL-1"), "")};
    const auto found = va::where_attached(volumes, candidates);
    CHECK(of(found, photos).result == MatchResult::Unknown);
    CHECK(found.volumes.size() == 1 && found.volumes.front().empty());
    // Nothing attached, nothing registered.
    const auto none = va::where_attached({}, {});
    CHECK(none.archives.empty() && none.volumes.empty());
}

} // namespace

void attached_tests()
{
    matched_archives_are_attached_at_their_roots();
    another_device_with_the_filesystem_is_migrated();
    an_archive_on_several_volumes_is_ambiguous();
    twins_and_retired_archives_are_each_found();
    volumes_without_a_mount_point_hold_nothing();
}
