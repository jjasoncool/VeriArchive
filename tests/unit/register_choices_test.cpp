// What Register Archive offers and says (src/gui/register_choices.*, decision
// 0046): its rows of the attached volumes, which options apply to the source
// chosen, the registration they stand for and its summary. Every volume,
// device, archive and root below is sample data made up for the test
// (SAMPLE-* labels, models and serials, ARCH-SAMPLE-* labels, c0ffee00-...
// uuids, mount points under /media/SAMPLE-*); none is of a real volume,
// device or archive.
#include "test_support.h"

#include "common/utf8.h"
#include "gui/register_choices.h"
#include "gui/report.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using va::app::Language;
using va::app::Text;
using va::gui::ArchiveName;
using va::gui::IdentityShown;
using va::gui::OptionsChosen;
using va::gui::PendingRoot;

const va::Uuid photos = "c0ffee00-0000-4000-8000-000000000b01";
const va::Uuid whole = "c0ffee00-0000-4000-8000-000000000b02";
const va::Uuid cloned = "c0ffee00-0000-4000-8000-000000000b03";
const va::Uuid retired = "c0ffee00-0000-4000-8000-000000000b04";
const va::Uuid other = "c0ffee00-0000-4000-8000-000000000b05";
const va::Uuid pending_archive = "c0ffee00-0000-4000-8000-000000000b06";

constexpr std::uint64_t gib = std::uint64_t{1} << 30;

va::VolumeInfo volume(std::string mount_point, std::string filesystem_uuid, std::string serial,
    std::string label = "SAMPLE-VOLUME")
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = std::move(filesystem_uuid);
    info.volume_label = std::move(label);
    info.capacity_bytes = std::uint64_t{1} << 40;
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = std::move(serial);
    device.bus = "sample-usb";
    info.devices.push_back(device);
    return info;
}

va::ArchiveCandidate candidate(const va::Uuid& uuid, va::VolumeInfo identity, std::string root_subpath)
{
    va::ArchiveCandidate archive;
    archive.archive_uuid = uuid;
    archive.identity = std::move(identity);
    archive.identity.mount_point.clear();
    archive.root_subpath = std::move(root_subpath);
    return archive;
}

const std::vector<ArchiveName> names{
    {photos, "ARCH-SAMPLE-1", "SAMPLE-VOLUME"},
    {whole, "ARCH-SAMPLE-2", "SAMPLE-VOLUME"},
    {cloned, "ARCH-SAMPLE-3", "SAMPLE-VOLUME"},
    {retired, "ARCH-SAMPLE-4", "SAMPLE-VOLUME"},
    {other, "ARCH-SAMPLE-5", "SAMPLE-VOLUME"},
};

// The archives: Photos on volume A, the whole of volume B, a folder whose
// filesystem is on volume C on another device than recorded (a clone), a
// retired one at "Old" on A.
std::vector<va::ArchiveCandidate> candidates()
{
    auto old = candidate(retired, volume("", "1111000011110000", "SAMPLE-SERIAL-A"), "Old");
    old.retired = true;
    return {
        candidate(photos, volume("", "1111000011110000", "SAMPLE-SERIAL-A"), "Photos"),
        candidate(whole, volume("", "2222000022220000", "SAMPLE-SERIAL-B"), ""),
        candidate(cloned, volume("", "3333000033330000", "SAMPLE-SERIAL-X"), "Clone"),
        old,
    };
}

std::string native(const std::filesystem::path& path)
{
    return va::path_to_utf8(path);
}

void volume_rows_tell_what_each_holds()
{
    const Text english;
    const auto guid = std::string("\\\\?\\Volume{c0ffee00-0000-4000-8000-0000000000e1}\\");
    const std::vector<va::gui::AttachedVolume> volumes{
        {volume("/media/SAMPLE-A", "1111000011110000", "SAMPLE-SERIAL-A", "SAMPLE-A"), 300 * gib},
        {volume("/media/SAMPLE-B", "2222000022220000", "SAMPLE-SERIAL-B", "SAMPLE-B"), std::nullopt},
        {volume("/media/SAMPLE-C", "3333000033330000", "SAMPLE-SERIAL-Y", "SAMPLE-C"), std::nullopt},
        {volume(guid, "4444000044440000", "SAMPLE-SERIAL-E", ""), std::nullopt},
        {volume("/media/SAMPLE-D", "5555000055550000", "SAMPLE-SERIAL-D", "SAMPLE-D"), 412 * gib},
        {volume("/media/SAMPLE-F", "6666000066660000", "SAMPLE-SERIAL-F", "SAMPLE-F"), std::nullopt},
        // A sector-by-sector clone of volume B, on another device.
        {volume("/media/SAMPLE-G", "2222000022220000", "SAMPLE-SERIAL-G", "SAMPLE-B"), std::nullopt},
    };
    const std::vector<PendingRoot> pending{
        {pending_archive, "ARCH-SAMPLE-6", native(std::filesystem::path("/media/SAMPLE-F") / "Data")}};
    // And an archive in a folder of the volume known by its GUID path alone.
    auto archives = candidates();
    archives.push_back(candidate(other, volume("", "4444000044440000", "SAMPLE-SERIAL-E", ""), "Data"));
    const auto rows = va::gui::volume_rows(english, volumes, archives, pending, names, IdentityShown::Recorded);
    CHECK(rows.size() == 7);
    if (rows.size() != 7) {
        return;
    }
    // By mount point, the volume known by its GUID path alone last.
    CHECK(rows[0].volume == 0 && rows[1].volume == 1 && rows[2].volume == 2 && rows[3].volume == 4 &&
          rows[4].volume == 5 && rows[5].volume == 6 && rows[6].volume == 3);

    // A folder of an archive, and a retired one, on it: a folder in it may be chosen.
    const auto photos_root = native(std::filesystem::path("/media/SAMPLE-A") / "Photos");
    const auto old_root = native(std::filesystem::path("/media/SAMPLE-A") / "Old");
    CHECK(rows[0].title == "SAMPLE-A" && rows[0].where == native(std::filesystem::path("/media/SAMPLE-A")));
    CHECK(rows[0].root == rows[0].where);
    CHECK(rows[0].details == "SAMPLE-DEVICE · sample-usb · 1.0 TiB · " + va::gui::format_bytes(300 * gib) + " used");
    CHECK(!rows[0].selectable);
    CHECK(rows[0].note == "Holds ARCH-SAMPLE-1 (" + photos_root + "), ARCH-SAMPLE-4 (" + old_root +
                              "); the whole volume cannot be registered: choose a folder in it that does not overlap "
                              "them");
    CHECK(rows[0].folder_button == "Choose a folder in " + rows[0].where + "...");
    CHECK(rows[0].folder_tooltip == rows[0].folder_button);

    // An archive at the volume's root alone: the volume is chosen, and
    // Options offers what applies there (a new revision, another archive);
    // no folder in it, which would lie in that root.
    CHECK(rows[1].selectable && rows[1].folder_button.empty());
    CHECK(rows[1].note == "Holds ARCH-SAMPLE-2 (" + native(std::filesystem::path("/media/SAMPLE-B")) + ")");

    // The filesystem of an archive on another device, in a folder: said so,
    // in the words of MIGRATED.
    const auto clone_root = native(std::filesystem::path("/media/SAMPLE-C") / "Clone");
    CHECK(!rows[2].selectable && !rows[2].folder_button.empty());
    CHECK(rows[2].note.starts_with("Holds the filesystem of ARCH-SAMPLE-3, behind another enclosure or on another "
                                   "device than recorded (" +
                                   clone_root + ")"));

    // Nothing on it: chosen as a whole.
    CHECK(rows[3].selectable && rows[3].note.empty() && rows[3].folder_button.empty());
    CHECK(rows[3].details.ends_with(va::gui::format_bytes(412 * gib) + " used"));

    // An unfinished registration's root on it.
    CHECK(!rows[4].selectable && !rows[4].folder_button.empty());
    CHECK(rows[4].note.starts_with("Holds an unfinished registration of ARCH-SAMPLE-6 (" + pending.front().root + ")"));

    // A clone of a whole archived volume: chosen, to register as another archive.
    CHECK(rows[5].selectable && rows[5].folder_button.empty());
    CHECK(rows[5].note.starts_with("Holds the filesystem of ARCH-SAMPLE-2, behind another enclosure"));

    // No label: said so. Known by its GUID path alone, its button is short
    // and its tooltip names it.
    CHECK(rows[6].title == "Volume without a label" && !rows[6].selectable);
    CHECK(rows[6].folder_button == "Choose a folder in it..." &&
          rows[6].folder_tooltip == "Choose a folder in " + guid + "...");

    // In Chinese, with the catalog's words.
    const Text chinese(Language::TraditionalChinese);
    const auto chinese_rows =
        va::gui::volume_rows(chinese, volumes, archives, pending, names, IdentityShown::Recorded);
    CHECK(chinese_rows[0].note == "已有 ARCH-SAMPLE-1（" + photos_root + "）、ARCH-SAMPLE-4（" + old_root +
                                      "）；整個磁碟區不能註冊，可以選其中不重疊的資料夾");
    CHECK(chinese_rows[0].folder_button == "選擇 " + rows[0].where + " 裡的資料夾…");
    CHECK(chinese_rows[2].note.starts_with("已有 ARCH-SAMPLE-3 的檔案系統，但外接盒或裝置與記錄中的不同（" + clone_root +
                                           "）"));
    CHECK(chinese_rows[6].title == "沒有標籤的磁碟區" && chinese_rows[6].folder_button == "選擇其中的資料夾…");

    // The sample identity names every device SAMPLE-DEVICE, whatever was read.
    auto read = volumes;
    read[0].volume.devices.front().model = "RECORDED-MODEL";
    const auto sample_rows = va::gui::volume_rows(english, read, archives, pending, names, IdentityShown::Sample);
    CHECK(sample_rows[0].details.starts_with("SAMPLE-DEVICE · sample · "));
}

// The same rows in the relocation dialog (decision 0046), for the archive
// relocated: a volume is chosen whole only where nothing else lies on it;
// another archive at the volume's root leaves nothing of it to choose; the
// archive's own filesystem on another device than recorded (its device in
// another enclosure) never stands in the way, and in a folder it is offered
// as the new root.
void volume_rows_of_a_relocation()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    using va::gui::VolumeUse;
    const std::vector<va::gui::AttachedVolume> volumes{
        // ARCH-SAMPLE-1 ("Photos") and the retired ARCH-SAMPLE-4 ("Old") in folders.
        {volume("/media/SAMPLE-A", "1111000011110000", "SAMPLE-SERIAL-A", "SAMPLE-A"), std::nullopt},
        // ARCH-SAMPLE-2 at the volume's root.
        {volume("/media/SAMPLE-B", "2222000022220000", "SAMPLE-SERIAL-B", "SAMPLE-B"), std::nullopt},
        // The filesystem of ARCH-SAMPLE-3 ("Clone") on another device: the
        // archive relocated, its device in another enclosure.
        {volume("/media/SAMPLE-C", "3333000033330000", "SAMPLE-SERIAL-Y", "SAMPLE-C"), std::nullopt},
        // Nothing on it.
        {volume("/media/SAMPLE-D", "5555000055550000", "SAMPLE-SERIAL-D", "SAMPLE-D"), std::nullopt},
        // A copy of the whole of volume B, sector by sector, on another device.
        {volume("/media/SAMPLE-G", "2222000022220000", "SAMPLE-SERIAL-G", "SAMPLE-B"), std::nullopt},
    };
    const auto rows = va::gui::volume_rows(english, volumes, candidates(), {}, names, IdentityShown::Recorded,
        VolumeUse::Relocation, cloned);
    CHECK(rows.size() == 5);
    if (rows.size() != 5) {
        return;
    }
    // Folders of other archives: a folder that overlaps none of them.
    CHECK(!rows[0].selectable && !rows[0].folder_button.empty() && rows[0].own_root.empty());
    CHECK(rows[0].note.ends_with("; the whole volume cannot be the new root: choose a folder in it that does not "
                                 "overlap them"));
    // Another archive at the volume's root: nothing on it can be the new root.
    CHECK(!rows[1].selectable && rows[1].folder_button.empty());
    CHECK(rows[1].note == "Holds ARCH-SAMPLE-2 (" + native(std::filesystem::path("/media/SAMPLE-B")) +
                              "); nothing on it can be the new root, which would overlap them");
    // The archive relocated, its filesystem in a folder on another device:
    // offered as the new root, and named as its own, not as another's.
    const auto clone_root = native(std::filesystem::path("/media/SAMPLE-C") / "Clone");
    CHECK(!rows[2].selectable && rows[2].own_root == clone_root);
    CHECK(rows[2].note == "The filesystem of this archive is here, on another device than recorded (" + clone_root +
                              ")");
    CHECK(rows[2].own_button == "Use as the new root" && rows[2].own_tooltip == "Use " + clone_root + " as the new root");
    // Nothing on it: chosen whole.
    CHECK(rows[3].selectable && rows[3].note.empty() && rows[3].folder_button.empty() && rows[3].own_button.empty());
    // A copy of another archive's whole volume: nothing on it either.
    CHECK(!rows[4].selectable && rows[4].note.starts_with("Holds the filesystem of ARCH-SAMPLE-2, behind another "
                                                          "enclosure") &&
          rows[4].note.ends_with("; nothing on it can be the new root, which would overlap them"));

    // The relocation of ARCH-SAMPLE-2, whose filesystem is the whole of
    // volume G on another device: G chosen whole, B (its carrier as
    // recorded) too, nothing else lying on them.
    const auto own = va::gui::volume_rows(english, volumes, candidates(), {}, names, IdentityShown::Recorded,
        VolumeUse::Relocation, whole);
    CHECK(own.size() == 5 && own[1].selectable && own[4].selectable && own[4].own_root.empty());
    CHECK(own.size() == 5 && own[1].note == "This archive is here, as recorded (" +
                                                native(std::filesystem::path("/media/SAMPLE-B")) + ")");
    CHECK(own.size() == 5 && own[4].note.starts_with("The filesystem of this archive is here, on another device"));

    // In Chinese, with the catalog's words.
    const auto chinese_rows = va::gui::volume_rows(chinese, volumes, candidates(), {}, names, IdentityShown::Recorded,
        VolumeUse::Relocation, cloned);
    CHECK(chinese_rows.size() == 5 && chinese_rows[1].note.ends_with("；其中沒有任何位置能作為新的根目錄，否則會與它們重疊"));
    CHECK(chinese_rows.size() == 5 && chinese_rows[2].note == "此封存的檔案系統在這裡，但裝置與記錄中的不同（" +
                                                                    clone_root + "）" &&
          chinese_rows[2].own_button == "設為新的根目錄");
}

va::gui::ChosenSource source_at(const std::string& mount_point, const std::string& subpath, const va::VolumeInfo& on)
{
    va::gui::ChosenSource source;
    source.volume = on;
    source.volume.mount_point = std::filesystem::path(mount_point);
    source.root_subpath = subpath;
    source.root = subpath.empty() ? std::filesystem::path(mount_point) : std::filesystem::path(mount_point) / subpath;
    return source;
}

void options_apply_as_a_registration_judges_the_root()
{
    const Text english;
    const auto on_a = volume("/media/SAMPLE-A", "1111000011110000", "SAMPLE-SERIAL-A");
    // An archive's very root: a new revision of it, or another archive.
    auto options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Photos", on_a), candidates(), {},
        names);
    CHECK(options.registered_here == std::vector<va::Uuid>{photos} && options.new_revision);
    CHECK(options.clones_here.empty() && !options.unfinished && !options.refusal);
    // Matched as roots are compared: in any case.
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "photos", on_a), candidates(), {}, names);
    CHECK(options.registered_here == std::vector<va::Uuid>{photos});

    // The volume, or a folder in an archive's root: they would overlap.
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "", on_a), candidates(), {}, names);
    CHECK(options.refusal.value_or("").starts_with("This folder or volume overlaps the root of ARCH-SAMPLE-1 ("));
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Photos/2024", on_a), candidates(), {},
        names);
    CHECK(options.refusal.has_value() && options.registered_here.empty());
    // Beside them: a new archive.
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Music", on_a), candidates(), {}, names);
    CHECK(!options.refusal && options.registered_here.empty() && options.clones_here.empty() && !options.new_revision);

    // A retired archive takes no new revision (decision 0003): only another
    // archive, and Options says why.
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Old", on_a), candidates(), {}, names);
    CHECK(options.registered_here == std::vector<va::Uuid>{retired} && !options.new_revision && options.retired);
    CHECK(va::gui::options_shown(english, options, {}, names).blocked ==
          "ARCH-SAMPLE-4 is retired and takes no new revision: register another archive, or choose another folder or "
          "volume.");

    // Two archives at one root (AMBIGUOUS, twins registered on purpose): neither
    // takes a new revision.
    auto twins = candidates();
    twins.push_back(candidate(other, volume("", "1111000011110000", "SAMPLE-SERIAL-A"), "Photos"));
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Photos", on_a), twins, {}, names);
    CHECK(options.registered_here == (std::vector<va::Uuid>{photos, other}) && !options.new_revision &&
          !options.retired);
    CHECK(va::gui::options_shown(english, options, {}, names).blocked ==
          "Several archives are registered here (ARCH-SAMPLE-1, ARCH-SAMPLE-5), so none takes a new revision: "
          "register another archive, or choose another folder or volume.");
    CHECK(va::gui::options_shown(Text(Language::TraditionalChinese), options, {}, names).blocked ==
          "此處註冊了多個封存（ARCH-SAMPLE-1、ARCH-SAMPLE-5），所以都不能建立新版次：請註冊為另一個封存，或選擇其他資料夾"
          "或磁碟區。");

    // An earlier carrier of an archive (PREVIOUS_CARRIER, decision 0004), as
    // matching found it: at its root, only another archive; a root that
    // overlaps it is refused, as the registration refuses it.
    const std::vector<va::gui::EarlierCarrier> earlier{{other, "Before"}};
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "before", on_a), candidates(), {}, names,
        earlier);
    CHECK(options.earlier_carrier_of == std::vector<va::Uuid>{other} && options.registered_here.empty() &&
          !options.refusal && !options.new_revision);
    options = va::gui::source_options(english, source_at("/media/SAMPLE-A", "Before/2024", on_a), candidates(), {},
        names, earlier);
    CHECK(options.refusal.value_or("").starts_with("This folder or volume overlaps the root of ARCH-SAMPLE-5 (" +
                                                   native(std::filesystem::path("/media/SAMPLE-A") / "Before") + ")"));

    // The filesystem of an archive on another device (MIGRATED).
    const auto on_c = volume("/media/SAMPLE-C", "3333000033330000", "SAMPLE-SERIAL-Y");
    options = va::gui::source_options(english, source_at("/media/SAMPLE-C", "Clone", on_c), candidates(), {}, names);
    CHECK(options.clones_here == std::vector<va::Uuid>{cloned} && options.registered_here.empty());
    CHECK(va::gui::default_copy_of(options) == cloned);

    // An unfinished registration of the very root, however its plan spells it.
    const auto on_f = volume("/media/SAMPLE-F", "6666000066660000", "SAMPLE-SERIAL-F");
    const std::vector<PendingRoot> pending{
        {pending_archive, "ARCH-SAMPLE-6", native(std::filesystem::path("/media/SAMPLE-F") / "DATA") + "/"}};
    options = va::gui::source_options(english, source_at("/media/SAMPLE-F", "Data", on_f), candidates(), pending, names);
    CHECK(options.unfinished && options.unfinished->archive_uuid == pending_archive);
    options = va::gui::source_options(english, source_at("/media/SAMPLE-F", "Other", on_f), candidates(), pending, names);
    CHECK(!options.unfinished);
}

void options_shown_and_the_request_they_make()
{
    const Text english;
    const auto on_a = volume("/media/SAMPLE-A", "1111000011110000", "SAMPLE-SERIAL-A");
    const auto here = source_at("/media/SAMPLE-A", "Photos", on_a);
    const auto options = va::gui::source_options(english, here, candidates(), {}, names);

    // Neither chosen yet: Next waits.
    OptionsChosen chosen;
    auto shown = va::gui::options_shown(english, options, chosen, names);
    CHECK(shown.new_revision && shown.another_archive && !shown.resume && shown.label && shown.copy_of);
    CHECK(shown.blocked == "Choose whether to create a new revision of the archive registered here or to register "
                           "another archive.");
    CHECK(shown.another_archive_note == "This folder or volume is registered as ARCH-SAMPLE-1. Another archive gets "
                                        "revisions and a history of its own.");

    // A new revision keeps the archive's label and copy.
    chosen.new_revision = true;
    chosen.label = "ARCH-0099";
    chosen.copy_of = other;
    chosen.media_copy = true;
    shown = va::gui::options_shown(english, options, chosen, names);
    CHECK(!shown.blocked && !shown.label && !shown.copy_of);
    auto request = va::gui::registration_request(here, options, chosen, {{"SAMPLE-SERIAL-A", true}});
    CHECK(request.root == here.root && request.new_revision && !request.force_new_archive && !request.resume);
    CHECK(!request.label && !request.copy_of && request.write_media_copy);
    CHECK(request.rotational_override.at("SAMPLE-SERIAL-A"));
    auto summary = va::gui::registration_summary(english, here, options, chosen, names, IdentityShown::Recorded);
    CHECK(summary.size() == 6);
    if (summary.size() == 6) {
        CHECK(summary[0].name == "Source" && summary[0].value == native(here.root));
        CHECK(summary[1].value == "SAMPLE-VOLUME · SAMPLE-DEVICE");
        CHECK(summary[2].value == "ARCH-SAMPLE-1, the archive's own");
        CHECK(summary[3].value == "every file in this folder; their number and size are known after the scan");
        CHECK(summary[4].name == "Registration" && summary[4].value == "a new revision of ARCH-SAMPLE-1");
        CHECK(summary[5].value == "written to the .veriarchive folder in the root");
    }

    // Another archive at the same root, with its own label and copy.
    chosen.new_revision = false;
    chosen.another_archive = true;
    chosen.media_copy = false;
    shown = va::gui::options_shown(english, options, chosen, names);
    CHECK(!shown.blocked && shown.label && shown.copy_of);
    request = va::gui::registration_request(here, options, chosen, {});
    CHECK(request.force_new_archive && !request.new_revision && request.label == "ARCH-0099" && request.copy_of == other);
    summary = va::gui::registration_summary(english, here, options, chosen, names, IdentityShown::Recorded);
    CHECK(summary.size() == 7 && summary[4].value == "another archive, besides ARCH-SAMPLE-1" &&
          summary[6].name == "Copy of" && summary[6].value == "ARCH-SAMPLE-5");
    CHECK(summary[5].value == "not written: nothing is written to the archive");
    // A label not of the form ARCH-NNNN is refused before it starts.
    chosen.label = "photos 2024";
    CHECK(va::gui::options_shown(english, options, chosen, names).blocked ==
          "A label has the form ARCH-NNNN, such as ARCH-0012.");

    // A new folder: the next label, a new archive, no copy.
    auto whole_volume = source_at("/media/SAMPLE-D", "", volume("/media/SAMPLE-D", "5555000055550000", "SAMPLE-SERIAL-D"));
    whole_volume.used_bytes = 412 * gib;
    auto none = va::gui::source_options(english, whole_volume, candidates(), {}, names);
    OptionsChosen plain;
    shown = va::gui::options_shown(english, none, plain, names);
    CHECK(!shown.blocked && shown.label && !shown.new_revision && !shown.another_archive && !shown.resume);
    CHECK(shown.label_note == "Leave it empty to take the repository's next label (ARCH-NNNN).");
    request = va::gui::registration_request(whole_volume, none, plain, {});
    CHECK(!request.label && !request.copy_of && !request.new_revision && !request.force_new_archive &&
          !request.write_media_copy);
    summary = va::gui::registration_summary(english, whole_volume, none, plain, names, IdentityShown::Recorded);
    CHECK(summary.size() == 7 && summary[2].value == "the next label of the repository" &&
          summary[3].value == "every file of the volume, about " + va::gui::format_bytes(412 * gib) +
                                  " in use; their number is known after the scan" &&
          summary[4].value == "a new archive" && summary[6].value == "not a copy");
    // The repository's next label, where it was read, named on both pages;
    // still not typed (the registration takes it).
    none.next_label = "ARCH-0004";
    CHECK(va::gui::options_shown(english, none, plain, names).label_note ==
          "Leave it empty to take ARCH-0004, the repository's next label.");
    CHECK(va::gui::options_shown(Text(Language::TraditionalChinese), none, plain, names).label_note ==
          "留空時使用儲存庫的下一個標籤 ARCH-0004。");
    CHECK(!va::gui::registration_request(whole_volume, none, plain, {}).label);
    summary = va::gui::registration_summary(english, whole_volume, none, plain, names, IdentityShown::Recorded);
    CHECK(summary[2].value == "ARCH-0004, the repository's next label");
    // The sample identity in the screenshot mode.
    summary = va::gui::registration_summary(english, whole_volume, none, plain, names, IdentityShown::Sample);
    CHECK(summary[1].value == "SAMPLE-VOLUME · SAMPLE-DEVICE");
    CHECK(va::gui::source_volume_words(english, whole_volume, IdentityShown::Recorded) == "SAMPLE-VOLUME · SAMPLE-DEVICE");

    // The filesystem of an archive on another device: only another archive.
    const auto clone = source_at("/media/SAMPLE-C", "Clone", volume("/media/SAMPLE-C", "3333000033330000", "SAMPLE-SERIAL-Y"));
    const auto clone_options = va::gui::source_options(english, clone, candidates(), {}, names);
    shown = va::gui::options_shown(english, clone_options, plain, names);
    CHECK(!shown.new_revision && shown.another_archive);
    CHECK(shown.blocked == "The filesystem of this folder or volume is a registered archive's, but the enclosure or "
                           "device differs from the one recorded: choose to register another archive, or choose "
                           "another folder or volume.");
    plain.another_archive = true;
    CHECK(!va::gui::options_shown(english, clone_options, plain, names).blocked);
    CHECK(va::gui::registration_request(clone, clone_options, plain, {}).force_new_archive);
    // Copy of lists the clone's original first.
    const auto choices = va::gui::copy_of_choices(names, clone_options);
    CHECK(choices.size() == names.size() && choices.front().archive_uuid == cloned &&
          choices[1].archive_uuid == photos);

    // An archive's root that also holds another archive's filesystem on
    // another device (MIGRATED: the archive was registered from a clone of
    // that one): a new revision of it is `--new-revision --force-new-archive`,
    // as the engine refuses it otherwise.
    auto with_clone = candidates();
    with_clone.push_back(candidate(other, volume("", "1111000011110000", "SAMPLE-SERIAL-Z"), "Photos"));
    const auto both = va::gui::source_options(english, here, with_clone, {}, names);
    CHECK(both.registered_here == std::vector<va::Uuid>{photos} && both.clones_here == std::vector<va::Uuid>{other} &&
          both.new_revision);
    OptionsChosen revision;
    revision.new_revision = true;
    CHECK(!va::gui::options_shown(english, both, revision, names).blocked);
    request = va::gui::registration_request(here, both, revision, {});
    CHECK(request.new_revision && request.force_new_archive);
    // A new revision where nothing else is at the root takes no --force-new-archive.
    CHECK(!va::gui::registration_request(here, options, revision, {}).force_new_archive);

    // An earlier carrier of an archive at its root (decision 0004), such as
    // the volume a relocation left: only another archive, as with
    // --force-new-archive.
    const auto old_carrier = source_at("/media/SAMPLE-D", "Before", volume("/media/SAMPLE-D", "5555000055550000", "SAMPLE-SERIAL-D"));
    const auto carrier_options = va::gui::source_options(english, old_carrier, candidates(), {}, names, {{other, "Before"}});
    OptionsChosen apart;
    shown = va::gui::options_shown(english, carrier_options, apart, names);
    CHECK(shown.another_archive && !shown.new_revision && shown.label);
    CHECK(shown.another_archive_note.starts_with("This folder or volume is an earlier carrier of ARCH-SAMPLE-5: "));
    CHECK(shown.blocked == "This folder or volume is an earlier carrier of a registered archive: choose to register "
                           "another archive, or choose another folder or volume.");
    CHECK(!va::gui::registration_request(old_carrier, carrier_options, apart, {}).force_new_archive);
    apart.another_archive = true;
    CHECK(!va::gui::options_shown(english, carrier_options, apart, names).blocked);
    request = va::gui::registration_request(old_carrier, carrier_options, apart, {});
    CHECK(request.force_new_archive && !request.new_revision);
    summary = va::gui::registration_summary(english, old_carrier, carrier_options, apart, names, IdentityShown::Recorded);
    CHECK(summary[4].value == "another archive, besides ARCH-SAMPLE-5");
    CHECK(va::gui::copy_of_choices(names, carrier_options).front().archive_uuid == other);
}

// Create a new revision... of an archive: where the attached volumes hold it
// now (a drive letter may have changed), and a refusal where another archive
// is at the root it recorded. Two sample archives, one root path.
void a_new_revision_is_of_the_archive_asked_for()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto recorded = native(std::filesystem::path("/media/SAMPLE-A") / "Photos");
    // Photos' volume is now at /media/SAMPLE-K; another volume, whose
    // folder Photos is archive ARCH-SAMPLE-5, at /media/SAMPLE-A.
    auto archives = candidates();
    archives.push_back(candidate(other, volume("", "7777000077770000", "SAMPLE-SERIAL-H"), "Photos"));
    const std::vector<va::gui::AttachedVolume> moved{
        {volume("/media/SAMPLE-A", "7777000077770000", "SAMPLE-SERIAL-H"), std::nullopt},
        {volume("/media/SAMPLE-K", "1111000011110000", "SAMPLE-SERIAL-A"), std::nullopt},
    };
    CHECK(va::gui::new_revision_source(moved, archives, photos, recorded) ==
          native(std::filesystem::path("/media/SAMPLE-K") / "Photos"));
    CHECK(va::gui::new_revision_source(moved, archives, other, recorded) == recorded);
    // Not attached: the root it recorded, which Next then reads.
    CHECK(va::gui::new_revision_source({moved[0]}, archives, photos, recorded) == recorded);

    // That root read: another archive is there, not the one asked for.
    const auto there = source_at("/media/SAMPLE-A", "Photos", moved[0].volume);
    const auto options = va::gui::source_options(english, there, archives, {}, names);
    CHECK(options.registered_here == std::vector<va::Uuid>{other});
    CHECK(va::gui::new_revision_refusal(english, options, photos, names, recorded) ==
          "ARCH-SAMPLE-1 is not at " + recorded +
              " now: its volume may be attached at another drive letter or mount point, or not attached. Choose the "
              "folder or volume where it is now.");
    CHECK(va::gui::new_revision_refusal(chinese, options, photos, names, recorded) ==
          "ARCH-SAMPLE-1 目前不在 " + recorded + "：它的磁碟區可能換了磁碟機代號或掛載點，或沒有連接。請選擇它現在所在的資料夾或磁碟區。");
    CHECK(!va::gui::new_revision_refusal(english, options, other, names, recorded));
}

void an_unfinished_registration_is_only_resumed()
{
    const Text english;
    const auto on_f = volume("/media/SAMPLE-F", "6666000066660000", "SAMPLE-SERIAL-F");
    const auto source = source_at("/media/SAMPLE-F", "Data", on_f);
    const std::vector<PendingRoot> pending{{pending_archive, "ARCH-SAMPLE-6", native(source.root)}};
    const auto options = va::gui::source_options(english, source, candidates(), pending, names);
    OptionsChosen chosen;
    chosen.label = "ARCH-0100";
    chosen.copy_of = other;
    chosen.media_copy = true;
    auto shown = va::gui::options_shown(english, options, chosen, names);
    CHECK(shown.resume && !shown.label && !shown.new_revision && !shown.another_archive && !shown.copy_of);
    CHECK(!shown.blocked);
    CHECK(shown.resume_note.starts_with("An interrupted registration of ARCH-SAMPLE-6 exists at this folder"));
    // As `register <root> --resume --write-media-copy`: the plan fixed the rest.
    const auto request = va::gui::registration_request(source, options, chosen, {});
    CHECK(request.resume && request.write_media_copy && !request.label && !request.copy_of && !request.new_revision);
    const auto summary = va::gui::registration_summary(english, source, options, chosen, names, IdentityShown::Recorded);
    CHECK(summary.size() == 6 && summary[2].value == "ARCH-SAMPLE-6, as the interrupted registration planned it" &&
          summary[4].value == "the rest of the interrupted registration of ARCH-SAMPLE-6");
    // It reads only what the interrupted registration has not hashed.
    if (summary.size() == 6) {
        CHECK(summary[3].name == "To read" &&
              summary[3].value == "the files that the interrupted registration has not hashed yet; those it hashed "
                                  "are not read again");
    }
    // Whatever else was chosen, it is resumed: nothing else can start there.
    chosen.new_revision = true;
    chosen.another_archive = true;
    CHECK(va::gui::registration_request(source, options, chosen, {}).resume);
    CHECK(!va::gui::options_shown(english, options, chosen, names).blocked);

    const Text chinese(Language::TraditionalChinese);
    const auto chinese_summary =
        va::gui::registration_summary(chinese, source, options, chosen, names, IdentityShown::Recorded);
    CHECK(chinese_summary.size() == 6 && chinese_summary[3].value == "中斷的註冊尚未計算雜湊的檔案；已計算雜湊的不必重讀");
}

// The report of a registration in its job's details (a sample outcome).
void the_registration_report_says_what_it_recorded()
{
    va::RegisterOutcome outcome;
    outcome.archive_uuid = photos;
    outcome.label = "ARCH-SAMPLE-1";
    outcome.revision = 2;
    outcome.counts.files = 3;
    outcome.counts.dirs = 1;
    outcome.counts.links = 0;
    outcome.counts.bytes = 2048;
    outcome.manifest = std::filesystem::path("/sample/repository/manifests") / "SAMPLE.manifest.jsonl";
    va::Entry unreadable;
    unreadable.path.p = "Photos/SAMPLE-locked.jpg";
    unreadable.unreadable = "permission";
    outcome.unreadable.push_back(unreadable);
    outcome.warnings.push_back("a sample warning");
    outcome.media_copy = std::filesystem::path("/media/SAMPLE-A") / "Photos" / ".veriarchive" / "SAMPLE.manifest.jsonl";

    const Text english;
    const auto report = va::gui::registration_report(english, outcome);
    CHECK(report.starts_with("Registered ARCH-SAMPLE-1 (" + photos + ") revision 2\n\n"));
    CHECK(report.contains("\n3 files, 1 empty directory, 0 links, " + va::gui::format_bytes(2048) + " hashed\n"));
    CHECK(report.contains("Manifest: " + native(outcome.manifest) + "\n"));
    CHECK(report.contains("\nUnreadable at registration (recorded without hashes, never reported as verified):\n"
                          "  permission  Photos/SAMPLE-locked.jpg\n"));
    CHECK(report.contains("Warning: a sample warning\n"));
    CHECK(report.ends_with("\nMedia copy: " + native(outcome.media_copy) + "\n"));

    const Text chinese(Language::TraditionalChinese);
    const auto chinese_report = va::gui::registration_report(chinese, outcome);
    CHECK(chinese_report.starts_with("已註冊 ARCH-SAMPLE-1（" + photos + "）版次 2\n\n"));
    CHECK(chinese_report.contains("清單檔：" + native(outcome.manifest) + "\n"));
    CHECK(chinese_report.contains("  權限不足  Photos/SAMPLE-locked.jpg\n"));
    CHECK(chinese_report.contains("警告：a sample warning\n"));
    CHECK(chinese_report.ends_with("\n媒體副本：" + native(outcome.media_copy) + "\n"));

    // Without a media copy, the baseline is only in the repository.
    outcome.media_copy.clear();
    CHECK(va::gui::registration_report(english, outcome)
              .ends_with("\nThe baseline exists only in the repository: back up RepositoryRoot.\n"));
}

void toasts_say_what_was_registered()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::registered_toast(english, "ARCH-0004", 1) == "Registered as ARCH-0004");
    CHECK(va::gui::registered_toast(english, "ARCH-0001", 2) == "Registered revision 2 of ARCH-0001");
    CHECK(va::gui::registered_toast(chinese, "ARCH-0004", 1) == "已註冊為 ARCH-0004");
    CHECK(va::gui::registered_toast(chinese, "ARCH-0001", 2) == "已註冊 ARCH-0001 的版次 2");
    CHECK(va::gui::not_registered_toast(chinese, "/media/SAMPLE-A") == "未能註冊：/media/SAMPLE-A");
}

void sample_volumes_are_plainly_sample()
{
    // The screenshot mode's list: no drive letter, every name SAMPLE.
    const auto volumes = va::gui::sample_volumes();
    CHECK(volumes.size() >= 2);
    for (const auto& sample : volumes) {
        const auto mount = native(sample.volume.mount_point);
        CHECK(mount.starts_with("/media/SAMPLE-"));
        CHECK(sample.volume.volume_label.empty() || sample.volume.volume_label.starts_with("SAMPLE-"));
        CHECK(sample.volume.devices.size() == 1 && sample.volume.devices.front().model.starts_with("SAMPLE-DEVICE-"));
        CHECK(sample.volume.filesystem_uuid.starts_with("c0ffee00-"));
        CHECK(sample.used_bytes > 0 && sample.used_bytes < sample.volume.capacity_bytes);
    }
}

} // namespace

void register_choices_tests()
{
    volume_rows_tell_what_each_holds();
    volume_rows_of_a_relocation();
    options_apply_as_a_registration_judges_the_root();
    options_shown_and_the_request_they_make();
    a_new_revision_is_of_the_archive_asked_for();
    an_unfinished_registration_is_only_resumed();
    the_registration_report_says_what_it_recorded();
    toasts_say_what_was_registered();
    sample_volumes_are_plainly_sample();
}
