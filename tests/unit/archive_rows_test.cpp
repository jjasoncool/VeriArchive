// What Archives says of each archive (src/gui/archive_rows.*, decision 0046):
// the rows of its list and the details of the archive selected. Every
// archive, revision, device, run and time below is sample data made up for
// the test (ARCH-SAMPLE-*, c0ffee00-... uuids, RECORDED-* identities); none
// is of a real archive or device.
#include "test_support.h"

#include "app/device_alerts.h"
#include "app/local_time.h"
#include "core/engine/device_alerts.h"
#include "core/engine/read_speed.h"
#include "gui/archive_rows.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using va::app::Language;
using va::app::Text;
using va::gui::IdentityShown;
using va::gui::Pill;
using va::gui::Tone;

constexpr std::uint64_t one_tib = std::uint64_t{1} << 40;

// 2026-11-04T12:00:00Z, the sample "now".
const std::chrono::sys_seconds now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 4} + 12h;

std::string ago(std::chrono::seconds before)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now - before);
}

// A sample identity as a revision records it, every name marked RECORDED.
va::VolumeInfo recorded_volume()
{
    va::VolumeInfo volume;
    volume.filesystem_type = "ntfs";
    volume.filesystem_uuid = "c0ffee00c0ffee01";
    volume.volume_label = "RECORDED-LABEL";
    volume.capacity_bytes = 8001563222016;
    va::PhysicalDeviceInfo device;
    device.model = "RECORDED-MODEL";
    device.serial = "RECORDED-SERIAL";
    device.bus = "usb";
    volume.devices = {device};
    return volume;
}

va::VerifyRunRecord sample_run(std::string id, std::string result, std::chrono::seconds before,
    std::string counts = R"({"items":{}})")
{
    va::VerifyRunRecord run;
    run.run_id = std::move(id);
    run.archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    run.revision = 1;
    run.mode = "FULL";
    run.started = ago(before + 5min + 37s);
    run.finished = ago(before);
    run.result = std::move(result);
    run.coverage_bytes = 1024 * 1024;
    run.counts_json = std::move(counts);
    return run;
}

// Archive ARCH-SAMPLE-1, revision 1 with chunk hashes, its last FULL
// verification PASSED three days ago.
va::ArchiveStatus sample_status()
{
    va::ArchiveStatus status;
    status.archive.archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    status.archive.label = "ARCH-SAMPLE-1";
    status.archive.current_revision = 1;
    status.archive.status = "REGISTERED";
    va::ManifestHeader header;
    header.archive_uuid = status.archive.archive_uuid;
    header.label = status.archive.label;
    header.revision = 1;
    header.created = ago(24h * 30);
    header.identity = recorded_volume();
    header.root_hint = "Q:/SAMPLE";
    header.chunk_size = 64 * 1024 * 1024;
    header.counts.files = 152;
    header.counts.bytes = 2 * one_tib;
    status.header = header;
    status.last_full = sample_run("c0ffee00-0000-4000-9000-000000000001", "PASSED", 72h);
    status.last_any = status.last_full;
    status.last_run = status.last_full;
    return status;
}

std::string counts_with_health(const char* health)
{
    return nlohmann::json{{"items", nlohmann::json::object()},
        {"device_health", {{{"model", "RECORDED-MODEL"}, {"serial", "RECORDED-SERIAL"}, {"wwn", ""},
                              {"health", {{"status", health}, {"conditions", nlohmann::json::array()}}}}}}}
        .dump();
}

// Whether any of `values` names the recorded identity.
bool names_recorded(const std::vector<va::gui::Field>& fields)
{
    for (const auto& field : fields) {
        if (field.value.find("RECORDED") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void ages()
{
    using va::gui::age_words;
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(age_words(english, ago(10min), now) == "less than an hour ago");
    CHECK(age_words(english, ago(-10min), now) == "less than an hour ago");  // a clock set back
    CHECK(age_words(english, ago(1h), now) == "1 hour ago");
    CHECK(age_words(english, ago(5h + 59min), now) == "5 hours ago");
    CHECK(age_words(english, ago(24h), now) == "1 day ago");
    CHECK(age_words(english, ago(24h * 3 + 1h), now) == "3 days ago");
    CHECK(age_words(english, ago(24h * 13), now) == "13 days ago");
    CHECK(age_words(english, ago(24h * 14), now) == "2 weeks ago");
    CHECK(age_words(english, ago(24h * 59), now) == "8 weeks ago");
    CHECK(age_words(english, ago(24h * 60), now) == "2 months ago");
    // Never "12 months": the last days of a year are 11 months.
    CHECK(age_words(english, ago(24h * 359), now) == "11 months ago");
    CHECK(age_words(english, ago(24h * 362), now) == "11 months ago");
    CHECK(age_words(english, ago(24h * 364), now) == "11 months ago");
    CHECK(age_words(english, ago(24h * 365), now) == "1 year ago");
    CHECK(age_words(english, ago(24h * (365 + 362)), now) == "1 year 11 months ago");
    CHECK(age_words(english, ago(24h * 729), now) == "1 year 11 months ago");
    CHECK(age_words(chinese, ago(24h * 729), now) == "1 年 11 個月前");
    CHECK(age_words(english, ago(24h * (365 + 31)), now) == "1 year 1 month ago");
    CHECK(age_words(english, ago(24h * (365 + 62)), now) == "1 year 2 months ago");
    CHECK(age_words(english, ago(24h * (730 + 31)), now) == "2 years 1 month ago");
    CHECK(age_words(english, ago(24h * (730 + 95)), now) == "2 years 3 months ago");
    CHECK(age_words(english, ago(24h * 730), now) == "2 years ago");
    CHECK(age_words(english, "", now).empty());
    CHECK(age_words(chinese, ago(10min), now) == "不到 1 小時前");
    CHECK(age_words(chinese, ago(5h), now) == "5 小時前");
    CHECK(age_words(chinese, ago(24h * 3), now) == "3 天前");
    CHECK(age_words(chinese, ago(24h * 14), now) == "2 週前");
    CHECK(age_words(chinese, ago(24h * 150), now) == "5 個月前");
    CHECK(age_words(chinese, ago(24h * (365 + 62)), now) == "1 年 2 個月前");
    CHECK(age_words(chinese, ago(24h * 730), now) == "2 年前");

    using va::gui::duration_words;
    CHECK(duration_words(english, ago(8s), ago(0s)) == "8 s");
    CHECK(duration_words(english, ago(5min + 37s), ago(0s)) == "5 min 37 s");
    CHECK(duration_words(english, ago(12h + 44min + 5s), ago(0s)) == "12 h 44 min");
    CHECK(duration_words(chinese, ago(5min + 37s), ago(0s)) == "5 分 37 秒");
    CHECK(duration_words(chinese, ago(12h + 44min), ago(0s)) == "12 小時 44 分");
    CHECK(duration_words(english, "", ago(0s)).empty());
}

void results()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto run = sample_run("c0ffee00-0000-4000-9000-000000000002", "PASSED_WITH_WARNINGS", 1h);
    CHECK(va::gui::result_with_mode(english, run) == "FULL VERIFY PASSED_WITH_WARNINGS");
    CHECK(va::gui::result_with_mode(chinese, run) == "完整驗證 通過（有警告）");
    CHECK((va::gui::result_pill(chinese, run) == Pill{"通過（有警告）", Tone::Warning}));
    run.mode = "QUICK";
    CHECK(va::gui::result_with_mode(chinese, run) == "快速驗證 通過（有警告）");
    CHECK(va::gui::result_tone("PASSED") == Tone::Success);
    CHECK(va::gui::result_tone("INCOMPLETE") == Tone::Neutral);
    CHECK(va::gui::result_tone("FAILED") == Tone::Error);
    CHECK(std::string(va::gui::tone_class(Tone::Warning)) == "va-warning");
    CHECK(std::string(va::gui::tone_class(Tone::Neutral)) == "va-neutral");
}

void rows()
{
    using va::gui::archive_row;
    const Text english;
    const Text chinese(Language::TraditionalChinese);

    // In the screenshot mode the row shows the sample identity, never the
    // recorded one; in normal use the recorded one.
    const auto status = sample_status();
    auto row = archive_row(english, status, IdentityShown::Sample, now);
    CHECK(row.label == "ARCH-SAMPLE-1" && row.archive_uuid == status.archive.archive_uuid);
    CHECK(row.volume_label == "SAMPLE-VOLUME");
    CHECK(row.subtitle == "SAMPLE-DEVICE · 1.0 TiB · Last FULL verify 3 days ago");
    CHECK((row.result == Pill{"PASSED", Tone::Success}));
    CHECK(row.marks.empty() && row.problem.empty() && row.tone == Tone::Success);
    CHECK(archive_row(chinese, status, IdentityShown::Sample, now).subtitle == "SAMPLE-DEVICE・1.0 TiB・上次完整驗證 3 天前");
    row = archive_row(english, status, IdentityShown::Recorded, now);
    CHECK(row.volume_label == "RECORDED-LABEL" && row.subtitle.starts_with("RECORDED-MODEL · 7.3 TiB · "));

    // Never verified: no age, and a pill that says so.
    auto never = sample_status();
    never.last_full.reset();
    never.last_any.reset();
    never.last_run.reset();
    row = archive_row(chinese, never, IdentityShown::Sample, now);
    CHECK((row.result == Pill{"從未驗證", Tone::Neutral}) && row.tone == Tone::Neutral);
    CHECK(row.subtitle == "SAMPLE-DEVICE・1.0 TiB");

    // OVERDUE, a device alert of the last run (an interrupted one too) and a
    // retired archive each add their mark; the alert makes the dot red.
    auto marked = sample_status();
    marked.full_overdue = true;
    marked.last_run = sample_run("c0ffee00-0000-4000-9000-000000000003", "INCOMPLETE", 1h, counts_with_health("warning"));
    marked.archive.status = "RETIRED";
    row = archive_row(english, marked, IdentityShown::Sample, now);
    CHECK((row.marks == std::vector<Pill>{{"OVERDUE", Tone::Warning}, {"⚠ device reports a problem", Tone::Error},
                            {"RETIRED", Tone::Neutral}}));
    CHECK(row.tone == Tone::Error);
    row = archive_row(chinese, marked, IdentityShown::Sample, now);
    CHECK((row.marks == std::vector<Pill>{{"逾期", Tone::Warning}, {"⚠ 裝置回報異常", Tone::Error},
                            {"已停用", Tone::Neutral}}));
    marked.last_run = marked.last_full;
    marked.archive.status = "REGISTERED";
    CHECK(archive_row(english, marked, IdentityShown::Sample, now).tone == Tone::Warning);

    // An archive whose first registration has not finished says so, as
    // `archive list` does.
    auto incomplete = sample_status();
    incomplete.archive.status = "REGISTRATION_INCOMPLETE";
    incomplete.last_full.reset();
    incomplete.last_any.reset();
    incomplete.last_run.reset();
    row = archive_row(chinese, incomplete, IdentityShown::Sample, now);
    CHECK((row.marks == std::vector<Pill>{{"註冊未完成", Tone::Neutral}}));
    CHECK(archive_row(english, sample_status(), IdentityShown::Sample, now).marks.empty());

    // A last verification that FAILED.
    auto failed = sample_status();
    failed.last_full = sample_run("c0ffee00-0000-4000-9000-000000000004", "FAILED", 2h);
    failed.last_any = failed.last_full;
    row = archive_row(chinese, failed, IdentityShown::Sample, now);
    CHECK((row.result == Pill{"失敗", Tone::Error}) && row.tone == Tone::Error);

    // A manifest that cannot be used (decision 0028): the problem under the
    // row, red, but for the lost manifest of a retired archive.
    auto unusable = sample_status();
    unusable.header.reset();
    va::UnusableManifest missing;
    missing.kind = va::UnusableManifest::Kind::Missing;
    missing.archive_uuid = unusable.archive.archive_uuid;
    missing.revision = 1;
    missing.manifest = "manifests/c0ffee00-0000-4000-8000-000000000001/r0001.vam";
    unusable.unusable_manifest = missing;
    row = archive_row(english, unusable, IdentityShown::Sample, now);
    CHECK(row.problem.starts_with("Problem: ") && row.problem_alerts && row.tone == Tone::Error);
    CHECK(row.volume_label.empty() && row.subtitle == "Last FULL verify 3 days ago");
    unusable.manifest_lost = true;
    unusable.archive.status = "RETIRED";
    row = archive_row(english, unusable, IdentityShown::Sample, now);
    CHECK(row.problem.starts_with("Manifest lost: ") && !row.problem_alerts);
}

void banners()
{
    using va::gui::result_banner;
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    va::Policy policy;
    policy.full_verify_max_age_days = 180;
    policy.any_verify_max_age_days = 1;

    auto status = sample_status();
    auto banner = result_banner(english, status, policy, now);
    CHECK(banner.heading == "FULL VERIFY PASSED" && banner.tone == Tone::Success && !banner.overdue);
    CHECK((banner.lines == std::vector<std::string>{
                               "Last FULL verify: 3 days ago, " + va::app::local_time(status.last_full->finished, english)}));
    banner = result_banner(chinese, status, policy, now);
    CHECK(banner.heading == "完整驗證 通過");
    CHECK(banner.lines.front() == "上次完整驗證：3 天前，" + va::app::local_time(status.last_full->finished, chinese));

    // A later verification of another mode is said with its mode; each
    // overdue limit that fired names its days.
    auto quick = sample_run("c0ffee00-0000-4000-9000-000000000005", "PASSED", 30min);
    quick.mode = "QUICK";
    status.last_any = quick;
    status.full_overdue = true;
    status.any_overdue = true;
    banner = result_banner(english, status, policy, now);
    CHECK(banner.overdue && banner.lines.size() == 4);
    CHECK(banner.lines.size() == 4 && banner.lines[1] == "Last verify: QUICK PASSED, " +
                                                             va::app::local_time(quick.finished, english) +
                                                             ", less than an hour ago");
    CHECK(banner.lines.size() == 4 && banner.lines[2] == "OVERDUE: no FULL verify for more than 180 days" &&
          banner.lines[3] == "OVERDUE: no verify of any mode for more than 1 day");
    banner = result_banner(chinese, status, policy, now);
    CHECK(banner.lines.size() == 4 && banner.lines[2] == "逾期：超過 180 天沒有完整驗證" &&
          banner.lines[3] == "逾期：超過 1 天沒有任何模式的驗證");

    // Never verified, and retired.
    status = sample_status();
    status.last_full.reset();
    status.last_any.reset();
    status.archive.status = "RETIRED";
    banner = result_banner(english, status, policy, now);
    CHECK(banner.heading == "Never verified" && banner.tone == Tone::Neutral && banner.lines.size() == 2);
    CHECK(banner.lines.size() == 2 && banner.lines[0].starts_with("No FULL verification yet.") &&
          banner.lines[1].starts_with("Retired: "));
}

void details()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto status = sample_status();
    status.last_run = sample_run("c0ffee00-0000-4000-9000-000000000006", "INCOMPLETE", 1h, counts_with_health("warning"));

    // The device: in the screenshot mode the sample identity and the sample
    // device in the alerts and the health, nothing recorded.
    const auto sample = va::gui::device_fields(english, status, IdentityShown::Sample);
    CHECK(!names_recorded(sample));
    CHECK((sample == std::vector<va::gui::Field>{{"Volume", "SAMPLE-VOLUME"}, {"Capacity", "1.0 TiB"},
                         {"Model", "SAMPLE-DEVICE"}, {"Bus", "sample"}, {"Serial", "SAMPLE0001"},
                         {"Health in the last verification", "warning"}}));
    CHECK(names_recorded(va::gui::device_fields(english, status, IdentityShown::Recorded)));
    const auto alerts = va::gui::device_alert_lines(english, status, IdentityShown::Sample);
    CHECK(alerts.size() == 1 && alerts.front().words.starts_with(std::string(va::app::alert_mark)) &&
          alerts.front().words.find("SAMPLE-DEVICE") != std::string::npos &&
          alerts.front().words.find("RECORDED") == std::string::npos && alerts.front().slower_run.empty());
    CHECK(va::gui::device_alert_lines(english, sample_status(), IdentityShown::Sample).empty());
    // A slower read of the last FULL verification names that run, whose
    // result page charts where it read slower.
    va::SpeedComparison slower;
    slower.baseline_run = "c0ffee00-0000-4000-9000-000000000000";
    slower.baseline_finished = ago(24h * 60);
    slower.bytes_per_second = 50'000'000;
    slower.baseline_bytes_per_second = 100'000'000;
    slower.whole_compared = true;
    slower.slower_percent = 50;
    auto slowed = sample_status();
    slowed.last_full->counts_json =
        nlohmann::json{{"items", nlohmann::json::object()}, {"read_compared", va::speed_comparison_json(slower)}}.dump();
    const auto slower_lines = va::gui::device_alert_lines(english, slowed, IdentityShown::Sample);
    CHECK(slower_lines.size() == 1 && slower_lines.front().slower_run == slowed.last_full->run_id &&
          slower_lines.front().words.find("50% slower") != std::string::npos);

    // The content, with and without chunk hashes.
    auto content = va::gui::content_fields(english, status);
    CHECK(content.size() == 5 && content[0] == (va::gui::Field{"Root", "Q:/SAMPLE", true}));
    CHECK(content.size() == 5 && content[1] == (va::gui::Field{"Files", "152 files · 2.0 TiB"}));
    CHECK(content.size() == 5 && content[2].value == "1, recorded " + va::app::local_time(status.header->created, english));
    CHECK(content.size() == 5 && content[3] == (va::gui::Field{"Chunk hashes", "yes (chunk size 64.0 MiB)"}));
    CHECK(content.size() == 5 && content[4] == (va::gui::Field{"Archive UUID", status.archive.archive_uuid}));
    // The menu of FULL verify lists adding chunk hashes only for an archive
    // without them (decision 0033).
    CHECK(!va::gui::lists_adding_chunk_hashes(status));
    status.header->chunk_size = 0;
    CHECK(va::gui::lists_adding_chunk_hashes(status));
    content = va::gui::content_fields(chinese, status);
    CHECK(content.size() == 5 && content[3] == (va::gui::Field{"區塊雜湊", "沒有"}));
    CHECK(content.size() == 5 && content[4].name == "封存 UUID");

    // The menus: adding chunk hashes as its rule says (decision 0033), a new
    // revision where the root is known, a comparison with another archive.
    using va::gui::adding_choice;
    const auto offered = adding_choice(chinese, std::optional<std::string>{}, false);
    CHECK(offered.offered && offered.note.starts_with("每個檔案只讀取一次"));
    const auto refused = adding_choice(english, std::optional<std::string>{"a retired archive takes no new revisions"}, false);
    CHECK(!refused.offered && refused.note == "a retired archive takes no new revisions");
    CHECK(!adding_choice(english, std::optional<std::string>{}, true).offered);
    CHECK(!adding_choice(english, std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "sample failure"}), false).offered);
    CHECK(va::gui::new_revision_choice(english, sample_status()).offered);
    auto rootless = sample_status();
    rootless.header->root_hint.clear();
    CHECK(!va::gui::new_revision_choice(english, rootless).offered);
    rootless.header.reset();
    CHECK(!va::gui::new_revision_choice(english, rootless).offered);
    CHECK(!va::gui::compare_choice(english, 1).offered && va::gui::compare_choice(english, 2).offered);
}

void copies()
{
    const Text english;
    auto original = sample_status();
    auto copy = sample_status();
    copy.archive.archive_uuid = "c0ffee00-0000-4000-8000-000000000002";
    copy.archive.label = "ARCH-SAMPLE-2";
    copy.header->copy_of = original.archive.archive_uuid;
    auto unrelated = sample_status();
    unrelated.archive.archive_uuid = "c0ffee00-0000-4000-8000-000000000003";
    unrelated.archive.label = "ARCH-SAMPLE-3";
    const std::vector<va::ArchiveStatus> all{original, copy, unrelated};

    // Both ways, each with what a comparison takes as A and B.
    const auto of_original = va::gui::copy_links(english, original, all, IdentityShown::Sample);
    CHECK(of_original.size() == 1 && of_original[0].label == "ARCH-SAMPLE-2" && of_original[0].listed &&
          of_original[0].how == "Recorded as a copy of this archive" &&
          of_original[0].reference == original.archive.archive_uuid &&
          of_original[0].copy == copy.archive.archive_uuid && of_original[0].volume_label == "SAMPLE-VOLUME");
    const auto of_copy = va::gui::copy_links(english, copy, all, IdentityShown::Sample);
    CHECK(of_copy.size() == 1 && of_copy[0].label == "ARCH-SAMPLE-1" &&
          of_copy[0].how == "This archive is recorded as a copy of it" &&
          of_copy[0].reference == original.archive.archive_uuid && of_copy[0].copy == copy.archive.archive_uuid);
    CHECK(va::gui::copy_links(english, unrelated, all, IdentityShown::Sample).empty());
    // A copy of an archive of another repository (decision 0005).
    copy.header->copy_of = "c0ffee00-0000-4000-8000-0000000000ff";
    const auto elsewhere = va::gui::copy_links(english, copy, all, IdentityShown::Sample);
    CHECK(elsewhere.size() == 1 && !elsewhere[0].listed && elsewhere[0].label == "c0ffee00");
}

void runs()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    std::vector<va::VerifyRunRecord> history{
        sample_run("c0ffee00-0000-4000-9000-000000000011", "PASSED", 24h * 10),
        sample_run("c0ffee00-0000-4000-9000-000000000012", "INCOMPLETE", 24h * 5, counts_with_health("warning")),
        sample_run("c0ffee00-0000-4000-9000-000000000013", "FAILED", 24h * 2),
    };
    const auto rows = va::gui::recent_runs(english, history, 2);
    CHECK(rows.size() == 2 && rows[0].run_id == "c0ffee00-0000-4000-9000-000000000013" &&
          rows[1].run_id == "c0ffee00-0000-4000-9000-000000000012");
    CHECK(rows.size() == 2 && (rows[0].result == Pill{"FAILED", Tone::Error}) && rows[0].marks.empty());
    CHECK(rows.size() == 2 && rows[0].title == va::app::local_time(history[2].finished, english));
    CHECK(rows.size() == 2 && rows[0].meta == "1.0 MiB · 5 min 37 s");
    const std::vector<Pill> health_mark{{"⚠ device reports a problem", Tone::Error}};
    CHECK(rows.size() == 2 && rows[1].marks == health_mark);
    const auto all = va::gui::recent_runs(chinese, history, 10);
    CHECK(all.size() == 3 && (all[1].result == Pill{"未完成", Tone::Neutral}) && all[0].meta == "1.0 MiB・5 分 37 秒");

    // The toasts once a verification ended.
    CHECK(va::gui::verified_toast(english, "ARCH-SAMPLE-1", "PASSED") == "ARCH-SAMPLE-1: FULL VERIFY PASSED");
    CHECK(va::gui::verified_toast(chinese, "ARCH-SAMPLE-1", "FAILED") == "ARCH-SAMPLE-1：完整驗證 失敗");
    CHECK(va::gui::not_verified_toast(chinese, "ARCH-SAMPLE-1") == "未能驗證：ARCH-SAMPLE-1");

    // A stopped verification, as the details offer to resume it.
    va::app::OperationRow stopped;
    stopped.progress = "1.0 GiB of 2.0 GiB (50%)";
    CHECK(va::gui::stopped_verification_words(english, stopped, false) ==
          "The last FULL verification of this archive stopped at 1.0 GiB of 2.0 GiB (50%). Resume continues it "
          "without reading again what it read.");
    CHECK(va::gui::stopped_verification_words(chinese, stopped, true) ==
          "此封存上次加入區塊雜湊的完整驗證停止於 1.0 GiB of 2.0 GiB (50%)。按「繼續」即可接續，已讀過的不必重讀。");

    // No second verification while the archive's job waits or works.
    CHECK(va::gui::archive_has_a_job(chinese, "ARCH-SAMPLE-1") ==
          "ARCH-SAMPLE-1 已經有一個工作在等候或執行；請先在「工作」停止它，或等它結束。");
}

// A sample unfinished verification of ARCH-SAMPLE-1 (or of `archive`), half
// done: stopped as its record says, or working.
va::app::UnfinishedOperation sample_verification(std::string run, bool stopped,
    va::Uuid archive = "c0ffee00-0000-4000-8000-000000000001")
{
    va::app::UnfinishedOperation operation;
    operation.kind = va::app::OperationKind::Verification;
    operation.key = "run:" + run;
    operation.archive_uuid = std::move(archive);
    operation.label = "ARCH-SAMPLE-1";
    operation.root = "Q:/SAMPLE";
    operation.run_id = std::move(run);
    operation.revision = 1;
    operation.started = ago(1h);
    operation.files_done = 76;
    operation.bytes_done = std::uint64_t{1} << 30;
    operation.files_total = 152;
    operation.bytes_total = std::uint64_t{2} << 30;
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

// What the details show of an archive's unfinished verifications, and what
// their Resume runs.
void operations_in_details()
{
    using va::gui::details_operations;
    const Text english;
    const va::Uuid archive = "c0ffee00-0000-4000-8000-000000000001";
    const va::app::OperationWatch::Clock::time_point start{};

    // A stopped one is offered, as its row under Unfinished operations
    // offers Resume; one that works is not; one of another archive is not
    // this archive's.
    auto shown = details_operations(english,
        watched({sample_verification("c0ffee00-0000-4000-9000-0000000000a1", false),
            sample_verification("c0ffee00-0000-4000-9000-0000000000a2", true, "c0ffee00-0000-4000-8000-000000000002"),
            sample_verification("c0ffee00-0000-4000-9000-0000000000a3", true)}),
        archive, start, now);
    CHECK(shown.stopped && shown.stopped->run_id == "c0ffee00-0000-4000-9000-0000000000a3" &&
          shown.stopped->key == "run:c0ffee00-0000-4000-9000-0000000000a3" && !shown.adding_unfinished);
    CHECK(shown.words.starts_with("The last FULL verification of this archive stopped at "));
    shown = details_operations(
        english, watched({sample_verification("c0ffee00-0000-4000-9000-0000000000a1", false)}), archive, start, now);
    CHECK(!shown.stopped && shown.words.empty() && !shown.adding_unfinished);

    // One that adds chunk hashes, working or not, keeps a second one from
    // being offered (decision 0033).
    auto adding = sample_verification("c0ffee00-0000-4000-9000-0000000000a4", false);
    adding.adds_chunk_hashes = true;
    shown = details_operations(english, watched({adding}), archive, start, now);
    CHECK(shown.adding_unfinished && !shown.stopped);
    adding.recorded_stopped = true;
    shown = details_operations(english, watched({adding}), archive, start, now);
    CHECK(shown.adding_unfinished && shown.stopped &&
          shown.words.starts_with("The last FULL verification of this archive that adds chunk hashes stopped at "));

    // A stopped relocation of the archive, which Resume in the details
    // continues too (decision 0046); not one whose archive is retired since.
    auto relocation = sample_verification("c0ffee00-0000-4000-9000-0000000000a6", true);
    relocation.kind = va::app::OperationKind::Relocation;
    relocation.key = "relocation:" + archive;
    relocation.root = "Q:/SAMPLE-NEW";
    shown = details_operations(english, watched({relocation}), archive, start, now);
    CHECK(shown.stopped && shown.stopped->key == "relocation:" + archive && !shown.adding_unfinished &&
          shown.words == "The relocation of this archive to Q:/SAMPLE-NEW stopped at 1.00 GiB of 2.00 GiB (50%). "
                         "Resume continues it without reading again what matched.");
    CHECK(details_operations(Text(va::app::Language::TraditionalChinese), watched({relocation}), archive, start, now)
              .words == "此封存搬移至 Q:/SAMPLE-NEW 的作業停止於 1.00 GiB / 2.00 GiB（50%）。按「繼續」即可接續，已相符的不必重讀。");
    relocation.archive_retired = true;
    relocation.resumable = false;
    CHECK(!details_operations(english, watched({relocation}), archive, start, now).stopped);

    // Resume: the run that this window interrupted last as it ran it, at
    // its root or by its uuid, with its strict metadata; any other exactly
    // as on its row under Unfinished operations, strict metadata as the
    // policy says.
    using va::gui::details_resume;
    using va::gui::InterruptedRun;
    const auto operation = sample_verification("c0ffee00-0000-4000-9000-0000000000a5", true);
    va::Config config;
    auto request = details_resume(operation, std::nullopt, config);
    CHECK(request.resume_run == operation.run_id && request.archive_uuid == archive && !request.root &&
          !request.strict_metadata);
    const InterruptedRun by_uuid{operation.run_id, std::nullopt, archive, true};
    request = details_resume(operation, by_uuid, config);
    CHECK(request.resume_run == operation.run_id && request.archive_uuid == archive && !request.root &&
          request.strict_metadata);
    const InterruptedRun at_root{operation.run_id, std::filesystem::path("Q:/SAMPLE"), archive, false};
    request = details_resume(operation, at_root, config);
    CHECK(request.root == std::filesystem::path("Q:/SAMPLE") && !request.archive_uuid && !request.strict_metadata);
    config.policy.strict_metadata = true;
    request = details_resume(operation, at_root, config);
    CHECK(request.root && request.strict_metadata);
    // Another run than the window's: as on its row.
    const InterruptedRun other_run{"c0ffee00-0000-4000-9000-0000000000a6", std::nullopt, archive, true};
    config.policy.strict_metadata = false;
    request = details_resume(operation, other_run, config);
    CHECK(request.resume_run == operation.run_id && request.archive_uuid == archive && !request.strict_metadata);
}

} // namespace

void archive_rows_tests()
{
    ages();
    results();
    rows();
    banners();
    details();
    copies();
    runs();
    operations_in_details();
}
