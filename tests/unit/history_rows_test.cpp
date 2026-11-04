// History across archives (src/gui/history_rows.*, decision 0046): its rows,
// its filter and what the check of the chain says. Every archive, run and
// time below is sample data made up for the test (ARCH-SAMPLE-*, c0ffee00-...
// uuids, RECORDED-* identities); none is of a real archive or device.
#include "test_support.h"

#include "app/local_time.h"
#include "core/engine/read_speed.h"
#include "gui/history_rows.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using va::app::Language;
using va::app::Text;
using va::gui::IdentityShown;
using va::gui::Pill;
using va::gui::Tone;

const va::Uuid first_archive = "c0ffee00-0000-4000-8000-000000000001";
const va::Uuid second_archive = "c0ffee00-0000-4000-8000-000000000002";

// 2026-11-04T12:00:00Z, the sample "now".
const std::chrono::sys_seconds now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 4} + 12h;

std::string ago(std::chrono::seconds before)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now - before);
}

va::VerifyRunRecord sample_run(std::string id, const va::Uuid& archive, std::string result,
    std::chrono::seconds before, nlohmann::json counts = {{"items", nlohmann::json::object()}})
{
    va::VerifyRunRecord run;
    run.run_id = std::move(id);
    run.archive_uuid = archive;
    run.revision = 1;
    run.mode = "FULL";
    run.started = ago(before + 5min + 37s);
    run.finished = ago(before);
    run.result = std::move(result);
    run.coverage_bytes = 1024 * 1024;
    run.counts_json = counts.dump();
    return run;
}

// Half of 100 GB read, each hundredth 1 GB in 10 s.
va::ReadSpeed half_read()
{
    va::ReadSpeed speed;
    speed.content_bytes = 100'000'000'000;
    speed.devices = {"sample SAMPLE-SERIAL"};
    speed.volume = "c0ffee00-0000-4000-8000-0000000000aa";
    for (std::uint32_t hundredth = 0; hundredth < 50; ++hundredth) {
        speed.parts.push_back({hundredth, 1'000'000'000, 10'000});
    }
    return speed;
}

va::ArchiveStatus sample_status(const va::Uuid& archive, std::string label)
{
    va::ArchiveStatus status;
    status.archive.archive_uuid = archive;
    status.archive.label = std::move(label);
    status.archive.status = "REGISTERED";
    va::ManifestHeader header;
    header.identity.volume_label = "RECORDED-LABEL";
    status.header = header;
    return status;
}

void names_and_filter()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto without_header = sample_status(second_archive, "ARCH-SAMPLE-2");
    without_header.header.reset();
    const auto names =
        va::gui::archive_names({sample_status(first_archive, "ARCH-SAMPLE-1"), without_header}, IdentityShown::Sample);
    CHECK((names == std::vector<va::gui::ArchiveName>{
                        {first_archive, "ARCH-SAMPLE-1", "SAMPLE-VOLUME"}, {second_archive, "ARCH-SAMPLE-2", ""}}));
    CHECK(va::gui::filter_words(english, names[0]) == "ARCH-SAMPLE-1 · SAMPLE-VOLUME");
    CHECK(va::gui::filter_words(chinese, names[0]) == "ARCH-SAMPLE-1・SAMPLE-VOLUME");
    CHECK(va::gui::filter_words(english, names[1]) == "ARCH-SAMPLE-2");
    // A list to choose an archive from adds the start of its uuid: two
    // archives of one label and volume label (a clone) read apart there.
    CHECK(va::gui::choice_words(english, names[0]) == "ARCH-SAMPLE-1 · SAMPLE-VOLUME · c0ffee00");
    CHECK(va::gui::choice_words(chinese, names[1]) == "ARCH-SAMPLE-2・c0ffee00");
    const va::gui::ArchiveName twin{"c0ffee01-0000-4000-8000-000000000003", "ARCH-SAMPLE-1", "SAMPLE-VOLUME"};
    CHECK(va::gui::choice_words(english, twin) != va::gui::choice_words(english, names[0]));
    CHECK(va::gui::archive_names({sample_status(first_archive, "ARCH-SAMPLE-1")}, IdentityShown::Recorded)
              .front()
              .volume_label == "RECORDED-LABEL");
}

void rows()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const std::vector<va::gui::ArchiveName> names{{first_archive, "ARCH-SAMPLE-1", "SAMPLE-VOLUME"}};
    nlohmann::json slower{{"items", nlohmann::json::object()},
        {"read_compared", {{"baseline", "c0ffee00-0000-4000-9000-000000000000"}, {"baseline_finished", ago(24h * 60)},
                              {"bytes_per_second", 1}, {"baseline_bytes_per_second", 2}, {"whole_compared", true},
                              {"slower_percent", 50}, {"slower_parts", nlohmann::json::array()}, {"alert", true}}}};
    const std::vector<va::VerifyRunRecord> runs{
        sample_run("c0ffee00-0000-4000-9000-000000000011", first_archive, "PASSED", 24h * 10),
        sample_run("c0ffee00-0000-4000-9000-000000000012", second_archive, "FAILED", 24h * 5, slower),
        sample_run("c0ffee00-0000-4000-9000-000000000013", first_archive, "INCOMPLETE", 24h * 2,
            {{"items", nlohmann::json::object()}, {"read_speed", va::read_speed_json(half_read())},
                {"relocation", "c0ffee00-0000-4000-9000-0000000000ff"}}),
    };

    // Every archive's, newest first; an archive no name is known for by the
    // start of its uuid.
    auto shown = va::gui::history_rows(english, runs, names, std::nullopt);
    CHECK(shown.size() == 3 && shown[0].run_id == runs[2].run_id && shown[2].run_id == runs[0].run_id);
    if (shown.size() == 3) {
        CHECK((shown[0].result == Pill{"FULL VERIFY INCOMPLETE", Tone::Neutral}));
        CHECK(shown[0].title == "ARCH-SAMPLE-1 · " + va::app::local_time(runs[2].finished, english));
        CHECK(shown[0].meta == "Stopped at 50% · 1.0 MiB · 5 min 37 s · 95.4 MiB/s");
        CHECK((shown[0].marks == std::vector<Pill>{{"interrupted relocation attempt", Tone::Neutral}}));
        CHECK((shown[1].result == Pill{"FULL VERIFY FAILED", Tone::Error}) && shown[1].title.starts_with("c0ffee00 · "));
        CHECK((shown[1].marks == std::vector<Pill>{{"⚠ reading slower", Tone::Error}}));
        CHECK(shown[2].meta == "1.0 MiB · 5 min 37 s" && shown[2].marks.empty());
    }
    // The filter, and a page at a time.
    shown = va::gui::history_rows(english, runs, names, first_archive);
    CHECK(shown.size() == 2 && shown[0].run_id == runs[2].run_id && shown[1].run_id == runs[0].run_id);
    shown = va::gui::history_rows(english, runs, names, std::nullopt, 1, 1);
    CHECK(shown.size() == 1 && shown[0].run_id == runs[1].run_id);
    shown = va::gui::history_rows(english, runs, names, first_archive, 1, 5);
    CHECK(shown.size() == 1 && shown[0].run_id == runs[0].run_id);
    CHECK(va::gui::history_count(runs, std::nullopt) == 3 && va::gui::history_count(runs, first_archive) == 2 &&
          va::gui::history_count(runs, "c0ffee00-0000-4000-8000-0000000000ff") == 0);
    shown = va::gui::history_rows(chinese, runs, names, std::nullopt, 0, 1);
    CHECK(shown.size() == 1 && shown[0].result.words == "完整驗證 未完成" &&
          shown[0].meta == "停止於 50%・1.0 MiB・5 分 37 秒・每秒 95.4 MiB" &&
          shown[0].marks.size() == 1 && shown[0].marks[0].words == "中斷的搬移嘗試");

    CHECK(va::gui::no_runs_words(english, std::nullopt) == "No verification is recorded yet.");
    CHECK(va::gui::no_runs_words(chinese, first_archive) == "從未驗證。");
}

void chain()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const std::vector<va::gui::ArchiveName> names{{first_archive, "ARCH-SAMPLE-1", "SAMPLE-VOLUME"}};
    const std::vector<va::VerifyRunRecord> runs{
        sample_run("c0ffee00-0000-4000-9000-000000000011", first_archive, "PASSED", 24h * 10),
        sample_run("c0ffee00-0000-4000-9000-000000000012", first_archive, "PASSED", 24h * 5),
        sample_run("c0ffee00-0000-4000-9000-000000000013", second_archive, "FAILED", 24h * 2),
    };
    va::HistoryCheck intact;
    intact.runs = 3;
    auto shown = va::gui::chain_words(english, intact, runs, names);
    CHECK(shown.tone == Tone::Success &&
          shown.result == "The history of every archive is intact: 3 verification runs, hash chain verified.");
    CHECK(shown.limits.starts_with("Checking the chain detects"));
    intact.runs = 1;
    CHECK(va::gui::chain_words(chinese, intact, runs, names).result ==
          "全部封存的歷程完整：共 1 次驗證執行，雜湊鏈已驗證。");

    // BROKEN: the run named by its archive and when it ended, which History
    // lists; by its id alone where the history read does not hold it.
    va::HistoryCheck broken;
    broken.runs = 3;
    broken.first_broken_run = "c0ffee00-0000-4000-9000-000000000012";
    broken.problem = "row was edited (row_hash does not match its contents)";
    shown = va::gui::chain_words(chinese, broken, runs, names);
    CHECK(shown.tone == Tone::Error &&
          shown.result == "歷程於執行 c0ffee00-0000-4000-9000-000000000012（ARCH-SAMPLE-1・" +
                              va::app::local_time(runs[1].finished, chinese) +
                              "）處斷裂：此筆紀錄已被修改（row_hash 與其內容不符）");
    CHECK(shown.limits.starts_with("檢查歷程鏈可偵測"));
    broken.first_broken_run = "c0ffee00-0000-4000-9000-000000000013";
    CHECK(va::gui::chain_words(english, broken, runs, names).result ==
          "History BROKEN at run c0ffee00-0000-4000-9000-000000000013 (c0ffee00 · " +
              va::app::local_time(runs[2].finished, english) +
              "): row was edited (row_hash does not match its contents)");
    CHECK(va::gui::chain_words(english, broken, {}, names).result ==
          "History BROKEN at run c0ffee00-0000-4000-9000-000000000013: row was edited (row_hash does not match its "
          "contents)");
    const std::expected<va::HistoryCheck, va::IoError> failure(
        std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "sample failure"}));
    shown = va::gui::chain_words(english, failure, runs, names);
    CHECK(shown.tone == Tone::Error && shown.result == "Cannot check the history: sample failure" && shown.limits.empty());

    // A check that found it intact holds until another run is recorded; one
    // that found it BROKEN, or could not check it, until it is checked again.
    intact.runs = 3;
    CHECK(va::gui::chain_check_holds(intact, 3) && !va::gui::chain_check_holds(intact, 4));
    CHECK(va::gui::chain_check_holds(broken, 3) && va::gui::chain_check_holds(broken, 4));
    CHECK(va::gui::chain_check_holds(failure, 4));
}

} // namespace

void history_rows_tests()
{
    names_and_filter();
    rows();
    chain();
}
