#include "test_support.h"

#include "core/identity/filesystem_id.h"
#include "core/identity/matcher.h"

#include <algorithm>

namespace {

va::VolumeInfo volume(std::string uuid, std::string serial, std::uint64_t device_capacity = 1000000)
{
    va::VolumeInfo info;
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = std::move(uuid);
    info.filesystem_uuid_short =
        info.filesystem_uuid.size() == 16 ? info.filesystem_uuid.substr(8) : "";
    info.volume_label = "ARCHIVE";
    info.capacity_bytes = 999000;
    va::PhysicalDeviceInfo device;
    device.model = "WDC WD80EFAX";
    device.serial = std::move(serial);
    device.capacity_bytes = device_capacity;
    info.devices.push_back(device);
    return info;
}

va::ArchiveCandidate candidate(std::string uuid, va::VolumeInfo identity, std::string root = "")
{
    va::ArchiveCandidate archive;
    archive.archive_uuid = std::move(uuid);
    archive.identity = std::move(identity);
    archive.root_subpath = std::move(root);
    return archive;
}

va::MediaEvidence evidence(va::VolumeInfo seen)
{
    va::MediaEvidence result;
    result.volume = std::move(seen);
    return result;
}

bool has_detail(const va::MatchOutcome& outcome, std::string_view detail)
{
    return std::find(outcome.details.begin(), outcome.details.end(), detail) !=
           outcome.details.end();
}

const std::string disk_a = "1111222233334444";
const std::string disk_b = "5555666677778888";

void decision_table_rows()
{
    const std::vector archives{candidate("A", volume(disk_a, "SERIAL-1"))};

    auto outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "", archives);
    CHECK(outcome.result == va::MatchResult::Match);
    CHECK(outcome.archives == std::vector<va::Uuid>{"A"});

    outcome = va::match_identity(evidence(volume(disk_a, "")), "", archives);
    CHECK(outcome.result == va::MatchResult::MatchWeak);

    outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-2")), "", archives);
    CHECK(outcome.result == va::MatchResult::Migrated);

    outcome = va::match_identity(evidence(volume(disk_b, "SERIAL-1")), "", archives);
    CHECK(outcome.result == va::MatchResult::Mismatch);
    CHECK(outcome.archives == std::vector<va::Uuid>{"A"});

    outcome = va::match_identity(evidence(volume(disk_b, "SERIAL-2")), "", archives);
    CHECK(outcome.result == va::MatchResult::Unknown);
    CHECK(outcome.archives.empty());

    const std::vector clones{
        candidate("A", volume(disk_a, "SERIAL-1")), candidate("B", volume(disk_a, "SERIAL-1"))};
    outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "", clones);
    CHECK(outcome.result == va::MatchResult::Ambiguous);
    CHECK(outcome.archives.size() == 2);
}

void copied_identity_file_is_a_mismatch()
{
    const std::vector archives{
        candidate("A", volume(disk_a, "SERIAL-1")), candidate("B", volume(disk_b, "SERIAL-2"))};

    auto seen = evidence(volume(disk_a, "SERIAL-1"));
    seen.claimed_archive_uuid = "B";
    auto outcome = va::match_identity(seen, "", archives);
    CHECK(outcome.result == va::MatchResult::Mismatch);
    CHECK(has_detail(outcome, "identity-file-copied"));

    auto unknown_media = evidence(volume("9999999999999999", "OTHER"));
    unknown_media.claimed_archive_uuid = "A";
    outcome = va::match_identity(unknown_media, "", archives);
    CHECK(outcome.result == va::MatchResult::Mismatch);
    CHECK(outcome.archives == std::vector<va::Uuid>{"A"});
}

// Decision 0001: archives on one volume are told apart by root_subpath,
// compared case-insensitively.
void root_subpath_narrows_candidates()
{
    const std::vector archives{
        candidate("2025", volume(disk_a, "SERIAL-1"), "Backups/2025"),
        candidate("2026", volume(disk_a, "SERIAL-1"), "Backups/2026"),
    };
    auto outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "Backups/2026", archives);
    CHECK(outcome.result == va::MatchResult::Match);
    CHECK(outcome.archives == std::vector<va::Uuid>{"2026"});

    outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "backups/2026", archives);
    CHECK(outcome.archives == std::vector<va::Uuid>{"2026"});

    outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "Backups", archives);
    CHECK(outcome.result == va::MatchResult::Unknown);
}

// Decisions 0003 and 0004.
void retired_and_previous_carriers_are_recognised()
{
    auto retired = candidate("A", volume(disk_a, "SERIAL-1"));
    retired.retired = true;
    auto outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1")), "", std::vector{retired});
    CHECK(outcome.result == va::MatchResult::Match);
    CHECK(has_detail(outcome, "retired"));

    auto relocated = candidate("A", volume(disk_b, "NEW-DISK"));
    relocated.previous_carriers.push_back({volume(disk_a, "OLD-DISK"), ""});
    outcome = va::match_identity(evidence(volume(disk_a, "OLD-DISK")), "", std::vector{relocated});
    CHECK(outcome.result == va::MatchResult::PreviousCarrier);
    CHECK(outcome.archives == std::vector<va::Uuid>{"A"});
}

// R-95: secondary evidence adds details; a resized device behind the same serial
// is a replaced device.
void secondary_evidence()
{
    const std::vector archives{candidate("A", volume(disk_a, "SERIAL-1", 1000000))};

    auto relabelled = volume(disk_a, "SERIAL-1");
    relabelled.volume_label = "RENAMED";
    relabelled.devices.front().model = "wdc_wd80efax"; // udev spelling is not a difference
    auto outcome = va::match_identity(evidence(relabelled), "", archives);
    CHECK(outcome.result == va::MatchResult::Match);
    CHECK(has_detail(outcome, "volume-label-differs"));
    CHECK(!has_detail(outcome, "model-differs"));

    outcome = va::match_identity(evidence(volume(disk_a, "SERIAL-1", 2000000)), "", archives);
    CHECK(outcome.result == va::MatchResult::Mismatch);
    CHECK(has_detail(outcome, "device-replaced"));
}

// §B1.3: a 32-bit-only NTFS reading still matches through the short form.
void ntfs_short_serial_matches_full_serial()
{
    const std::vector archives{candidate("A", volume(disk_a, "SERIAL-1"))};
    auto seen = volume("", "SERIAL-1");
    seen.filesystem_uuid_short = disk_a.substr(8);
    const auto outcome = va::match_identity(evidence(seen), "", archives);
    CHECK(outcome.result == va::MatchResult::Match);
}

void serial_comparison()
{
    CHECK(va::serials_equal("WD-WCC4E1234567", "wd-wcc4e1234567"));
    CHECK(va::serials_equal("  S3Z9NB0K123456 ", "S3Z9NB0K123456"));
    CHECK(va::serials_equal("1234ABCD", "2143BADC")); // byte-pair-swapped ATA serial
    CHECK(!va::serials_equal("1234ABCD", "1234ABCE"));
    CHECK(!va::serials_equal("", ""));
}

void filesystem_identifiers()
{
    CHECK(va::normalise_filesystem_type("NTFS") == "ntfs");
    CHECK(va::normalise_filesystem_type("ntfs3") == "ntfs");
    CHECK(va::normalise_filesystem_type("FAT32") == "vfat");
    CHECK(va::normalise_filesystem_type("exFAT") == "exfat");
    CHECK(va::normalise_filesystem_type("ext4") == "ext4");

    auto id = va::normalise_filesystem_id("ntfs", "1234ABCD5678EF90"); // blkid / 64-bit form
    CHECK(id.uuid == "1234abcd5678ef90" && id.uuid_short == "5678ef90");
    id = va::normalise_filesystem_id("ntfs", "5678EF90");               // Windows 32-bit form
    CHECK(id.uuid.empty() && id.uuid_short == "5678ef90");
    id = va::normalise_filesystem_id("exfat", "ABCD-1234");
    CHECK(id.uuid == "abcd1234" && id.uuid_short == "abcd1234");
    CHECK(va::nvme_wwn_from_windows_serial("E823_8FA6_BF53_0001_001B_448B_47F1_5232.") ==
          std::optional<std::string>("eui.e8238fa6bf530001001b448b47f15232"));
    CHECK(!va::nvme_wwn_from_windows_serial("AN0RZH3N"));
    id = va::normalise_filesystem_id("ext4", "3F2B8C1E-0D4A-4C6E-9B7A-1E2D3C4B5A69");
    CHECK(id.uuid == "3f2b8c1e-0d4a-4c6e-9b7a-1e2d3c4b5a69" && id.uuid_short.empty());
}

} // namespace

void identity_tests()
{
    decision_table_rows();
    copied_identity_file_is_a_mismatch();
    root_subpath_narrows_candidates();
    retired_and_previous_carriers_are_recognised();
    secondary_evidence();
    ntfs_short_serial_matches_full_serial();
    serial_comparison();
    filesystem_identifiers();
}
