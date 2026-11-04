#pragma once
// What the comparison dialog offers and its job reports (decision 0020,
// decision 0046): the archives that "Copy to check (B)" lists for the
// reference archive A, the copies of A first; which B it starts with; why A
// and B cannot be compared as chosen; and the report of a comparison, which
// the job's details show.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "core/engine/archive_status.h"
#include "core/engine/compare_archives.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

// The archives that B lists for A = `reference`: the copies of A first (the
// archives whose current revision records them as A's copies, and the one
// that A's records it as a copy of, decision 0005), then every other
// archive in the order of `statuses` (Archives' order), A itself included:
// choosing it is refused (comparison_refusal), never hidden.
std::vector<Uuid> copy_candidates(const std::vector<ArchiveStatus>& statuses, const Uuid& reference);

// The B that the dialog chooses for A: `wanted`, where it is listed and is
// not A; otherwise the first copy of A; otherwise the first archive other
// than A; nothing where there is none.
std::optional<Uuid> default_copy(const std::vector<ArchiveStatus>& statuses, const Uuid& reference,
    const std::optional<Uuid>& wanted = std::nullopt);

// Why A and B cannot be compared as chosen: A and B are the same archive,
// which the engine allows, but which here is almost surely a slip
// ("identical" would then say nothing about the copy meant); or one is not
// chosen. Nothing where they can.
std::optional<std::string> comparison_refusal(
    const app::Text& text, const std::optional<Uuid>& reference, const std::optional<Uuid>& copy);

// The report lists at most this many differences: a text view with millions
// of lines would make the window unusable. The counts above the list cover
// all.
inline constexpr std::size_t listed_differences = 2000;

// What a comparison's report shows: the comparison with its differences cut
// to the listed ones, so a report that stays on screen holds little memory,
// and how many there are in all.
struct ComparisonShown {
    ArchiveComparison comparison;
    std::size_t differences = 0;
};

ComparisonShown shown_comparison(ArchiveComparison comparison);

// The report: the result, what it means, A and B each with its revision and
// when that revision was recorded (decision 0041), the counts, the
// differences listed and how many more there are, and what a comparison does
// not prove.
std::string comparison_report(const app::Text& text, const ComparisonShown& shown);
std::string comparison_failure_report(const app::Text& text, const CompareFailure& failure);

// The toast once a comparison ended: its title and result, "Compare ARCH-0001
// with ARCH-0002: DIFFERENT"; stopped, or without a result.
std::string compared_toast(const app::Text& text, const std::string& title, const std::string& result);
std::string not_compared_toast(const app::Text& text, const std::string& title, bool stopped);

} // namespace va::gui
