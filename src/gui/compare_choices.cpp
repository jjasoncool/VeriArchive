#include "gui/compare_choices.h"

#include "app/local_time.h"
#include "app/plain_words.h"

#include <algorithm>

namespace va::gui {
namespace {

constexpr const char* note =
    "This compares what each registration read. FULL verification of each archive proves that its "
    "media still holds it.";

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
        return text("Choose the two archives to compare.");
    }
    if (*reference == *copy) {
        return text("A and B are the same archive; choose the copy to check as B.");
    }
    return std::nullopt;
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

std::string comparison_report(const app::Text& text, const ComparisonShown& shown)
{
    const auto& comparison = shown.comparison;
    std::string body = text.format("ARCHIVES {}", text(comparison.result)) + "\n";
    if (comparison.result == "IDENTICAL") {
        body += text("Every entry of A is in B with the same type and content, and B has no other entries.") + "\n";
    } else if (comparison.result == "IDENTICAL_WITH_WARNINGS") {
        body += text("No entry differs, but the entries listed below are not proven the same.") + "\n";
    }
    body += "\n" +
            text.format("A (reference): {} ({}) revision {}, recorded {}", comparison.label_a, comparison.archive_a,
                comparison.revision_a, app::local_time(comparison.created_a, text)) +
            "\n" +
            text.format("B (copy): {} ({}) revision {}, recorded {}", comparison.label_b, comparison.archive_b,
                comparison.revision_b, app::local_time(comparison.created_b, text)) +
            "\n" +
            text.format("Entries: {} in A, {} in B, {} the same", comparison.entries_a, comparison.entries_b,
                comparison.same) +
            "\n" + text.format("Differences: {}", shown.differences);
    for (const auto& [status, count] : comparison.counts) {
        body += "  " + text(status) + " " + std::to_string(count);
    }
    body += "\n";
    if (!comparison.differences.empty()) {
        body += "\n";
        for (const auto& difference : comparison.differences) {
            // "path:stream" names an alternate data stream, as Windows writes it.
            body += text(difference.status) + "  " + difference.path +
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
