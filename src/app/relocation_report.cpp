#include "app/relocation_report.h"

#include "app/device_alerts.h"
#include "app/plain_words.h"
#include "common/utf8.h"

namespace va::app {
namespace {

// An item as the command line lists it: its status, its path and its detail,
// in plain words as a verification lists it (decision 0017).
std::string item_line(const Text& text, const VerifyItemRecord& item)
{
    return "  " + text(item.status) + "  " + item.path.p +
           (item.detail.empty() ? "" : "  " + text.parenthesized(text(item_detail_words(item.detail)))) + "\n";
}

} // namespace

std::string relocation_report(const Text& text, const RelocateOutcome& outcome, const std::string& root)
{
    std::string report = text.format("Relocated {} ({}) to {} as revision {}", outcome.label, outcome.archive_uuid,
                             root, outcome.revision) +
                         "\n  " +
                         text.format("FULL VERIFY {} of the new carrier (run {})", text(outcome.result),
                             outcome.run_id) +
                         "\n";
    // Decision 0044: a device alert of the new carrier, beside the result.
    for (const auto& line : health_alert_lines(text, outcome.devices)) {
        report += "  " + line + "\n";
    }
    for (const auto& device : outcome.devices) {
        report += "  " + text.format("device health: {}", device_health_words(text, device)) + "\n";
    }
    // A file whose whole-file hash or chunk hashes cannot be relied on, as a
    // FULL verify warns of it (decision 0032).
    for (const auto& warning : outcome.warnings) {
        report += "  " + text.format("warning: {}", text(warning)) + "\n";
    }
    for (const auto& item : outcome.items) {
        report += item_line(text, item);
    }
    return report;
}

nlohmann::json relocation_json(const RelocateOutcome& outcome)
{
    return {
        {"archive_uuid", outcome.archive_uuid},
        {"revision", outcome.revision},
        {"result", outcome.result},
        {"run_id", outcome.run_id},
        {"manifest", path_to_utf8(outcome.manifest)},
        {"counts", outcome.counts},
        {"warnings", outcome.warnings},
        {"device_health", device_health_json(outcome.devices)},
        {"resumed", outcome.resumed},
    };
}

std::string relocation_refusal(const Text& text, const RelocateFailure& failure)
{
    std::string refusal = text.format("RELOCATION REFUSED: {}", text(failure.message)) + "\n";
    // Compared by chunk, a file whose content differs names the chunks that
    // differ (decision 0032).
    for (const auto& item : failure.items) {
        refusal += item_line(text, item);
    }
    return refusal;
}

} // namespace va::app
