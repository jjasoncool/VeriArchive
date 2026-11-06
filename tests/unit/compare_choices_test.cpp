// What the cross-check dialog offers and its job reports
// (src/gui/compare_choices.*, decisions 0020, 0046 and 0049): B's list with
// the copies of A first, the B it starts with, the A=B refusal, the report cut
// to its listed differences, its words for a difference that take neither
// archive for the original, each archive's last FULL verification beside it,
// and the toast. Every archive below is sample data
// made up for the test (ARCH-SAMPLE-* labels, c0ffee00-... uuids, sample
// paths); none is of a real archive.
#include "test_support.h"

#include "gui/compare_choices.h"

#include <chrono>
#include <string>
#include <vector>

namespace {

using va::app::Language;
using va::app::Text;
using namespace std::chrono_literals;

// The time the reports are worded at (sample).
const auto now = std::chrono::sys_seconds{std::chrono::sys_days{std::chrono::year{2026} / 11 / 6}} + 10h;

// A sample FULL verification of `revision` that ended at 2026-11-02 08:00 UTC.
va::VerifyRunRecord full_run(std::uint32_t revision, std::string result)
{
    va::VerifyRunRecord run;
    run.run_id = "c0ffee00-0000-4000-9000-0000000000a1";
    run.revision = revision;
    run.mode = "FULL";
    run.started = "2026-11-02T07:00:00Z";
    run.finished = "2026-11-02T08:00:00Z";
    run.result = std::move(result);
    return run;
}

const va::Uuid original = "c0ffee00-0000-4000-8000-000000000c01";
const va::Uuid backup = "c0ffee00-0000-4000-8000-000000000c02";
const va::Uuid unrelated = "c0ffee00-0000-4000-8000-000000000c03";
const va::Uuid source = "c0ffee00-0000-4000-8000-000000000c04";

va::ArchiveStatus status(const va::Uuid& uuid, std::string label, std::optional<va::Uuid> copy_of)
{
    va::ArchiveStatus shown;
    shown.archive.archive_uuid = uuid;
    shown.archive.label = label;
    shown.archive.current_revision = 1;
    shown.archive.status = "REGISTERED";
    va::ManifestHeader header;
    header.archive_uuid = uuid;
    header.label = std::move(label);
    header.copy_of = std::move(copy_of);
    shown.header = std::move(header);
    return shown;
}

// In Archives' order: the original (itself a copy of `source`), an unrelated
// archive, its backup, and the source.
std::vector<va::ArchiveStatus> statuses()
{
    return {
        status(original, "ARCH-SAMPLE-1", source),
        status(unrelated, "ARCH-SAMPLE-2", std::nullopt),
        status(backup, "ARCH-SAMPLE-3", original),
        status(source, "ARCH-SAMPLE-4", std::nullopt),
    };
}

void copies_come_first()
{
    const auto all = statuses();
    // The archives recorded as its copies and the one it is a copy of, in
    // Archives' order, then the others, A itself included.
    CHECK(va::gui::copy_candidates(all, original) == (std::vector<va::Uuid>{backup, source, original, unrelated}));
    CHECK(va::gui::copy_candidates(all, unrelated) == (std::vector<va::Uuid>{original, unrelated, backup, source}));
    CHECK(va::gui::copy_candidates(all, backup) == (std::vector<va::Uuid>{original, unrelated, backup, source}));

    // B starts as a copy of A, or as the B asked for where it is not A.
    CHECK(va::gui::default_copy(all, original) == backup);
    CHECK(va::gui::default_copy(all, original, unrelated) == unrelated);
    CHECK(va::gui::default_copy(all, original, original) == backup);
    CHECK(va::gui::default_copy(all, unrelated) == original);
    CHECK(va::gui::default_copy(all, backup) == original);
    CHECK(!va::gui::default_copy({status(original, "ARCH-SAMPLE-1", std::nullopt)}, original));
}

void the_same_archive_is_refused()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::comparison_refusal(english, original, original) ==
          "A and B are the same archive; choose another archive as B.");
    CHECK(va::gui::comparison_refusal(chinese, original, original) == "A 與 B 是同一個封存；請選擇另一個封存作為 B。");
    CHECK(va::gui::comparison_refusal(english, original, std::nullopt) == "Choose the two archives to cross-check.");
    CHECK(!va::gui::comparison_refusal(english, original, backup));
}

void the_report_lists_at_most_its_differences()
{
    va::ArchiveComparison comparison;
    comparison.archive_a = original;
    comparison.archive_b = backup;
    comparison.label_a = "ARCH-SAMPLE-1";
    comparison.label_b = "ARCH-SAMPLE-3";
    comparison.revision_a = 1;
    comparison.revision_b = 2;
    comparison.created_a = "2026-11-01T08:00:00Z";
    comparison.created_b = "2026-11-02T08:00:00Z";
    comparison.entries_a = 2600;
    comparison.entries_b = 100;
    comparison.result = "DIFFERENT";
    for (int index = 0; index < 2500; ++index) {
        comparison.differences.push_back({"sample/file-" + std::to_string(index) + ".dat", "", "Missing", ""});
    }
    comparison.counts = {{"Missing", 2500}};
    const auto shown = va::gui::shown_comparison(comparison);
    CHECK(shown.differences == 2500 && shown.comparison.differences.size() == va::gui::listed_differences);

    const Text english;
    const auto report = va::gui::comparison_report(english, shown, {}, {}, now);
    CHECK(report.starts_with("Cross-check: DIFFERENT\n"));
    CHECK(report.contains("\nA: ARCH-SAMPLE-1 (" + original + ") revision 1, recorded "));
    CHECK(report.contains("\nB: ARCH-SAMPLE-3 (" + backup + ") revision 2, recorded "));
    // Neither archive is taken for the original: an entry only A has.
    CHECK(report.contains("Differences: 2500  Only in A 2500\n"));
    CHECK(report.contains("Only in A  sample/file-1999.dat\n") && !report.contains("sample/file-2000.dat"));
    CHECK(report.contains("... and 500 more differences, not listed here\n"));
    CHECK(report.contains("\nA: never FULL verified\nB: never FULL verified\n"));
    // Different contents: no word on what the media hold.
    CHECK(!report.contains("To show that the media hold this content"));
    CHECK(report.contains("A cross-check compares what each registration read; it reads no media."));

    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::comparison_report(chinese, shown, {}, {}, now).starts_with("核對完整性：不同\n"));
    CHECK(va::gui::comparison_failure_report(english, {"Cancelled", "the comparison was cancelled"}) ==
          "Not compared: the comparison was cancelled\n");
}

void differences_take_no_side()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::difference_words(english, "Missing") == "Only in A");
    CHECK(va::gui::difference_words(english, "New") == "Only in B");
    CHECK(va::gui::difference_words(english, "Modified") == "Differs");
    CHECK(va::gui::difference_words(chinese, "Missing") == "只在 A");
    CHECK(va::gui::difference_words(chinese, "New") == "只在 B");
    CHECK(va::gui::difference_words(chinese, "Modified") == "兩邊不同");
}

// Decision 0049: each archive's last FULL verification beside a cross-check,
// and what they show together where the two hold the same content.
void the_report_says_what_the_media_held()
{
    va::ArchiveComparison comparison;
    comparison.archive_a = original;
    comparison.archive_b = backup;
    comparison.label_a = "ARCH-SAMPLE-1";
    comparison.label_b = "ARCH-SAMPLE-3";
    comparison.revision_a = 1;
    comparison.revision_b = 2;
    comparison.created_a = "2026-11-01T08:00:00Z";
    comparison.created_b = "2026-11-02T08:00:00Z";
    comparison.entries_a = 6;
    comparison.entries_b = 6;
    comparison.same = 6;
    comparison.result = "IDENTICAL";
    const auto shown = va::gui::shown_comparison(comparison);
    const Text english;
    const Text chinese(Language::TraditionalChinese);

    // Both passed a FULL verification of the revision cross-checked, neither
    // OVERDUE: both media held it then.
    const va::gui::VerifiedSide a{full_run(1, "PASSED"), false};
    const va::gui::VerifiedSide b{full_run(2, "PASSED_WITH_WARNINGS"), false};
    auto report = va::gui::comparison_report(english, shown, a, b, now);
    CHECK(report.contains("A and B hold the same entries, each with the same type and content.\n"));
    CHECK(report.contains("\nA: FULL VERIFY PASSED, 4 days ago, "));
    CHECK(report.contains("\nB: FULL VERIFY PASSED_WITH_WARNINGS, 4 days ago, "));
    CHECK(report.contains("Both media held this content at their last FULL verifications, neither of which is "
                          "overdue.\n"));
    CHECK(va::gui::comparison_report(chinese, shown, a, b, now).contains("\nA：完整驗證 通過，4 天前，"));

    // B's last FULL verification is of an earlier revision, and A is
    // OVERDUE: a FULL verification of each would show it.
    const va::gui::VerifiedSide stale_a{full_run(1, "PASSED"), true};
    const va::gui::VerifiedSide earlier_b{full_run(1, "PASSED"), false};
    report = va::gui::comparison_report(english, shown, stale_a, earlier_b, now);
    CHECK(report.contains("\nA: FULL VERIFY PASSED, 4 days ago, ") && report.contains(" · OVERDUE\n"));
    CHECK(report.contains("\nB: no FULL verification of revision 2 yet; the last, of revision 1: FULL VERIFY PASSED, "
                          "4 days ago, "));
    CHECK(report.contains("To show that the media hold this content, FULL verify ARCH-SAMPLE-1 and ARCH-SAMPLE-3.\n"));
    CHECK(va::gui::comparison_report(chinese, shown, stale_a, earlier_b, now)
              .contains("要證明媒體保有這些內容，請完整驗證 ARCH-SAMPLE-1 與 ARCH-SAMPLE-3。\n"));

    // A FAILED one shows nothing; one side never verified is named alone.
    const va::gui::VerifiedSide failed_a{full_run(1, "FAILED"), false};
    report = va::gui::comparison_report(english, shown, failed_a, b, now);
    CHECK(report.contains("To show that the media hold this content, FULL verify ARCH-SAMPLE-1.\n"));
    report = va::gui::comparison_report(english, shown, a, {}, now);
    CHECK(report.contains("\nB: never FULL verified\n"));
    CHECK(report.contains("To show that the media hold this content, FULL verify ARCH-SAMPLE-3.\n"));
}

void toasts_say_the_result()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::compared_toast(english, "Cross-check ARCH-SAMPLE-1 and ARCH-SAMPLE-3", "IDENTICAL") ==
          "Cross-check ARCH-SAMPLE-1 and ARCH-SAMPLE-3: IDENTICAL");
    CHECK(va::gui::compared_toast(chinese, "核對 ARCH-SAMPLE-1 與 ARCH-SAMPLE-3 的完整性", "DIFFERENT") ==
          "核對 ARCH-SAMPLE-1 與 ARCH-SAMPLE-3 的完整性：不同");
    CHECK(va::gui::not_compared_toast(chinese, "核對 A 與 B 的完整性", true) == "核對 A 與 B 的完整性：已停止");
    CHECK(va::gui::not_compared_toast(chinese, "核對 A 與 B 的完整性", false) == "核對 A 與 B 的完整性：未能核對");
}

} // namespace

void compare_choices_tests()
{
    copies_come_first();
    the_same_archive_is_refused();
    the_report_lists_at_most_its_differences();
    differences_take_no_side();
    the_report_says_what_the_media_held();
    toasts_say_the_result();
}
