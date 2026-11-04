// What Devices and Archives say of the devices attached now
// (src/gui/device_rows.*, decision 0046): the read put together, where each
// archive is attached, the devices' cards, and how the reads are spaced.
// Every volume, device, archive and root below is sample data made up for the
// test (SAMPLE-* labels, models, serials and system names, ARCH-SAMPLE-*
// labels, c0ffee00-... uuids, mount points under /media/SAMPLE-*); none is of
// a real volume, device or archive.
#include "test_support.h"

#include "app/attached_devices.h"
#include "common/utf8.h"
#include "gui/device_rows.h"
#include "gui/report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using va::MatchResult;
using va::app::Language;
using va::app::Text;
using va::gui::Attached;
using va::gui::IdentityShown;
using va::gui::Tone;

const va::Uuid photos = "c0ffee00-0000-4000-8000-000000000c01";
const va::Uuid whole = "c0ffee00-0000-4000-8000-000000000c02";
const va::Uuid cloned = "c0ffee00-0000-4000-8000-000000000c03";
const va::Uuid absent = "c0ffee00-0000-4000-8000-000000000c04";
const va::Uuid twin = "c0ffee00-0000-4000-8000-000000000c05";
const va::Uuid unmatched = "c0ffee00-0000-4000-8000-000000000c06";

constexpr std::uint64_t gib = std::uint64_t{1} << 30;

va::PhysicalDeviceInfo device(std::string number, std::string bus = "sample-usb")
{
    va::PhysicalDeviceInfo info;
    info.device_ref = "SAMPLE-DEVICE-REF-" + number;
    info.model = "SAMPLE-DEVICE-" + number;
    info.serial = "SAMPLE-SERIAL-" + number;
    info.capacity_bytes = std::uint64_t{1} << 40;
    info.bus = std::move(bus);
    return info;
}

va::VolumeInfo volume(std::string mount_point, std::string filesystem_uuid, std::vector<va::PhysicalDeviceInfo> devices,
    std::string label = "SAMPLE-VOLUME")
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = std::move(filesystem_uuid);
    info.volume_label = std::move(label);
    info.capacity_bytes = std::uint64_t{1} << 40;
    info.devices = std::move(devices);
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

std::string native(const std::filesystem::path& path)
{
    return va::path_to_utf8(path);
}

// The archives: Photos in a folder of volume A, the whole of volume B, a
// folder whose filesystem is on volume C on another device than recorded (a
// clone or another enclosure), one whose volume is not attached, and one on
// a filesystem that two volumes hold (a device and its clone, neither with a
// serial to tell them apart).
std::vector<va::ArchiveCandidate> candidates()
{
    auto twin_identity = volume("", "c0ffee00-0000-4000-8000-0000000000e5", {device("05")});
    twin_identity.devices.front().serial.clear();
    return {
        candidate(photos, volume("", "c0ffee00-0000-4000-8000-0000000000e1", {device("01")}), "Photos"),
        candidate(whole, volume("", "c0ffee00-0000-4000-8000-0000000000e2", {device("01")}), ""),
        candidate(cloned, volume("", "c0ffee00-0000-4000-8000-0000000000e3", {device("02")}), "Clone"),
        candidate(absent, volume("", "c0ffee00-0000-4000-8000-0000000000e4", {device("04")}), "Away"),
        candidate(twin, twin_identity, "Twin"),
    };
}

// Attached: volumes A and B on device 01, C (the filesystem of `cloned`) on
// device 03, D (holding nothing) on device 06, the two that look like `twin`,
// a volume known by its GUID path alone, and a volume whose device the system
// does not name.
va::app::AttachedDevices attached_now()
{
    auto twin_one = device("07");
    twin_one.serial.clear();
    auto twin_two = device("08");
    twin_two.serial.clear();
    auto with_wwn = device("09");
    with_wwn.serial.clear();
    with_wwn.wwn = "SAMPLE-WWN-09";
    const auto guid = std::string("\\\\?\\Volume{c0ffee00-0000-4000-8000-0000000000f1}\\");
    std::vector<va::VolumeInfo> volumes{
        volume("/media/SAMPLE-B", "c0ffee00-0000-4000-8000-0000000000e2", {device("01")}, "SAMPLE-B"),
        volume("/media/SAMPLE-C", "c0ffee00-0000-4000-8000-0000000000e3", {device("03", "sample-sata")}, "SAMPLE-C"),
        volume("/media/SAMPLE-A", "c0ffee00-0000-4000-8000-0000000000e1", {device("01")}, "SAMPLE-A"),
        volume("/media/SAMPLE-D", "c0ffee00-0000-4000-8000-0000000000e6", {device("06")}, ""),
        volume("/media/SAMPLE-T1", "c0ffee00-0000-4000-8000-0000000000e5", {twin_one}, "SAMPLE-T"),
        volume("/media/SAMPLE-T2", "c0ffee00-0000-4000-8000-0000000000e5", {twin_two}, "SAMPLE-T"),
        volume(guid, "c0ffee00-0000-4000-8000-0000000000f1", {with_wwn}, "SAMPLE-G"),
        volume("/media/SAMPLE-N", "c0ffee00-0000-4000-8000-0000000000f2", {}, "SAMPLE-N"),
    };
    volumes[3].read_only = true;
    volumes[0].devices.front().health = va::DeviceHealth{va::HealthStatus::Warning, {"predicting-failure"}};
    volumes[2].devices.front().health = volumes[0].devices.front().health;
    va::app::AttachedDevices attached;
    attached.devices = va::app::devices_of(volumes);
    attached.volumes = std::move(volumes);
    return attached;
}

const std::vector<va::gui::ArchiveName> names{
    {photos, "ARCH-SAMPLE-1", "SAMPLE-A"},
    {whole, "ARCH-SAMPLE-2", "SAMPLE-B"},
    {cloned, "ARCH-SAMPLE-3", "SAMPLE-C"},
    {absent, "ARCH-SAMPLE-4", "SAMPLE-AWAY"},
    {twin, "ARCH-SAMPLE-5", "SAMPLE-T"},
};

va::gui::AttachedRead sample_read(bool repository = true, std::optional<std::string> unread = std::nullopt)
{
    const std::vector<std::optional<std::uint64_t>> used{300 * gib, std::nullopt, std::nullopt, 412 * gib};
    return va::gui::attached_read(attached_now(), used, candidates(), repository, std::move(unread));
}

// An archive's status, its current revision recorded on `identity`.
va::ArchiveStatus status_of(const va::Uuid& uuid, std::string label, const va::VolumeInfo& identity)
{
    va::ArchiveStatus status;
    status.archive.archive_uuid = uuid;
    status.archive.label = std::move(label);
    status.archive.current_revision = 1;
    status.archive.status = "REGISTERED";
    va::ManifestHeader header;
    header.archive_uuid = uuid;
    header.label = status.archive.label;
    header.identity = identity;
    status.header = header;
    return status;
}

void a_read_matches_the_archives_on_each_volume()
{
    const auto read = sample_read();
    CHECK(read.volumes.size() == 8 && read.on_volumes.size() == 8 && read.archive_volumes.size() == 8);
    CHECK(read.volumes[0].used_bytes == 300 * gib && !read.volumes[1].used_bytes && !read.volumes[7].used_bytes);
    CHECK(read.on_volumes[0].size() == 1 && read.on_volumes[0].front().archive_uuid == whole);
    CHECK(read.on_volumes[1].size() == 1 && read.on_volumes[1].front().match == MatchResult::Migrated);
    CHECK(read.on_volumes[2].size() == 1 && read.on_volumes[2].front().archive_uuid == photos);
    CHECK(read.on_volumes[3].empty());
    CHECK(read.archives.size() == 5);
    const auto* found = va::gui::attachment_of(read, photos);
    CHECK(found && found->result == MatchResult::Match);
    CHECK(va::gui::attachment_of(read, absent) && va::gui::attachment_of(read, absent)->result == MatchResult::Unknown);
    CHECK(va::gui::attachment_of(read, twin) && va::gui::attachment_of(read, twin)->result == MatchResult::Ambiguous);
    CHECK(!va::gui::attachment_of(read, unmatched));

    // The screenshot mode: Devices lists the sample volumes, which hold no
    // archive; where the archives are is told on the volumes of their folders
    // (`own`, here sample volumes too, standing for them).
    va::app::AttachedDevices listed;
    listed.volumes = {volume("/media/SAMPLE-VOLUME-1", "c0ffee00-0000-4000-8000-000000000501", {device("11")})};
    listed.devices = va::app::devices_of(listed.volumes);
    const auto own = attached_now().volumes;
    const auto sample = va::gui::attached_read(listed, {}, candidates(), true, std::nullopt, true, own);
    CHECK(sample.sample && sample.on_volumes.front().empty());
    CHECK(sample.archive_volumes.size() == own.size());
    const auto* here = va::gui::attachment_of(sample, photos);
    CHECK(here && here->result == MatchResult::Match &&
          native(here->places.front().root) == native(std::filesystem::path("/media/SAMPLE-A") / "Photos"));
    // Each of `own` that holds an archive (B, C, A and the two that look
    // like `twin`) is listed after the sample list as the sample identity
    // alone, with a sample health and no space in use, the archives on it.
    CHECK(sample.volumes.size() == 1 + 5 && sample.on_volumes.size() == 1 + 5 && sample.devices.size() == 1 + 5);
    if (sample.volumes.size() == 6 && sample.devices.size() == 6) {
        const auto& held = sample.volumes[1];
        CHECK(native(held.volume.mount_point) == "/media/SAMPLE-B" && held.volume.volume_label == "SAMPLE-VOLUME");
        CHECK(!held.used_bytes && held.volume.devices.size() == 1);
        CHECK(held.volume.devices.front().model == "SAMPLE-DEVICE" && held.volume.devices.front().serial == "SAMPLE0001");
        CHECK(sample.devices[1].volumes == std::vector<std::size_t>{1});
        CHECK(sample.devices[1].device.health && sample.devices[1].device.health->status == va::HealthStatus::Healthy);
        CHECK(sample.on_volumes[1].size() == 1 && sample.on_volumes[1].front().archive_uuid == whole &&
              sample.on_volumes[1].front().volume == 1);
        CHECK(sample.on_volumes[2].size() == 1 && sample.on_volumes[2].front().match == MatchResult::Migrated);
        // Its card shows nothing of `own` but the mount point and the roots.
        const auto cards = va::gui::device_cards(Text(), sample, names, IdentityShown::Sample);
        const auto card = std::ranges::find(cards, std::string("SAMPLE-DEVICE"), &va::gui::DeviceCard::title);
        CHECK(card != cards.end());
        const std::vector<va::gui::Field> sample_fields{{"Serial", "SAMPLE0001"}, {"WWN", "SAMPLE-WWN"}};
        if (card != cards.end()) {
            CHECK(card->subtitle == "sample · 1.0 TiB" && card->fields == sample_fields);
            CHECK(card->volumes.size() == 1 && card->volumes.front().title == "SAMPLE-VOLUME" &&
                  card->volumes.front().register_root.empty() && !card->volumes.front().archives.empty());
        }
    }
}

void archives_say_where_they_are_attached()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto read = sample_read();
    const auto photos_root = native(std::filesystem::path("/media/SAMPLE-A") / "Photos");

    // At its root: the row starts with it, the details have no notice, FULL
    // verify no tooltip, and Device says where.
    const auto photos_status = status_of(photos, "ARCH-SAMPLE-1", candidates()[0].identity);
    const auto here = va::gui::archive_attached(chinese, &read, photos_status, IdentityShown::Recorded);
    CHECK(here.line && here.line->state == Attached::Here && here.line->words == photos_root);
    CHECK(!here.notice && !here.verify_tooltip);
    CHECK((here.fields ==
           std::vector<va::gui::Field>{{"目前所在的磁碟區", "SAMPLE-A"}, {"目前的根目錄", photos_root, true}}));
    // The volume as the views show it: the sample identity in the screenshot mode.
    const auto sample = va::gui::archive_attached(english, &read, photos_status, IdentityShown::Sample);
    CHECK(!sample.fields.empty() && sample.fields.front().value == "SAMPLE-VOLUME");

    // Not attached: named by its recorded device, model and serial.
    const auto absent_status = status_of(absent, "ARCH-SAMPLE-4", candidates()[3].identity);
    const auto away = va::gui::archive_attached(chinese, &read, absent_status, IdentityShown::Recorded);
    CHECK(away.line && away.line->state == Attached::Nowhere && away.line->words == "未連接");
    CHECK(away.notice == "這個封存的裝置（SAMPLE-DEVICE-04，序號 SAMPLE-SERIAL-04）目前沒有連接。接上之後就能驗證。");
    CHECK(away.verify_tooltip && away.verify_tooltip->contains("沒有連接"));
    CHECK((away.fields == std::vector<va::gui::Field>{{"目前的根目錄", "未連接"}}));
    // Retired: not expected to be verified, so nothing asks for its device.
    auto retired_status = absent_status;
    retired_status.archive.status = "RETIRED";
    const auto retired = va::gui::archive_attached(chinese, &read, retired_status, IdentityShown::Recorded);
    CHECK(retired.line && retired.line->state == Attached::Nowhere && retired.line->words == "未連接");
    CHECK(!retired.notice && !retired.verify_tooltip && retired.fields == away.fields);
    const auto away_english = va::gui::archive_attached(english, &read, absent_status, IdentityShown::Sample);
    CHECK(away_english.notice == "The device of this archive (SAMPLE-DEVICE, serial SAMPLE0001) is not attached now. "
                                 "Once it is attached, the archive can be verified.");
    // A device without a serial is named by its WWN; without a device, by
    // its volume label.
    auto wwn_identity = candidates()[3].identity;
    wwn_identity.devices.front().serial.clear();
    wwn_identity.devices.front().wwn = "SAMPLE-WWN-04";
    CHECK(va::gui::archive_attached(english, &read, status_of(absent, "ARCH-SAMPLE-4", wwn_identity),
              IdentityShown::Recorded)
              .notice == "The device of this archive (SAMPLE-DEVICE-04, WWN SAMPLE-WWN-04) is not attached now. "
                         "Once it is attached, the archive can be verified.");
    auto no_device = candidates()[3].identity;
    no_device.devices.clear();
    CHECK(va::gui::archive_attached(chinese, &read, status_of(absent, "ARCH-SAMPLE-4", no_device),
              IdentityShown::Recorded)
              .notice == "這個封存的磁碟區（SAMPLE-VOLUME）目前沒有連接。接上之後就能驗證。");

    // MIGRATED: its filesystem on another device; the notice says what it
    // means, then, on a line of its own, how it is relocated in the window
    // (decision 0046): Relocate to this carrier... beside it, the root found
    // there chosen.
    const auto cloned_status = status_of(cloned, "ARCH-SAMPLE-3", candidates()[2].identity);
    const auto moved = va::gui::archive_attached(chinese, &read, cloned_status, IdentityShown::Recorded);
    const auto clone_root = native(std::filesystem::path("/media/SAMPLE-C") / "Clone");
    CHECK(moved.line && moved.line->state == Attached::Migrated && moved.line->words == "裝置與記錄中的不同" &&
          moved.line->tooltip == clone_root);
    CHECK(moved.notice &&
          moved.notice->starts_with("這個封存的檔案系統出現在 " + clone_root + "，但裝置與記錄中的不同（已遷移）"));
    CHECK(moved.notice && moved.notice->contains("「完整驗證」不會讀取那裡"));
    CHECK(moved.notice && moved.notice->ends_with("\n如果那是這個封存的裝置換了外接盒，或是您有意把封存移到那個裝置，"
                                                  "請按「搬移到這個載體…」把封存搬移過去；搬移前會先完整讀取它。"));
    CHECK(moved.notice && !moved.notice->contains("archive relocate"));
    CHECK(moved.relocate_root == clone_root);
    CHECK(moved.verify_tooltip && moved.verify_tooltip->contains("已遷移") &&
          moved.verify_tooltip->contains("「完整驗證」不會讀取那裡"));
    const auto moved_english = va::gui::archive_attached(english, &read, cloned_status, IdentityShown::Recorded);
    CHECK(moved_english.notice &&
          moved_english.notice->starts_with("The filesystem of this archive is attached at " + clone_root + ", ") &&
          moved_english.notice->contains("(MIGRATED)") &&
          moved_english.notice->ends_with("\nIf it is this archive's device in another enclosure, or you moved the "
                                          "archive to that device on purpose, relocate the archive to it with Relocate "
                                          "to this carrier..., which reads it in full first."));
    // Retired, it takes no new revisions: no relocation is offered, nor
    // named, and the notice says why.
    auto retired_clone = cloned_status;
    retired_clone.archive.status = "RETIRED";
    const auto retired_moved = va::gui::archive_attached(english, &read, retired_clone, IdentityShown::Recorded);
    CHECK(!retired_moved.relocate_root);
    CHECK(retired_moved.notice && !retired_moved.notice->contains("Relocate") &&
          retired_moved.notice->ends_with(
              "\nThis archive is retired and takes no new revisions, so it is not relocated."));
    const auto retired_chinese = va::gui::archive_attached(chinese, &read, retired_clone, IdentityShown::Recorded);
    CHECK(retired_chinese.notice && !retired_chinese.notice->contains("搬移到") &&
          retired_chinese.notice->ends_with("\n此封存已停用，不再接受新的版次，因此不會搬移。"));

    // AMBIGUOUS: more than one volume looks like it.
    const auto twin_status = status_of(twin, "ARCH-SAMPLE-5", candidates()[4].identity);
    const auto twins = va::gui::archive_attached(chinese, &read, twin_status, IdentityShown::Recorded);
    CHECK(twins.line && twins.line->state == Attached::Ambiguous && twins.line->words == "多個磁碟區都像這個封存");
    CHECK(twins.line && twins.line->tooltip == native(std::filesystem::path("/media/SAMPLE-T1") / "Twin") + "\n" +
                                                   native(std::filesystem::path("/media/SAMPLE-T2") / "Twin"));
    CHECK(twins.notice && twins.notice->contains("「驗證一個資料夾或磁碟區…」") &&
          twins.notice->ends_with("再從上面列出的根目錄中選擇要驗證的那一個。"));
    CHECK(twins.verify_tooltip && twins.verify_tooltip->contains("「驗證一個資料夾或磁碟區…」"));

    // No read at hand, or one that did not match the archive: nothing is said.
    CHECK(va::gui::archive_attached(chinese, nullptr, photos_status, IdentityShown::Recorded) ==
          va::gui::ArchiveAttached{});
    CHECK(va::gui::archive_attached(chinese, &read, status_of(unmatched, "ARCH-SAMPLE-6", candidates()[0].identity),
              IdentityShown::Recorded) == va::gui::ArchiveAttached{});
}

void health_is_a_pill_in_its_tone()
{
    const Text chinese(Language::TraditionalChinese);
    using va::DeviceHealth;
    using va::HealthStatus;
    using va::gui::health_pill;
    CHECK((health_pill(chinese, DeviceHealth{HealthStatus::Healthy, {}}) == va::gui::Pill{"健康", Tone::Success}));
    CHECK((health_pill(chinese, DeviceHealth{HealthStatus::Warning, {"predicting-failure"}}) ==
           va::gui::Pill{"警告（預測即將故障）", Tone::Warning}));
    CHECK((health_pill(chinese, DeviceHealth{HealthStatus::Unhealthy, {}}) == va::gui::Pill{"不健康", Tone::Error}));
    CHECK((health_pill(chinese, DeviceHealth{}) == va::gui::Pill{"無法讀取", Tone::Neutral}));
    CHECK((health_pill(chinese, std::nullopt) == va::gui::Pill{"無法讀取", Tone::Neutral}));
    // A condition that tells of a failure warns, whatever the status says.
    CHECK(health_pill(chinese, DeviceHealth{HealthStatus::Healthy, {"bad-sectors"}}).tone == Tone::Warning);
    CHECK(health_pill(chinese, DeviceHealth{HealthStatus::Healthy, {"in-service"}}).tone == Tone::Success);
    // In English, capitalised as the other pills are.
    const Text english;
    CHECK(health_pill(english, DeviceHealth{HealthStatus::Healthy, {}}).words == "Healthy");
    CHECK(health_pill(english, DeviceHealth{HealthStatus::Warning, {"predicting-failure"}}).words.starts_with("Warning ("));
    CHECK(health_pill(english, std::nullopt).words == "Cannot be read");
}

void buses_are_named_as_people_write_them()
{
    // The enumerators' names; any other (a sample value) as given.
    CHECK(va::gui::bus_words("usb") == "USB" && va::gui::bus_words("sata") == "SATA");
    CHECK(va::gui::bus_words("nvme") == "NVMe" && va::gui::bus_words("scsi") == "SCSI");
    CHECK(va::gui::bus_words("sample-usb") == "sample-usb" && va::gui::bus_words("").empty());
}

void device_cards_list_volumes_and_archives()
{
    const Text chinese(Language::TraditionalChinese);
    const Text english;
    const auto read = sample_read();
    const auto cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded);
    // By their first volume's mount point, the GUID path last, then the
    // volumes whose device is not known.
    CHECK(cards.size() == 7);
    if (cards.size() != 7) {
        return;
    }
    const auto& first = cards[0];
    CHECK(first.title == "SAMPLE-DEVICE-01" && first.subtitle == "sample-usb・1.0 TiB" && first.bus == "sample-usb");
    CHECK((first.health == va::gui::Pill{"警告（預測即將故障）", Tone::Warning}));
    CHECK((first.fields == std::vector<va::gui::Field>{{"序號", "SAMPLE-SERIAL-01"}}));
    // Its two volumes by mount point, each with the archive on it.
    CHECK(first.volumes.size() == 2);
    if (first.volumes.size() == 2) {
        CHECK(first.volumes[0].title == "SAMPLE-A" && first.volumes[0].subtitle == "/media/SAMPLE-A・1.0 TiB");
        CHECK(first.volumes[0].archives.size() == 1 && first.volumes[0].archives.front().title == "ARCH-SAMPLE-1" &&
              first.volumes[0].archives.front().root == native(std::filesystem::path("/media/SAMPLE-A") / "Photos"));
        CHECK(first.volumes[0].register_root.empty());
        CHECK(first.volumes[1].subtitle == "/media/SAMPLE-B・1.0 TiB・已使用 300.0 GiB");
        CHECK(first.volumes[1].archives.size() == 1 && first.volumes[1].archives.front().archive_uuid == whole);
    }
    // MIGRATED, said on its volume.
    CHECK(cards[1].title == "SAMPLE-DEVICE-03" && cards[1].subtitle == "sample-sata・1.0 TiB");
    CHECK(cards[1].volumes.size() == 1 && cards[1].volumes.front().archives.size() == 1);
    if (!cards[1].volumes.empty() && !cards[1].volumes.front().archives.empty()) {
        const auto& clone = cards[1].volumes.front().archives.front();
        CHECK(clone.migrated && clone.title == "檔案系統屬於 ARCH-SAMPLE-3，但裝置與記錄中的不同（已遷移）");
    }
    // Nothing on it: Register this volume, with its root.
    const auto& empty = cards[2].volumes.front();
    CHECK(empty.title == "沒有標籤的磁碟區" && empty.subtitle == "/media/SAMPLE-D・1.0 TiB・已使用 412.0 GiB・唯讀");
    CHECK(empty.archives.empty() && empty.register_root == "/media/SAMPLE-D");
    CHECK(cards[3].volumes.front().archives.size() == 1 && cards[4].volumes.front().archives.size() == 1);
    // No serial reported, a WWN instead; known by its GUID path alone.
    CHECK((cards[5].fields == std::vector<va::gui::Field>{{"序號", "裝置沒有回報"}, {"WWN", "SAMPLE-WWN-09"}}));
    CHECK(cards[5].volumes.front().register_root.starts_with("\\\\?\\Volume{"));
    // The volume whose device is not known, without a health.
    CHECK(cards[6].title == "裝置不明的磁碟區" && !cards[6].health && cards[6].fields.empty());
    CHECK(cards[6].volumes.size() == 1 && cards[6].volumes.front().title == "SAMPLE-N");

    // An archive the names do not hold, by the start of its uuid; in English.
    const auto unnamed = va::gui::device_cards(english, read, {}, IdentityShown::Recorded);
    CHECK(unnamed[1].volumes.front().archives.front().title ==
          "The filesystem belongs to c0ffee00, but the device differs from the one recorded (MIGRATED)");

    // While a registration works on the volume, or on a folder in it (a
    // sample root), its root is said in place of Register this volume; on
    // another volume it changes nothing.
    const std::vector<std::filesystem::path> registering{std::filesystem::path("/media/SAMPLE-D") / "Sample folder",
        std::filesystem::path("/media/SAMPLE-ELSEWHERE")};
    const auto working = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, registering);
    CHECK(working.size() == 7);
    if (working.size() == 7) {
        const auto& line = working[2].volumes.front();
        CHECK(line.register_root.empty() &&
              line.registering == "正在註冊 " + native(std::filesystem::path("/media/SAMPLE-D") / "Sample folder"));
        CHECK(!working[5].volumes.front().register_root.empty() && working[5].volumes.front().registering.empty());
        CHECK(va::gui::device_cards(english, read, names, IdentityShown::Recorded,
                  {std::filesystem::path("/media/SAMPLE-D")})[2]
                  .volumes.front()
                  .registering == "Being registered: /media/SAMPLE-D");
    }

    // No repository open, or its archives could not be matched: nothing is
    // offered to register, as nothing tells what lies on the volumes.
    for (const auto& other : {sample_read(false), sample_read(true, "SAMPLE: the index cannot be read")}) {
        for (const auto& card : va::gui::device_cards(chinese, other, names, IdentityShown::Recorded)) {
            for (const auto& line : card.volumes) {
                CHECK(line.register_root.empty());
            }
        }
    }
}

void the_head_counts_the_devices()
{
    const Text chinese(Language::TraditionalChinese);
    const Text english;
    const auto read = sample_read();
    CHECK(va::gui::devices_status(chinese, read) == "已連接 6 個裝置");
    va::gui::AttachedRead one;
    one.devices.push_back({});
    CHECK(va::gui::devices_status(english, one) == "1 device attached");
    CHECK(va::gui::devices_status(english, va::gui::AttachedRead{}) == "No device is attached.");
    va::gui::AttachedRead volumes_alone;
    volumes_alone.volumes.resize(2);
    CHECK(va::gui::devices_status(chinese, volumes_alone) == "已連接 2 個磁碟區");
    // While a read is in flight; after 15 s, that a device may not answer.
    CHECK(va::gui::reading_words(chinese, 2s) == "正在讀取已連接的裝置……");
    CHECK(va::gui::reading_words(chinese, 15s) == "仍在讀取：可能有裝置沒有回應");
    CHECK(va::gui::reading_words(english, 1min) == "Still reading: a device may not be responding");
}

void reads_are_spaced_after_a_slow_one()
{
    using va::gui::read_due;
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    // Quick reads: every tick.
    CHECK(read_due(30s, 500ms, start, start + 30s));
    CHECK(read_due(30s, 3s, start, start + 30s));
    // A read of 6 s: the next one 60 s after it began.
    CHECK(!read_due(30s, 6s, start, start + 30s));
    CHECK(read_due(30s, 6s, start, start + 60s));
    CHECK(!read_due(5s, 1s, start, start + 5s));
    CHECK(read_due(5s, 1s, start, start + 10s));
}

void the_sample_list_is_plainly_sample()
{
    // The screenshot mode's devices: SAMPLE names, a health each, two
    // volumes on one device; never a drive letter.
    const auto volumes = va::gui::sample_volumes();
    std::vector<va::VolumeInfo> listed;
    for (const auto& sample : volumes) {
        listed.push_back(sample.volume);
        CHECK(native(sample.volume.mount_point).starts_with("/media/SAMPLE-"));
        for (const auto& on : sample.volume.devices) {
            CHECK(on.device_ref.starts_with("SAMPLE-DEVICE-REF-") && on.serial.starts_with("SAMPLE-SERIAL-"));
            CHECK(on.health.has_value());
        }
    }
    const auto devices = va::app::devices_of(listed);
    CHECK(devices.size() == 3);
    CHECK(std::ranges::any_of(devices, [](const va::app::AttachedDevice& device) { return device.volumes.size() == 2; }));
}

} // namespace

void device_rows_tests()
{
    a_read_matches_the_archives_on_each_volume();
    archives_say_where_they_are_attached();
    health_is_a_pill_in_its_tone();
    buses_are_named_as_people_write_them();
    device_cards_list_volumes_and_archives();
    the_head_counts_the_devices();
    reads_are_spaced_after_a_slow_one();
    the_sample_list_is_plainly_sample();
}
