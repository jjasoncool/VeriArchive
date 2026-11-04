// The result page of a recorded run (src/gui/run_result.*, decision 0046).
// Every archive, run, item, device, computer and time below is sample data
// made up for the test (ARCH-SAMPLE-*, c0ffee00-... uuids, RECORDED-* and
// SAMPLE-* names); none is of a real archive, device or computer.
#include "test_support.h"

#include "app/device_alerts.h"
#include "app/local_time.h"
#include "core/engine/comparison.h"
#include "core/engine/read_speed.h"
#include "core/identity/volume_json.h"
#include "gui/run_result.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using va::app::Language;
using va::app::Text;
using va::gui::Field;
using va::gui::IdentityShown;
using va::gui::Pill;
using va::gui::Tone;

const va::Uuid archive = "c0ffee00-0000-4000-8000-000000000001";

// 2026-11-04T12:00:00Z, the sample "now".
const std::chrono::sys_seconds now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 4} + 12h;

std::string ago(std::chrono::seconds before)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now - before);
}

// A sample read of the content of 100 GB: each hundredth 1 GB read in
// `milliseconds_each`, of the hundredths from..to.
va::ReadSpeed sample_read(std::uint64_t milliseconds_each, std::uint32_t from = 0, std::uint32_t to = 99)
{
    va::ReadSpeed speed;
    speed.content_bytes = 100'000'000'000;
    speed.devices = {"sample SAMPLE-SERIAL"};
    speed.volume = "c0ffee00-0000-4000-8000-0000000000aa";
    for (std::uint32_t hundredth = from; hundredth <= to; ++hundredth) {
        speed.parts.push_back({hundredth, 1'000'000'000, milliseconds_each});
    }
    return speed;
}

va::VerifyRunRecord sample_run(std::string id, std::string result, std::chrono::seconds before, nlohmann::json counts)
{
    va::VerifyRunRecord run;
    run.run_id = std::move(id);
    run.archive_uuid = archive;
    run.revision = 1;
    run.mode = "FULL";
    run.started = ago(before + 12h + 44min);
    run.finished = ago(before);
    run.result = std::move(result);
    run.coverage_bytes = 1024 * 1024;
    run.counts_json = counts.dump();
    run.host = "SAMPLE-RECORDED-HOST";
    run.app_version = "0.0.0-sample";
    return run;
}

// A sample identity as a revision and the evidence record it, every name
// marked RECORDED.
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

nlohmann::json health_of(const char* status)
{
    return {{{"model", "RECORDED-MODEL"}, {"serial", "RECORDED-SERIAL"}, {"wwn", ""},
        {"health", {{"status", status}, {"conditions", nlohmann::json::array()}}}}};
}

// A recorded sample run of ARCH-SAMPLE-1 with its evidence and the
// archive's history, which holds it.
va::gui::RecordedRun recorded(va::VerifyRunRecord run, std::vector<va::VerifyRunRecord> earlier = {})
{
    va::gui::RecordedRun shown;
    shown.history = std::move(earlier);
    shown.history.push_back(run);
    shown.run = std::move(run);
    shown.evidence = va::EvidenceRecord{archive, shown.run.run_id, shown.run.finished,
        va::volume_to_json_text(recorded_volume()), "MATCH", ""};
    shown.label = "ARCH-SAMPLE-1";
    shown.volume_label = "CURRENT-LABEL";
    return shown;
}

std::string field(const std::vector<Field>& fields, const std::string& name)
{
    const auto found = std::ranges::find(fields, name, &Field::name);
    return found == fields.end() ? std::string("<none>") : found->value;
}

bool names_recorded(const va::gui::ResultPage& page)
{
    const auto holds = [](const std::string& words) { return words.find("RECORDED") != std::string::npos; };
    return holds(page.archive) || std::ranges::any_of(page.alerts, holds) ||
           std::ranges::any_of(page.identity, [&](const Field& shown) { return holds(shown.value); }) ||
           std::ranges::any_of(page.technical, [&](const Field& shown) { return holds(shown.value); });
}

// A FULL verification that PASSED, read at 100 MB/s a hundredth, compared
// with the last one, read at 125 MB/s, and slower from 20% to 30%.
void passed()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto baseline_read = sample_read(8'000);
    const auto before = sample_run("c0ffee00-0000-4000-9000-000000000001", "PASSED", 24h * 90,
        {{"items", {{"Verified", 152'408}}}, {"read_speed", va::read_speed_json(baseline_read)}});
    auto own = sample_read(10'000);
    auto compared = va::compare_read_speed(own, baseline_read);
    CHECK(compared.has_value());
    if (!compared) {
        return;
    }
    compared->baseline_run = before.run_id;
    compared->baseline_finished = before.finished;
    compared->slower_parts = {{20, 29, 60}};
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000002", "PASSED", 72h,
        {{"items", {{"Verified", 152'400}, {"MetadataMismatch", 8}}}, {"device_health", health_of("healthy")},
            {"read_speed", va::read_speed_json(own)}, {"read_compared", va::speed_comparison_json(*compared)}});
    const auto shown = recorded(run, {before});

    // In the screenshot mode the sample identity and computer, nothing recorded.
    auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(!names_recorded(page));
    CHECK(page.heading == "FULL VERIFY PASSED" && page.tone == Tone::Success && !page.incomplete);
    CHECK(page.archive == "ARCH-SAMPLE-1 · SAMPLE-VOLUME · " + va::app::local_time(run.finished, english));
    CHECK(page.title == "ARCH-SAMPLE-1 · " + va::app::local_time(run.finished, english));
    CHECK((page.lines == std::vector<std::string>{"100% of the registered content was read and matched"}));
    // The comparison's slower stretches are a device alert.
    CHECK(page.alerts.size() == 1 && page.alerts.front().starts_with(std::string(va::app::alert_mark)));
    CHECK(!page.items_first && page.items_shown && page.items.empty() && page.items_heading == "Items (0)" &&
          page.items_none == "Every entry was verified.");
    CHECK((page.metrics == std::vector<Field>{{"Read and matched", "1.0 MiB"}, {"Entries compared", "152,408"},
                               {"Duration", "12 h 44 min"}, {"Average read speed", "95.4 MiB/s"}}));
    CHECK(page.speed_shown && page.chart && page.chart->compared);
    if (page.chart) {
        // 1 GB in 10 s: 95.37 MiB/s; the one compared with 119.2 MiB/s, so
        // the axis goes up to 150 in steps of 50.
        CHECK(page.chart->now[0] && std::abs(*page.chart->now[0] - 95.367) < 0.01);
        CHECK(page.chart->before[99] && std::abs(*page.chart->before[99] - 119.209) < 0.01);
        CHECK(page.chart->step == 50.0 && page.chart->top == 150.0);
        CHECK(page.chart->slower.size() == 1 && page.chart->slower.front().from == 20);
    }
    CHECK(page.speed_lines.size() == 1 &&
          page.speed_lines.front() == "Read speed: 95.4 MiB/s; the last FULL verification of this archive on this "
                                      "device, which finished " +
                                          va::app::local_time(before.finished, english) + ", read 119 MiB/s");
    CHECK(field(page.identity, "Media identity") == "MATCH" && field(page.identity, "Volume") == "SAMPLE-VOLUME" &&
          field(page.identity, "Model") == "SAMPLE-DEVICE" && field(page.identity, "Health") == "healthy");
    CHECK(field(page.technical, "Run") == run.run_id && field(page.technical, "Mode") == "FULL" &&
          field(page.technical, "Revision") == "1" && field(page.technical, "Computer") == "SAMPLE-HOST" &&
          field(page.technical, "Program version") == "0.0.0-sample" &&
          field(page.technical, "Entries by status") == "MetadataMismatch 8 · Verified 152,400" &&
          field(page.technical, "Bytes matched") == "1048576 bytes" && field(page.technical, "Resumed from") == "<none>");
    CHECK(field(page.technical, "Guarantee").starts_with("VeriArchive detects accidental corruption"));
    // In normal use what was recorded.
    page = va::gui::result_page(english, shown, IdentityShown::Recorded);
    CHECK(page.archive.starts_with("ARCH-SAMPLE-1 · RECORDED-LABEL · ") &&
          field(page.identity, "Serial") == "RECORDED-SERIAL" &&
          field(page.technical, "Computer") == "SAMPLE-RECORDED-HOST");
    // Without evidence: the archive's current carrier, and the devices whose
    // health the run recorded.
    auto without = shown;
    without.evidence.reset();
    page = va::gui::result_page(english, without, IdentityShown::Recorded);
    CHECK(page.archive.starts_with("ARCH-SAMPLE-1 · CURRENT-LABEL · ") &&
          field(page.identity, "Media identity") == "not known" && field(page.identity, "Model") == "RECORDED-MODEL");

    page = va::gui::result_page(chinese, shown, IdentityShown::Sample);
    CHECK(page.heading == "完整驗證 通過" &&
          (page.lines == std::vector<std::string>{"100% 的已註冊內容都已讀取且相符"}));
    CHECK(page.metrics.size() == 4 && page.metrics[0] == (Field{"已讀取且相符", "1.0 MiB"}) &&
          page.metrics[1] == (Field{"已比對的項目", "152,408"}) && page.metrics[2] == (Field{"耗時", "12 小時 44 分"}) &&
          page.metrics[3].value == "每秒 95.4 MiB");
    CHECK(page.items_heading == "項目（0）" && page.items_none == "每個項目都已驗證。");
    CHECK(field(page.identity, "媒體身分") == "相符" && field(page.technical, "模式") == "完整驗證");
}

// A FULL verification that FAILED: its items first, each in the tone of what
// it does to the result, its failures in the banner.
void failed()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto run = sample_run("c0ffee00-0000-4000-9000-000000000003", "FAILED", 1h,
        {{"items", {{"Verified", 10}, {"Modified", 1}, {"MetadataMismatch", 1}}},
            {"device_health", health_of("warning")}});
    run.failure_classes = "MediaIdentityMismatch";
    auto shown = recorded(run);
    va::VerifyItemRecord modified;
    modified.run_id = run.run_id;
    modified.path.p = "SAMPLE/合約.pdf";
    modified.status = "Modified";
    modified.detail = "content differs";
    va::VerifyItemRecord metadata;
    metadata.run_id = run.run_id;
    metadata.path.p = "SAMPLE/預算表.xlsx";
    metadata.status = "MetadataMismatch";
    metadata.detail = "mtime";
    shown.items = {modified, metadata};
    auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(page.heading == "FULL VERIFY FAILED" && page.tone == Tone::Error && page.items_first);
    CHECK((page.lines == std::vector<std::string>{"Failure: the media is not the one recorded"}));
    CHECK(page.alerts.size() == 1 && page.alerts.front().find("SAMPLE-DEVICE") != std::string::npos);
    CHECK((page.items == std::vector<va::gui::ResultItem>{
                             {Pill{"Modified", Tone::Error}, "SAMPLE/合約.pdf", "content differs"},
                             {Pill{"MetadataMismatch", Tone::Warning}, "SAMPLE/預算表.xlsx", "modification time"}}));
    CHECK(page.items_heading == "Items (2)" && page.items_none.empty());
    // An old run, without its read speed: the version did not record it.
    CHECK(page.speed_shown && !page.chart && page.metrics.back().value == "—");
    CHECK((page.speed_lines == std::vector<std::string>{
                                   "The version that ran this verification did not record read speed yet."}));
    CHECK(field(page.technical, "Failures") == "the media is not the one recorded");
    page = va::gui::result_page(chinese, shown, IdentityShown::Sample);
    CHECK(page.items.size() == 2 && page.items[0].status.words == "已修改" && page.items[1].detail == "修改時間");
    CHECK((page.speed_lines == std::vector<std::string>{"這次驗證時的版本還不記錄讀取速度。"}));
    CHECK(page.lines.size() == 1 && page.lines.front() == "失敗：媒體不是記錄中的那一個");

    // With strict metadata (R-51) the metadata differences alone failed it.
    run.failure_classes.clear();
    CHECK(va::gui::item_tone(run, metadata, false) == Tone::Error);
    CHECK(va::gui::item_tone(run, metadata, true) == Tone::Warning);
    CHECK(va::gui::item_tone(run, modified, true) == Tone::Error);
    // An entry unreadable since its registration only warns (decision 0010).
    va::VerifyItemRecord known;
    known.status = "Unreadable(media)";
    known.detail = "known-since-registration";
    CHECK(!va::gui::fails_the_run(known) && va::gui::item_tone(run, known, true) == Tone::Warning);
    // Every item of a run that passed with warnings is a warning.
    run.result = "PASSED_WITH_WARNINGS";
    CHECK(va::gui::item_tone(run, modified, true) == Tone::Warning);
    shown.run = run;
    CHECK(va::gui::result_page(english, shown, IdentityShown::Sample).items_first);
}

va::VerifyItemRecord sample_item(const va::VerifyRunRecord& run, std::string path, std::string status,
    std::string detail = {}, std::string stream = {})
{
    va::VerifyItemRecord item;
    item.run_id = run.run_id;
    item.path.p = std::move(path);
    item.stream = std::move(stream);
    item.status = std::move(status);
    item.detail = std::move(detail);
    return item;
}

// The items in the order the repository gives them (by path): on the page
// what failed the run first, then what warns, each in the order of its
// paths; an alternate data stream named "path:stream".
void items_in_order()
{
    const Text english;
    const auto run = sample_run("c0ffee00-0000-4000-9000-00000000000c", "FAILED", 1h,
        {{"items", {{"Verified", 10}, {"Modified", 2}, {"Unbaselined", 1}, {"MetadataMismatch", 1}}}});
    auto shown = recorded(run);
    shown.items = {sample_item(run, "SAMPLE/A-first.txt", "Unbaselined"),
        sample_item(run, "SAMPLE/B.docx", "Modified", "content differs", "SAMPLE-STREAM"),
        sample_item(run, "SAMPLE/C.pdf", "Modified", "content differs"),
        sample_item(run, "SAMPLE/D.xlsx", "MetadataMismatch", "mtime")};
    const auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK((page.items == std::vector<va::gui::ResultItem>{
                             {Pill{"Modified", Tone::Error}, "SAMPLE/B.docx:SAMPLE-STREAM", "content differs"},
                             {Pill{"Modified", Tone::Error}, "SAMPLE/C.pdf", "content differs"},
                             {Pill{"Unbaselined", Tone::Warning}, "SAMPLE/A-first.txt", ""},
                             {Pill{"MetadataMismatch", Tone::Warning}, "SAMPLE/D.xlsx", "modification time"}}));
}

// R-30: a mount point where the revision records nothing holds no part of
// the archive; its item changes no result, in a run that PASSED or in one
// that its metadata differences failed (strict metadata).
void mount_point_outside()
{
    const Text english;
    auto run = sample_run("c0ffee00-0000-4000-9000-00000000000d", "PASSED", 1h,
        {{"items", {{"Verified", 3}, {"SkippedMountPoint", 1}}}});
    const auto outside =
        sample_item(run, "SAMPLE-mounted", "SkippedMountPoint", va::mount_point_outside_archive);
    CHECK(va::gui::changes_no_result(outside) && !va::gui::fails_the_run(outside));
    CHECK(va::gui::item_tone(run, outside, false) == Tone::Neutral);
    auto shown = recorded(run);
    shown.items = {outside};
    auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(page.items.size() == 1 && page.items.front().status == (Pill{"SkippedMountPoint", Tone::Neutral}) &&
          page.items.front().detail == "another volume is mounted here; it is not part of the archive");
    // A mount point that now covers recorded entries fails the run.
    const auto covering = sample_item(run, "SAMPLE-photos", "SkippedMountPoint", "a mount point now covers this path");
    CHECK(!va::gui::changes_no_result(covering) && va::gui::fails_the_run(covering));
    // Strict metadata: its metadata differences failed it, not the mount point.
    run.result = "FAILED";
    const auto metadata = sample_item(run, "SAMPLE/a.txt", "MetadataMismatch", "mtime");
    shown = recorded(run);
    shown.items = {metadata, outside};
    page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(page.items.size() == 2 && page.items[0].status == (Pill{"MetadataMismatch", Tone::Error}) &&
          page.items[1].status == (Pill{"SkippedMountPoint", Tone::Neutral}));
    // Passed with warnings: it is still not a warning.
    run.result = "PASSED_WITH_WARNINGS";
    CHECK(va::gui::item_tone(run, outside, false) == Tone::Neutral &&
          va::gui::item_tone(run, metadata, false) == Tone::Warning);
}

// An INCOMPLETE run that resumed one: how far the two came, that no report
// can be saved for it; no read speed section, and no items where it found none.
void incomplete()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto first = sample_run("c0ffee00-0000-4000-9000-000000000004", "INCOMPLETE", 48h,
        {{"items", {{"Verified", 3}}}, {"read_speed", va::read_speed_json(sample_read(10'000, 0, 29))}});
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000005", "INCOMPLETE", 24h,
        {{"items", {{"Verified", 5}}}, {"resumed_from", first.run_id},
            {"read_speed", va::read_speed_json(sample_read(10'000, 30, 62))}});
    const auto shown = recorded(run, {first});
    CHECK(va::gui::stopped_percent(run, shown.history) == 63);
    CHECK(va::gui::run_read_speed(run, shown.history) == 100'000'000u);
    auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(page.incomplete && page.tone == Tone::Neutral && page.heading == "FULL VERIFY INCOMPLETE");
    CHECK((page.lines == std::vector<std::string>{"Stopped at 63%: 58.7 GiB of 93.1 GiB read.",
                             "A report cannot be saved for a verification that has not finished."}));
    CHECK(!page.speed_shown && !page.items_shown);
    CHECK(field(page.technical, "Resumed from") == first.run_id);
    page = va::gui::result_page(chinese, shown, IdentityShown::Sample);
    CHECK(page.lines.size() == 2 && page.lines[0] == "停止於 63%：已讀取 58.7 GiB / 93.1 GiB。" &&
          page.lines[1] == "未完成的驗證不能儲存報告。");

    // Resume where the latest read lists its stopped verification with
    // Resume, as the details offer it; not for another run.
    va::app::UnfinishedOperation operation;
    operation.kind = va::app::OperationKind::Verification;
    operation.key = "run:" + run.run_id;
    operation.archive_uuid = archive;
    operation.label = "ARCH-SAMPLE-1";
    operation.root = "Q:/SAMPLE";
    operation.run_id = run.run_id;
    operation.revision = 1;
    operation.started = ago(25h);
    operation.recorded_stopped = true;
    operation.updated = now;
    std::vector<va::app::OperationWatch> watches;
    va::app::follow(watches, {operation}, va::app::OperationWatch::Clock::time_point{}, now);
    const va::app::OperationWatch::Clock::time_point start{};
    const auto stopped = va::gui::stopped_run(english, watches, run, start, now);
    CHECK(stopped && stopped->key == operation.key);
    CHECK(!va::gui::stopped_run(english, watches, first, start, now));
    auto passed = run;
    passed.result = "PASSED";
    CHECK(!va::gui::stopped_run(english, watches, passed, start, now));
    CHECK(!va::gui::stopped_run(english, {}, run, start, now));
}

// A run that failed on its manifest read no media (decision 0028); a
// relocation records no read speed; a read too short to time.
void without_a_read()
{
    const Text english;
    auto run = sample_run("c0ffee00-0000-4000-9000-000000000006", "FAILED", 1h,
        {{"items", nlohmann::json::object()}, {"media_read", false}});
    run.failure_classes = "ManifestHashMismatch";
    run.coverage_bytes = 0;
    auto shown = recorded(run);
    shown.evidence.reset();
    auto page = va::gui::result_page(english, shown, IdentityShown::Sample);
    CHECK(page.lines.size() == 2 &&
          page.lines[0] == "Media: not read, as the manifest of this revision cannot be used" &&
          page.lines[1] == "Failure: the manifest is not the one recorded");
    CHECK(!page.speed_shown && page.items_none == "No entry was recorded as differing.");
    CHECK((page.identity ==
           std::vector<Field>{{"Media identity", "not read, as the manifest of this revision cannot be used"}}));

    const auto relocated = sample_run("c0ffee00-0000-4000-9000-000000000007", "PASSED", 1h,
        {{"items", {{"Verified", 3}}}, {"relocated_from_revision", 1}});
    page = va::gui::result_page(english, recorded(relocated), IdentityShown::Sample);
    CHECK(page.speed_shown && (page.speed_lines == std::vector<std::string>{"A relocation does not record its read speed."}));
    CHECK(page.lines.size() == 2 && page.lines[1] == "relocation from r1" && field(page.technical, "Note") == "relocation from r1");

    // A tiny archive, read in no measurable time: nothing to chart, no speed.
    const auto fast = sample_run("c0ffee00-0000-4000-9000-000000000008", "PASSED", 1h,
        {{"items", {{"Verified", 6}}}, {"read_speed", va::read_speed_json(sample_read(0))}});
    page = va::gui::result_page(english, recorded(fast), IdentityShown::Sample);
    const std::string too_little =
        "This verification read too little to chart its read speed by position in the content.";
    CHECK(page.speed_shown && !page.chart && (page.speed_lines == std::vector<std::string>{too_little}));
    CHECK(page.metrics.back().value == "—");
    // Read in less than a second in all: its speed, but no chart.
    const auto quick = sample_run("c0ffee00-0000-4000-9000-00000000000b", "PASSED", 1h,
        {{"items", {{"Verified", 6}}}, {"read_speed", va::read_speed_json(sample_read(9, 0, 99))}});
    page = va::gui::result_page(english, recorded(quick), IdentityShown::Sample);
    CHECK(!page.chart && page.speed_lines.size() == 3 && page.speed_lines[0] == too_little &&
          page.speed_lines[1].starts_with("Read speed: "));
    CHECK(va::gui::speed_chart(sample_read(10, 0, 99), std::nullopt, {}).has_value());
    // Read, but with nothing earlier to compare with.
    const auto alone = sample_run("c0ffee00-0000-4000-9000-000000000009", "PASSED", 1h,
        {{"items", {{"Verified", 6}}}, {"read_speed", va::read_speed_json(sample_read(10'000))}});
    page = va::gui::result_page(english, recorded(alone), IdentityShown::Sample);
    CHECK(page.chart && !page.chart->compared && page.speed_lines.size() == 2 &&
          page.speed_lines[0] == "Read speed: 95.4 MiB/s" &&
          page.speed_lines[1].starts_with("No earlier FULL verification of this archive"));

    // Chunk hashes added (decision 0033): the revision that recorded them.
    const auto adding = sample_run("c0ffee00-0000-4000-9000-00000000000a", "PASSED", 1h,
        {{"items", {{"Verified", 6}}}, {"add_chunk_hashes", 16777216}, {"chunk_hashes_added_from_revision", 1}});
    auto added = adding;
    added.revision = 2;
    page = va::gui::result_page(english, recorded(added), IdentityShown::Sample);
    CHECK(field(page.technical, "Revision") == "2, which recorded the chunk hashes added to revision 1");
}

void numbers()
{
    using va::gui::axis_step;
    using va::gui::axis_words;
    CHECK(axis_step(200.0) == 50.0 && axis_step(1000.0) == 250.0 && axis_step(3.0) == 1.0);
    CHECK(axis_step(0.0) == 1.0 && axis_step(90.0) == 25.0);
    CHECK(axis_words(0.0, 50.0) == "0" && axis_words(200.0, 50.0) == "200" && axis_words(1000.0, 250.0) == "1000");
    CHECK(axis_words(0.0, 0.25) == "0.00" && axis_words(0.5, 0.25) == "0.50" && axis_words(2.5, 2.5) == "2.5");
    CHECK(axis_words(0.0015, 0.0005) == "0.0015" && axis_words(0.002, 0.0005) == "0.0020");
    CHECK(va::gui::grouped(0) == "0" && va::gui::grouped(999) == "999" && va::gui::grouped(1000) == "1,000" &&
          va::gui::grouped(152408) == "152,408" && va::gui::grouped(1234567) == "1,234,567");
    CHECK(!va::gui::speed_chart(sample_read(0), std::nullopt, {}));
    CHECK(va::gui::displayed_host("SAMPLE-RECORDED-HOST", IdentityShown::Sample) == "SAMPLE-HOST");
    CHECK(va::gui::displayed_host("", IdentityShown::Sample).empty());
}

} // namespace

void run_result_tests()
{
    passed();
    failed();
    items_in_order();
    mount_point_outside();
    incomplete();
    without_a_read();
    numbers();
}
