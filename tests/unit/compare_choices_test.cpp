// What the comparison dialog offers and its job reports
// (src/gui/compare_choices.*, decisions 0020 and 0046): B's list with the
// copies of A first, the B it starts with, the A=B refusal, the report cut to
// its listed differences, and the toast. Every archive below is sample data
// made up for the test (ARCH-SAMPLE-* labels, c0ffee00-... uuids, sample
// paths); none is of a real archive.
#include "test_support.h"

#include "gui/compare_choices.h"

#include <string>
#include <vector>

namespace {

using va::app::Language;
using va::app::Text;

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
          "A and B are the same archive; choose the copy to check as B.");
    CHECK(va::gui::comparison_refusal(chinese, original, original) == "A 與 B 是同一個封存；請將要檢查的副本選為 B。");
    CHECK(va::gui::comparison_refusal(english, original, std::nullopt) == "Choose the two archives to compare.");
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
    const auto report = va::gui::comparison_report(english, shown);
    CHECK(report.starts_with("ARCHIVES DIFFERENT\n"));
    CHECK(report.contains("\nA (reference): ARCH-SAMPLE-1 (" + original + ") revision 1, recorded "));
    CHECK(report.contains("\nB (copy): ARCH-SAMPLE-3 (" + backup + ") revision 2, recorded "));
    CHECK(report.contains("Differences: 2500  Missing 2500\n"));
    CHECK(report.contains("Missing  sample/file-1999.dat\n") && !report.contains("sample/file-2000.dat"));
    CHECK(report.contains("... and 500 more differences, not listed here\n"));
    CHECK(report.contains("FULL verification of each archive proves that its media still holds it."));

    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::comparison_report(chinese, shown).starts_with("比較結果：不同\n"));
    CHECK(va::gui::comparison_failure_report(english, {"Cancelled", "the comparison was cancelled"}) ==
          "Not compared: the comparison was cancelled\n");
}

void toasts_say_the_result()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::gui::compared_toast(english, "Compare ARCH-SAMPLE-1 with ARCH-SAMPLE-3", "IDENTICAL") ==
          "Compare ARCH-SAMPLE-1 with ARCH-SAMPLE-3: IDENTICAL");
    CHECK(va::gui::compared_toast(chinese, "比較 ARCH-SAMPLE-1 與 ARCH-SAMPLE-3", "DIFFERENT") ==
          "比較 ARCH-SAMPLE-1 與 ARCH-SAMPLE-3：不同");
    CHECK(va::gui::not_compared_toast(chinese, "比較 A 與 B", true) == "比較 A 與 B：已停止");
    CHECK(va::gui::not_compared_toast(chinese, "比較 A 與 B", false) == "比較 A 與 B：未能比較");
}

} // namespace

void compare_choices_tests()
{
    copies_come_first();
    the_same_archive_is_refused();
    the_report_lists_at_most_its_differences();
    toasts_say_the_result();
}
