#pragma once
// What the cross-check dialog offers and its job reports (decisions 0020,
// 0046 and 0049): the archives that "Archive B" lists for archive A, the
// recorded copies of A first; which B it starts with; why A and B cannot be
// cross-checked as chosen; and the report of a cross-check, which the job's
// details show. A cross-check is the window's word for a comparison
// (GLOSSARY: Comparison; 核對完整性 in Chinese). A and B stand side by side:
// neither is taken for the original, so a difference is an entry only in A,
// an entry only in B, or an entry that differs.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "core/engine/archive_status.h"
#include "core/engine/compare_archives.h"

#include <chrono>
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

// Why A and B cannot be cross-checked as chosen: A and B are the same
// archive, which the engine allows, but which here is almost surely a slip
// ("identical" would then say nothing); or one is not chosen. Nothing where
// they can.
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

// What the report says beside the cross-check of one archive's media
// (decision 0049): its last FULL verification that reached a result and
// whether the archive is OVERDUE, as Archives listed it when the job
// started. A cross-check reads no media; only a FULL verification of the
// revision cross-checked tells that its media held that content.
struct VerifiedSide {
    std::optional<VerifyRunRecord> last_full;
    bool overdue = false;
};

VerifiedSide verified_side(const ArchiveStatus& status);

// How the report names a difference of a cross-check, which takes neither
// archive for the original: an entry that only A has ("Missing" in the
// comparison), only B has ("New"), or that both have and differs
// ("Modified"); any other status as the engine names it.
std::string difference_words(const app::Text& text, const std::string& status);

// The report: the result, what it means, A and B each with its revision and
// when that revision was recorded (decision 0041), the counts, the
// differences listed and how many more there are; then each archive's last
// FULL verification as of `now`, and what they show together: where the two
// are the same and both media passed a FULL verification of the revision
// cross-checked that is not OVERDUE, that both held it then; otherwise which
// archives a FULL verification would have to show it of (decision 0049).
std::string comparison_report(const app::Text& text, const ComparisonShown& shown, const VerifiedSide& a,
    const VerifiedSide& b, std::chrono::sys_seconds now);
std::string comparison_failure_report(const app::Text& text, const CompareFailure& failure);

// The toast once a cross-check ended: its title and result, "Cross-check
// ARCH-0001 and ARCH-0002: DIFFERENT"; stopped, or without a result.
std::string compared_toast(const app::Text& text, const std::string& title, const std::string& result);
std::string not_compared_toast(const app::Text& text, const std::string& title, bool stopped);

} // namespace va::gui
