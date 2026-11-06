#include "gui/compare_choices.h"

#include "app/local_time.h"
#include "app/plain_words.h"
#include "gui/archive_rows.h"

#include <algorithm>

namespace va::gui {
namespace {

constexpr const char* note = "A cross-check compares what each registration read; it reads no media.";

// What the report says of one archive's last FULL verification, against the
// revision cross-checked: its result with its mode, how long ago and when;
// that it is OVERDUE; or that there is none of that revision yet.
std::string verified_line(const app::Text& text, const std::string& name, std::uint32_t revision,
    const VerifiedSide& side, std::chrono::sys_seconds now)
{
    if (!side.last_full) {
        return text.format("{}: never FULL verified", name);
    }
    const auto& run = *side.last_full;
    const auto age = age_words(text, run.finished, now);
    const auto when = app::local_time(run.finished, text);
    std::string line =
        run.revision == revision
            ? text.format("{}: {}, {}, {}", name, result_with_mode(text, run), age, when)
            : text.format("{}: no FULL verification of revision {} yet; the last, of revision {}: {}, {}, {}", name,
                  revision, run.revision, result_with_mode(text, run), age, when);
    if (side.overdue) {
        line = text.format("{} · {}", line, text("OVERDUE"));
    }
    return line;
}

// Whether a FULL verification shows that the media of an archive held the
// revision cross-checked: one of that revision passed, and is not OVERDUE.
bool media_shown(const VerifiedSide& side, std::uint32_t revision)
{
    return side.last_full && side.last_full->revision == revision && !side.overdue &&
           result_tone(side.last_full->result) != Tone::Error && side.last_full->result != "INCOMPLETE";
}

// Whether `status` is recorded as a copy of `reference`, or `reference` as a
// copy of it.
bool copy_of_each_other(const ArchiveStatus& status, const ArchiveStatus* reference)
{
    const auto& uuid = status.archive.archive_uuid;
    if (!reference || uuid == reference->archive.archive_uuid) {
        return false;
    }
    const auto records = [](const ArchiveStatus& copy, const Uuid& original) {
        return copy.header && copy.header->copy_of && *copy.header->copy_of == original;
    };
    return records(status, reference->archive.archive_uuid) || records(*reference, uuid);
}

} // namespace

std::vector<Uuid> copy_candidates(const std::vector<ArchiveStatus>& statuses, const Uuid& reference)
{
    const auto found = std::ranges::find(statuses, reference,
        [](const ArchiveStatus& status) -> const Uuid& { return status.archive.archive_uuid; });
    const ArchiveStatus* a = found != statuses.end() ? &*found : nullptr;
    std::vector<Uuid> copies;
    std::vector<Uuid> others;
    for (const auto& status : statuses) {
        (copy_of_each_other(status, a) ? copies : others).push_back(status.archive.archive_uuid);
    }
    copies.insert(copies.end(), others.begin(), others.end());
    return copies;
}

std::optional<Uuid> default_copy(
    const std::vector<ArchiveStatus>& statuses, const Uuid& reference, const std::optional<Uuid>& wanted)
{
    const auto listed = copy_candidates(statuses, reference);
    if (wanted && *wanted != reference && std::ranges::find(listed, *wanted) != listed.end()) {
        return wanted;
    }
    // The copies come first: the first archive other than A is a copy of A
    // where it has one.
    for (const auto& archive : listed) {
        if (archive != reference) {
            return archive;
        }
    }
    return std::nullopt;
}

std::optional<std::string> comparison_refusal(
    const app::Text& text, const std::optional<Uuid>& reference, const std::optional<Uuid>& copy)
{
    if (!reference || !copy) {
        return text("Choose the two archives to cross-check.");
    }
    if (*reference == *copy) {
        return text("A and B are the same archive; choose another archive as B.");
    }
    return std::nullopt;
}

VerifiedSide verified_side(const ArchiveStatus& status)
{
    return {status.last_full, status.full_overdue || status.any_overdue};
}

std::string difference_words(const app::Text& text, const std::string& status)
{
    if (status == "Missing") {
        return text("Only in A");
    }
    if (status == "New") {
        return text("Only in B");
    }
    if (status == "Modified") {
        return text("Differs");
    }
    return text(status);
}

ComparisonShown shown_comparison(ArchiveComparison comparison)
{
    ComparisonShown shown{std::move(comparison), 0};
    auto& differences = shown.comparison.differences;
    shown.differences = differences.size();
    if (differences.size() > listed_differences) {
        differences.resize(listed_differences);
    }
    return shown;
}

std::string comparison_report(const app::Text& text, const ComparisonShown& shown, const VerifiedSide& a,
    const VerifiedSide& b, std::chrono::sys_seconds now)
{
    const auto& comparison = shown.comparison;
    std::string body = text.format("Cross-check: {}", text(comparison.result)) + "\n";
    const bool same = comparison.result == "IDENTICAL" || comparison.result == "IDENTICAL_WITH_WARNINGS";
    if (comparison.result == "IDENTICAL") {
        body += text("A and B hold the same entries, each with the same type and content.") + "\n";
    } else if (comparison.result == "IDENTICAL_WITH_WARNINGS") {
        body += text("No entry differs, but the entries listed below are not proven the same.") + "\n";
    }
    body += "\n" +
            text.format("A: {} ({}) revision {}, recorded {}", comparison.label_a, comparison.archive_a,
                comparison.revision_a, app::local_time(comparison.created_a, text)) +
            "\n" +
            text.format("B: {} ({}) revision {}, recorded {}", comparison.label_b, comparison.archive_b,
                comparison.revision_b, app::local_time(comparison.created_b, text)) +
            "\n" +
            text.format("Entries: {} in A, {} in B, {} the same", comparison.entries_a, comparison.entries_b,
                comparison.same) +
            "\n" + text.format("Differences: {}", shown.differences);
    for (const auto& [status, count] : comparison.counts) {
        body += "  " + difference_words(text, status) + " " + std::to_string(count);
    }
    body += "\n";
    if (!comparison.differences.empty()) {
        body += "\n";
        for (const auto& difference : comparison.differences) {
            // "path:stream" names an alternate data stream, as Windows writes it.
            body += difference_words(text, difference.status) + "  " + difference.path +
                    (difference.stream.empty() ? "" : ":" + difference.stream) +
                    (difference.detail.empty()
                            ? ""
                            : "  " + text.parenthesized(text(app::item_detail_words(difference.detail)))) +
                    "\n";
        }
        if (shown.differences > comparison.differences.size()) {
            const auto more = shown.differences - comparison.differences.size();
            // Two calls: the compiler checks each literal template against its value.
            body += (more == 1 ? text.format("... and {} more difference, not listed here", more)
                               : text.format("... and {} more differences, not listed here", more)) +
                    "\n";
        }
    }
    // Decision 0049: whether each archive's media held the content
    // cross-checked, as its last FULL verification tells.
    body += "\n" + text("The last FULL verification of each, which tells whether its media held this content then:") +
            "\n" + verified_line(text, "A", comparison.revision_a, a, now) + "\n" +
            verified_line(text, "B", comparison.revision_b, b, now) + "\n";
    if (same) {
        std::vector<std::string> unshown;
        if (!media_shown(a, comparison.revision_a)) {
            unshown.push_back(comparison.label_a);
        }
        if (!media_shown(b, comparison.revision_b)) {
            unshown.push_back(comparison.label_b);
        }
        if (unshown.empty()) {
            body += text("Both media held this content at their last FULL verifications, neither of which is "
                         "overdue.") +
                    "\n";
        } else {
            const auto names = unshown.size() == 1 ? unshown.front() : text.format("{} and {}", unshown[0], unshown[1]);
            body += text.format("To show that the media hold this content, FULL verify {}.", names) + "\n";
        }
    }
    body += "\n" + text(note) + "\n";
    return body;
}

std::string comparison_failure_report(const app::Text& text, const CompareFailure& failure)
{
    std::string body = text.format("Not compared: {}", text(failure.message)) + "\n";
    // Cancelling is the user's choice, not a fault of an archive or the repository.
    if (failure.failure_class != "Cancelled") {
        body += text.format("Failure: {}", text(app::failure_class_words(failure.failure_class))) + "\n";
    }
    return body;
}

std::string compared_toast(const app::Text& text, const std::string& title, const std::string& result)
{
    return text.format("{}: {}", title, text(result));
}

std::string not_compared_toast(const app::Text& text, const std::string& title, bool stopped)
{
    return stopped ? text.format("{}: {}", title, text("stopped")) : text.format("{}: {}", title, text("not compared"));
}

} // namespace va::gui
