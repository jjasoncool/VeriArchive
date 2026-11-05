// What the menu of an archive's details offers besides a comparison and a new
// revision (src/gui/archive_actions.*, decision 0046): the relocation to a
// new carrier (or the resume of a stopped one), the export of its manifest
// and its retirement, where each applies and why not; the relocation
// dialog's words and summary, and the new roots it refuses; the folder an
// export writes into and where it refuses to; the question before a
// retirement, also of an archive whose latest manifest is lost; and the
// titles, reports and toasts of their jobs. Every archive, volume, device,
// path and number below is sample data made up for the test (ARCH-SAMPLE-*
// labels, SAMPLE-* volumes and serials, c0ffee00-... uuids, sample roots);
// none is of a real archive.
#include "test_support.h"

#include "common/utf8.h"
#include "gui/archive_actions.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using va::app::Language;
using va::app::Text;

const va::Uuid sample_uuid = "c0ffee00-0000-4000-8000-0000000000b2";
const va::Uuid other_uuid = "c0ffee00-0000-4000-8000-0000000000b3";
const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

// A sample unfinished relocation of ARCH-SAMPLE-2 to a sample root: stopped
// as its record says, or working.
va::app::UnfinishedOperation sample_relocation(bool stopped)
{
    va::app::UnfinishedOperation operation;
    operation.kind = va::app::OperationKind::Relocation;
    operation.key = "relocation:" + sample_uuid;
    operation.archive_uuid = sample_uuid;
    operation.label = "ARCH-SAMPLE-2";
    operation.root = "Q:\\SAMPLE-NEW";
    operation.run_id = "c0ffee00-0000-4000-9000-0000000000c1";
    operation.revision = 2;
    operation.started = "2026-01-02T03:04:05Z";
    operation.files_done = 10;
    operation.files_total = 40;
    operation.recorded_stopped = stopped;
    operation.updated = now;
    return operation;
}

std::vector<va::app::OperationWatch> watched(const std::vector<va::app::UnfinishedOperation>& operations)
{
    std::vector<va::app::OperationWatch> watches;
    va::app::follow(watches, operations, va::app::OperationWatch::Clock::time_point{}, now);
    return watches;
}

va::ArchiveStatus sample_status(bool with_manifest = true, std::string status = "REGISTERED")
{
    va::ArchiveStatus shown;
    shown.archive.archive_uuid = sample_uuid;
    shown.archive.label = "ARCH-SAMPLE-2";
    shown.archive.current_revision = 2;
    shown.archive.status = std::move(status);
    if (with_manifest) {
        va::ManifestHeader header;
        header.archive_uuid = sample_uuid;
        header.label = "ARCH-SAMPLE-2";
        header.revision = 2;
        header.root_hint = "Q:\\SAMPLE-OLD";
        header.identity.volume_label = "SAMPLE-VOLUME-OLD";
        header.counts.files = 40;
        header.counts.streams = 3;  // not files (GLOSSARY Entry): the details show 40 files too
        header.counts.bytes = std::uint64_t{3} << 20;
        shown.header = std::move(header);
    }
    return shown;
}

void each_item_applies_where_it_can()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto usable = sample_status();
    const auto lost = sample_status(false);
    const auto retired = sample_status(true, "RETIRED");

    // Relocation: an archive whose latest manifest can be used, not retired.
    CHECK(va::gui::relocation_choice(english, usable).offered);
    const auto not_retired = va::gui::relocation_choice(english, retired);
    CHECK(!not_retired.offered && not_retired.note == "A retired archive takes no new revisions.");
    CHECK(va::gui::relocation_choice(chinese, retired).note == "已停用的封存不再接受新的版次。");
    const auto unusable = va::gui::relocation_choice(english, lost);
    CHECK(!unusable.offered && unusable.note.starts_with("The manifest of its latest revision cannot be used"));

    // An unfinished relocation of the archive: no second one; a stopped one
    // is resumed, one that works is not offered, saying where it is listed.
    const va::app::OperationWatch::Clock::time_point start{};
    CHECK(!va::gui::listed_relocation(english, watched({}), sample_uuid, start, now));
    const auto stopped = va::gui::listed_relocation(english, watched({sample_relocation(true)}), sample_uuid, start, now);
    CHECK(stopped && stopped->resumable && stopped->operation.root == "Q:\\SAMPLE-NEW");
    CHECK(!va::gui::listed_relocation(english, watched({sample_relocation(true)}), other_uuid, start, now));
    const auto resume = va::gui::relocation_choice(english, usable, stopped);
    CHECK(resume.offered &&
          resume.note == "Resumes the stopped relocation to Q:\\SAMPLE-NEW, also at another root where the new "
                         "carrier is attached at another drive letter or mount point now.");
    CHECK(va::gui::relocation_choice(chinese, usable, stopped).note.starts_with("繼續已停止、搬移至 Q:\\SAMPLE-NEW 的作業"));
    const auto working = va::gui::listed_relocation(english, watched({sample_relocation(false)}), sample_uuid, start, now);
    CHECK(working && !working->resumable && !working->state.empty());
    const auto busy = va::gui::relocation_choice(english, usable, working);
    CHECK(!busy.offered && busy.note == "An unfinished relocation of this archive to Q:\\SAMPLE-NEW is listed under "
                                        "Unfinished operations in Archives (" +
                                            working->state + ").");
    // Retired, nothing is offered, whatever is unfinished.
    CHECK(!va::gui::relocation_choice(english, retired, stopped).offered);

    // Export: whatever the status, while the latest manifest can be used.
    const auto exported = va::gui::export_choice(english, retired);
    CHECK(exported.offered && exported.note ==
                                  "Writes the manifest of revision 2 and identity.json into a folder named for the "
                                  "archive inside the folder you choose; `import` on the command line takes it into "
                                  "this repository or another.");
    CHECK(!va::gui::export_choice(english, lost).offered);
    CHECK(va::gui::export_choice(chinese, usable).note.starts_with("在您選擇的資料夾中建立以封存命名的資料夾，寫入版次 2 的清單檔"));

    // Retirement: once.
    CHECK(va::gui::retire_choice(english, usable).offered && va::gui::retire_choice(english, lost).offered);
    const auto again = va::gui::retire_choice(english, retired);
    CHECK(!again.offered && again.note == "This archive is retired already.");
    CHECK(va::gui::retire_choice(chinese, retired).note == "這個封存已經停用。");
}

void the_relocation_dialog_says_what_it_does()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::relocation_intro(english) ==
          "VeriArchive copies nothing: first copy the archive's files to the new carrier with your own tools, then "
          "choose here the folder or volume that holds them.");
    CHECK(va::gui::relocation_intro(chinese) ==
          "VeriArchive 不會複製任何東西：請先用您自己的工具把封存的檔案複製到新的載體，再在這裡選擇存放這些檔案的資料夾或"
          "磁碟區。");
    // GLOSSARY: a Copy is another archive; the files copied are the new carrier.
    CHECK(!va::gui::relocation_intro(english).contains("the copy") && !va::gui::relocation_intro(chinese).contains("複本"));
    CHECK(va::gui::relocation_resuming(english, "Q:\\SAMPLE-NEW") ==
          "The relocation of this archive to Q:\\SAMPLE-NEW stopped. Resume relocation continues it without reading "
          "again what matched; where the new carrier is attached at another drive letter or mount point now, give its "
          "root below.");
    CHECK(va::gui::relocation_resuming(chinese, "Q:\\SAMPLE-NEW").starts_with("此封存搬移至 Q:\\SAMPLE-NEW 的作業已停止。"));
    CHECK(va::gui::relocation_reading(english, 2) ==
          "The relocation reads the new carrier in full and accepts it only when every entry matches revision 2; it "
          "then records revision 3 with the new carrier, and the archive keeps its UUID and its history.");
    CHECK(va::gui::relocation_reading(chinese, 2) ==
          "搬移時會完整讀取新的載體，只有在每個項目都與版次 2 相符時才接受它；接著以新的載體記錄版次 3，封存保留它的 "
          "UUID 與歷程。");

    const auto status = sample_status();
    using va::gui::Field;
    const auto chosen =
        va::gui::relocation_summary(english, status, "/media/SAMPLE-NEW/ARCH-SAMPLE-2", va::gui::IdentityShown::Recorded);
    CHECK(chosen == (std::vector<Field>{{"Archive", "ARCH-SAMPLE-2 · SAMPLE-VOLUME-OLD"},
                       {"Recorded root", "Q:\\SAMPLE-OLD", true}, {"New root", "/media/SAMPLE-NEW/ARCH-SAMPLE-2", true},
                       {"Reads", "40 files (3.0 MiB), each in full"},
                       {"Records", "revision 3 with the new carrier, once every entry matches"}}));
    // Before a root is chosen; and the carrier as the screenshot mode shows it.
    const auto none = va::gui::relocation_summary(english, status, "", va::gui::IdentityShown::Sample);
    CHECK(none.size() == 5 && none[0].value == "ARCH-SAMPLE-2 · SAMPLE-VOLUME" &&
          (none[2] == Field{"New root", "not chosen yet", false}));
    CHECK(va::gui::relocation_summary(chinese, status, "", va::gui::IdentityShown::Recorded)[2].value == "尚未選擇");

    CHECK(va::gui::relocation_title(english, "ARCH-SAMPLE-2", "/media/SAMPLE-NEW", false) ==
          "Relocate ARCH-SAMPLE-2 to /media/SAMPLE-NEW");
    CHECK(va::gui::relocation_title(chinese, "ARCH-SAMPLE-2", "/media/SAMPLE-NEW", true) ==
          "繼續把 ARCH-SAMPLE-2 搬移至 /media/SAMPLE-NEW");
    CHECK(va::gui::relocated_toast(english, "ARCH-SAMPLE-2", 3) == "ARCH-SAMPLE-2: relocated as revision 3");
    CHECK(va::gui::relocated_toast(chinese, "ARCH-SAMPLE-2", 3) == "ARCH-SAMPLE-2：已搬移，成為版次 3");
    CHECK(va::gui::not_relocated_toast(english, "ARCH-SAMPLE-2", true) == "Relocation of ARCH-SAMPLE-2: stopped");
    CHECK(va::gui::not_relocated_toast(chinese, "ARCH-SAMPLE-2", false) == "未能搬移：ARCH-SAMPLE-2");
}

void an_export_writes_into_a_folder_of_its_own()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto status = sample_status();
    CHECK(va::gui::export_folder_name(status) == "ARCH-SAMPLE-2 c0ffee00");
    // What no file name can hold, on Windows or Linux, and dots and spaces
    // at its end.
    status.archive.label = "SAMPLE<1>:a/b\\c|d?e*f\"g";
    CHECK(va::gui::export_folder_name(status) == "SAMPLE_1__a_b_c_d_e_f_g c0ffee00");
    status.archive.label.clear();
    CHECK(va::gui::export_folder_name(status) == "c0ffee00");
    status.archive.label = "範例 封存";
    CHECK(va::gui::export_folder_name(status) == "範例 封存 c0ffee00");

    // Never into the root of an attached archive, nor a folder below it.
    const std::vector<va::gui::AttachedArchiveRoot> roots{
        {std::filesystem::path("/media/SAMPLE-1/photos"), "ARCH-SAMPLE-1"}};
    CHECK(!va::gui::export_refusal(english, "/media/SAMPLE-1", roots));
    CHECK(!va::gui::export_refusal(english, "/media/SAMPLE-1/photos-export", roots));
    const auto inside = va::gui::export_refusal(english, "/media/SAMPLE-1/photos/exports", roots);
    CHECK(inside && inside->starts_with(va::path_to_utf8(std::filesystem::path("/media/SAMPLE-1/photos/exports")) +
                                        " lies in the root of ARCH-SAMPLE-1 ("));
    CHECK(va::gui::export_refusal(english, "/media/SAMPLE-1/photos", roots).has_value());
    CHECK(va::gui::export_refusal(chinese, "/media/SAMPLE-1/photos", roots)->contains("ARCH-SAMPLE-1"));
    // The same words for a root that holds the folder as the file system
    // names the two (app::roots_holding), a sample link that names it here.
    CHECK(va::gui::export_refusal_in_root(english, "/media/SAMPLE-1/photos/exports", roots.front()) == *inside);
    const auto linked = va::gui::export_refusal_in_root(chinese, "/home/SAMPLE/link-to-photos", roots.front());
    CHECK(linked.starts_with(va::path_to_utf8(std::filesystem::path("/home/SAMPLE/link-to-photos")) +
                             " 位於 ARCH-SAMPLE-1 的根目錄（"));

    const va::ExportResult written{"/media/SAMPLE-EXPORT/ARCH-SAMPLE-2 c0ffee00/r0002.vam",
        "/media/SAMPLE-EXPORT/ARCH-SAMPLE-2 c0ffee00/identity.json"};
    const auto report = va::gui::export_report(english, "ARCH-SAMPLE-2", 2, written);
    CHECK(report.starts_with("Exported the manifest of revision 2 of ARCH-SAMPLE-2\n\nManifest: "));
    CHECK(report.contains("\nIdentity file: " + va::path_to_utf8(written.identity) + "\n"));
    CHECK(report.ends_with("\n\nThis repository or another takes it in with `import <file.vam>` on the command line, "
                           "also to put back a lost manifest; it is checked there against the revisions that "
                           "repository holds.\n"));
    const auto chinese_report = va::gui::export_report(chinese, "ARCH-SAMPLE-2", 2, written);
    CHECK(chinese_report.starts_with("已匯出 ARCH-SAMPLE-2 版次 2 的清單檔"));
    CHECK(chinese_report.contains("\n身分檔案：" + va::path_to_utf8(written.identity) + "\n"));
    CHECK(va::gui::export_title(english, "ARCH-SAMPLE-2") == "Export the manifest of ARCH-SAMPLE-2");
    CHECK(va::gui::exported_toast(chinese, "ARCH-SAMPLE-2") == "已匯出 ARCH-SAMPLE-2 的清單檔");
    CHECK(va::gui::not_exported_toast(english, "ARCH-SAMPLE-2", true) == "Export of ARCH-SAMPLE-2: stopped");
    CHECK(va::gui::not_exported_toast(chinese, "ARCH-SAMPLE-2", false) == "未能匯出：ARCH-SAMPLE-2");
}

va::VolumeInfo sample_volume(std::string mount_point, std::string filesystem_uuid, std::string serial)
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = std::move(filesystem_uuid);
    info.volume_label = "SAMPLE-VOLUME";
    info.capacity_bytes = std::uint64_t{1} << 40;
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = std::move(serial);
    device.bus = "sample-usb";
    info.devices.push_back(device);
    return info;
}

va::ArchiveCandidate sample_candidate(const va::Uuid& uuid, va::VolumeInfo identity, std::string root_subpath)
{
    va::ArchiveCandidate archive;
    archive.archive_uuid = uuid;
    archive.identity = std::move(identity);
    archive.identity.mount_point.clear();
    archive.root_subpath = std::move(root_subpath);
    return archive;
}

// The new root, checked before the relocation starts as the relocation
// judges it (decision 0001): never overlapping another archive's root on its
// volume, also one whose filesystem is there on another device; the
// archive's own root never counts.
void a_new_root_overlaps_no_other_archive()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto volume = sample_volume("/media/SAMPLE-N", "7777000077770000", "SAMPLE-SERIAL-N");
    const std::vector<va::gui::ArchiveName> names{{other_uuid, "ARCH-SAMPLE-3", "SAMPLE-VOLUME"}};
    // ARCH-SAMPLE-3 in "Photos" on this volume; ARCH-SAMPLE-2's own
    // filesystem in "Copy", on another device than recorded (MIGRATED).
    const std::vector<va::ArchiveCandidate> candidates{
        sample_candidate(other_uuid, volume, "Photos"),
        sample_candidate(sample_uuid, sample_volume("", "7777000077770000", "SAMPLE-SERIAL-OLD"), "Copy"),
    };
    CHECK(!va::gui::relocation_root_refusal(english, volume, "Copy", candidates, sample_uuid, names));
    CHECK(!va::gui::relocation_root_refusal(english, volume, "Photos-2", candidates, sample_uuid, names));
    const auto photos = va::path_to_utf8(std::filesystem::path("/media/SAMPLE-N") / "Photos");
    const auto inside = va::gui::relocation_root_refusal(english, volume, "Photos/2026", candidates, sample_uuid, names);
    CHECK(inside == "This folder or volume overlaps the root of ARCH-SAMPLE-3 (" + photos +
                        "): archive roots on one volume may not contain each other. Choose a folder that does not "
                        "overlap it.");
    // The whole volume holds that root.
    CHECK(va::gui::relocation_root_refusal(english, volume, "", candidates, sample_uuid, names).has_value());
    CHECK(va::gui::relocation_root_refusal(chinese, volume, "", candidates, sample_uuid, names) ==
          "此資料夾或磁碟區與 ARCH-SAMPLE-3（" + photos +
              "） 的根目錄重疊：同一磁碟區上的封存根目錄不得互相包含。請選擇不與它重疊的資料夾。");
    // Another volume: nothing of it overlaps.
    const auto elsewhere = sample_volume("/media/SAMPLE-M", "8888000088880000", "SAMPLE-SERIAL-M");
    CHECK(!va::gui::relocation_root_refusal(english, elsewhere, "", candidates, sample_uuid, names));
}

void a_retirement_asks_first()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    va::Retirement retirement{sample_uuid, "ARCH-SAMPLE-2", 2, std::nullopt, false};
    auto question = va::gui::retire_question(english, "ARCH-SAMPLE-2", retirement);
    CHECK(question.heading == "Retire ARCH-SAMPLE-2?" && question.retire == "Retire" && !question.confirm);
    CHECK(question.body ==
          "A retired archive is no longer expected to be verified and is never overdue. It keeps its verification "
          "history and is still recognised when attached, but it takes no new revisions. A retired archive cannot be "
          "made active again.");
    CHECK(va::gui::retire_question(chinese, "ARCH-SAMPLE-2", retirement).retire == "停用");

    // Its latest manifest lost: the command line's explanation, and an
    // explicit confirmation.
    va::UnusableManifest lost;
    lost.kind = va::UnusableManifest::Kind::Missing;
    lost.archive_uuid = sample_uuid;
    lost.revision = 2;
    lost.manifest = "manifests/" + sample_uuid + "/r0002.vam";
    retirement.lost_manifest = lost;
    question = va::gui::retire_question(english, "ARCH-SAMPLE-2", retirement);
    CHECK(question.confirm && !question.confirmation.empty());
    // As one paragraph, without the command line's indent; that its commands
    // are the command line's; and that there is no way back. Never that it is
    // still recognised when attached, which a lost manifest stops.
    CHECK(question.body.starts_with("Lost manifest of ARCH-SAMPLE-2 (" + sample_uuid + "), revision 2\nthe "
                                    "manifest of revision 2 (manifests/" +
                                    sample_uuid + "/r0002.vam) is missing\nRetiring the archive declares that "
                                                  "manifest lost for good: "));
    CHECK(!question.body.contains("\n  "));
    CHECK(question.body.contains("\n\n`import` and `register --lost-manifest` are commands of the command line.\n\n"));
    CHECK(question.body.ends_with("A retired archive cannot be made active again."));
    CHECK(!question.body.contains("still recognised when attached"));
    const auto chinese_question = va::gui::retire_question(chinese, "ARCH-SAMPLE-2", retirement);
    CHECK(chinese_question.body.contains("停用此封存，就是宣告"));
    CHECK(chinese_question.body.ends_with("\n\n`import` 與 `register --lost-manifest` 都是命令列的命令。\n\n"
                                          "已停用的封存無法再恢復使用。"));

    CHECK(va::gui::retired_toast(english, "ARCH-SAMPLE-2") == "Retired ARCH-SAMPLE-2");
    CHECK(va::gui::retired_toast(chinese, "ARCH-SAMPLE-2") == "已停用 ARCH-SAMPLE-2");
}

} // namespace

void archive_actions_tests()
{
    each_item_applies_where_it_can();
    the_relocation_dialog_says_what_it_does();
    an_export_writes_into_a_folder_of_its_own();
    a_new_root_overlaps_no_other_archive();
    a_retirement_asks_first();
}
