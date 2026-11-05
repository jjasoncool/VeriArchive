#include "gui/run_result.h"

#include "app/device_alerts.h"
#include "app/enclosure_words.h"
#include "app/history.h"
#include "app/local_time.h"
#include "app/plain_words.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/comparison.h"
#include "core/engine/device_alerts.h"
#include "core/engine/relocate_engine.h"
#include "core/identity/volume_json.h"
#include "gui/report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string_view>

namespace va::gui {
namespace {

constexpr double mebibyte = 1024.0 * 1024.0;

nlohmann::json counts_of(const VerifyRunRecord& run)
{
    auto counts = nlohmann::json::parse(run.counts_json, nullptr, false);
    return counts.is_object() ? counts : nlohmann::json::object();
}

std::string text_member(const nlohmann::json& counts, const char* name)
{
    const auto found = counts.find(name);
    return found != counts.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::optional<std::uint64_t> unsigned_member(const nlohmann::json& counts, const char* name)
{
    const auto found = counts.find(name);
    if (found == counts.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

// The item counts of the run ("items"): status -> entries, in the order of
// the statuses' names, as the report lists them.
std::map<std::string, std::uint64_t> item_counts(const nlohmann::json& counts)
{
    std::map<std::string, std::uint64_t> result;
    const auto items = counts.find("items");
    if (items == counts.end() || !items->is_object()) {
        return result;
    }
    for (const auto& [status, count] : items->items()) {
        if (count.is_number_unsigned()) {
            result[status] = count.get<std::uint64_t>();
        }
    }
    return result;
}

const VerifyRunRecord* run_named(const std::vector<VerifyRunRecord>& runs, const std::string& run_id)
{
    const auto found = std::ranges::find(runs, run_id, &VerifyRunRecord::run_id);
    return found == runs.end() ? nullptr : &*found;
}

// The verification that `run` finished, as it read the content: its own read
// with that of the runs it resumed (whole_verification).
std::optional<ReadSpeed> whole_read(const VerifyRunRecord& run, const std::vector<VerifyRunRecord>& history)
{
    const auto own = recorded_read_speed(run.counts_json);
    if (!own) {
        return std::nullopt;
    }
    return whole_verification(*own, text_member(counts_of(run), "resumed_from"), history);
}

// Parts of a line, joined as the interface joins them ("a · b", a・b).
std::string joined(const app::Text& text, const std::vector<std::string>& parts)
{
    std::string line;
    for (const auto& part : parts) {
        if (!part.empty()) {
            line = line.empty() ? part : text.format("{} · {}", line, part);
        }
    }
    return line;
}

std::vector<Field> identity_fields(const app::Text& text, const RecordedRun& recorded, IdentityShown shown)
{
    std::vector<Field> fields;
    const auto& run = recorded.run;
    if (read_no_media(run.counts_json)) {
        // Decision 0028: it failed on its manifest before the media was looked for.
        fields.push_back(
            {text("Media identity"), text("not read, as the manifest of this revision cannot be used")});
        return fields;
    }
    // Decision 0047: a run that went on past a changed enclosure says how it
    // was confirmed, as its counts record it.
    if (const auto confirmed = carrier_confirmed(run)) {
        fields.push_back({text("Media identity"), app::confirmed_identity_words(text, confirmed->confirmation)});
    } else {
        fields.push_back({text("Media identity"),
            recorded.evidence && !recorded.evidence->match_result.empty() ? text(recorded.evidence->match_result)
                                                                          : text("not known")});
    }
    // The volume and its devices as the evidence holds them, the health each
    // reported as the run's counts record it, named as the views name them.
    const auto health = displayed_devices(recorded_device_health(run.counts_json), shown);
    std::vector<PhysicalDeviceInfo> devices;
    if (recorded.evidence) {
        const auto json = nlohmann::json::parse(recorded.evidence->volume_json, nullptr, false);
        if (json.is_object()) {
            const auto volume = displayed_identity(volume_from_json(json), shown);
            fields.push_back({text("Volume"), volume.volume_label.empty() ? text("(none)") : volume.volume_label});
            fields.push_back({text("Capacity"), format_bytes(volume.capacity_bytes)});
            devices = volume.devices;
        }
    }
    if (devices.empty()) {
        devices = health;
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        fields.push_back({text("Model"), device.model.empty() ? text("not known") : device.model});
        if (!device.bus.empty()) {
            fields.push_back({text("Bus"), bus_words(device.bus)});
        }
        // Decision 0047: the enclosure's, where it hid the device's own (as
        // the evidence records it), as Devices names it.
        const auto serial = device.enclosure_hides_identity ? text("Enclosure serial") : text("Serial");
        if (!device.serial.empty()) {
            fields.push_back({serial, device.serial});
        } else if (!device.wwn.empty()) {
            fields.push_back({text("WWN"), device.wwn});
        } else {
            fields.push_back({serial, text("not known")});
        }
        // The health that the device reported as the run started, where
        // the run recorded it: the devices of the counts are those of the
        // evidence, in the same order.
        if (index < health.size() && health[index].health) {
            fields.push_back({text("Health"), app::health_words(text, *health[index].health)});
        }
    }
    return fields;
}

std::vector<Field> technical_fields(
    const app::Text& text, const RecordedRun& recorded, const nlohmann::json& counts, IdentityShown shown)
{
    const auto& run = recorded.run;
    std::vector<Field> fields;
    fields.push_back({text("Run"), run.run_id});
    fields.push_back({text("Mode"), text(run.mode)});
    // Decision 0033: the revision that recorded the chunk hashes added, or
    // that the run added none.
    auto revision = std::to_string(run.revision);
    if (const auto from = unsigned_member(counts, "chunk_hashes_added_from_revision")) {
        revision = text.format("{}, which recorded the chunk hashes added to revision {}", run.revision, *from);
    } else if (unsigned_member(counts, adding_member).value_or(0) > 0) {
        revision = run.result == "INCOMPLETE" ? text.format("{}, adding chunk hashes", run.revision)
                                              : text.format("{}; no revision recorded the chunk hashes added", run.revision);
    }
    fields.push_back({text("Revision"), revision});
    if (const auto resumed = text_member(counts, "resumed_from"); !resumed.empty()) {
        fields.push_back({text("Resumed from"), resumed});
    }
    if (const auto note = app::history_note(text, run); !note.empty()) {
        fields.push_back({text("Note"), note});
    }
    fields.push_back({text("Started"), app::local_time(run.started, text)});
    fields.push_back({text("Finished"), app::local_time(run.finished, text)});
    if (const auto items = item_counts(counts); !items.empty()) {
        std::vector<std::string> parts;
        for (const auto& [status, count] : items) {
            parts.push_back(text(status) + " " + grouped(count));
        }
        fields.push_back({text("Entries by status"), joined(text, parts)});
    }
    if (run.coverage_bytes) {
        fields.push_back({text("Bytes matched"), app::count_of(text, *run.coverage_bytes, app::Counted::Byte)});
    }
    if (!run.failure_classes.empty()) {
        fields.push_back({text("Failures"), app::failure_classes_words(text, run.failure_classes)});
    }
    if (const auto host = displayed_host(run.host, shown); !host.empty()) {
        fields.push_back({text("Computer"), host});
    }
    if (!run.app_version.empty()) {
        fields.push_back({text("Program version"), run.app_version});
    }
    fields.push_back({text("Guarantee"), text(guarantee)});
    return fields;
}

} // namespace

bool changes_no_result(const VerifyItemRecord& item)
{
    return item.status == "SkippedMountPoint" && item.detail == mount_point_outside_archive;
}

bool fails_the_run(const VerifyItemRecord& item)
{
    if (item.detail == "known-since-registration" || changes_no_result(item)) {
        return false;  // decision 0010; R-30
    }
    return item.status != "Unbaselined" && item.status != "Unrepresentable" && item.status != "MetadataMismatch";
}

Tone item_tone(const VerifyRunRecord& run, const VerifyItemRecord& item, bool any_failing_item)
{
    if (changes_no_result(item)) {
        return Tone::Neutral;
    }
    if (run.result == "PASSED_WITH_WARNINGS") {
        return Tone::Warning;
    }
    if (fails_the_run(item)) {
        return Tone::Error;
    }
    // Strict metadata (R-51): its metadata differences failed the run.
    if (item.status == "MetadataMismatch" && run.result == "FAILED" && !any_failing_item &&
        run.failure_classes.empty()) {
        return Tone::Error;
    }
    return Tone::Warning;
}

std::string axis_words(double value, double step)
{
    int decimals = 0;
    for (double scaled = step; decimals < 6; ++decimals, scaled *= 10.0) {
        if (std::abs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, scaled)) {
            break;
        }
    }
    return std::format("{:.{}f}", value, decimals);
}

double axis_step(double most)
{
    if (!(most > 0.0)) {
        return 1.0;
    }
    const double raw = most / 4.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double multiple : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (multiple * magnitude >= raw) {
            return multiple * magnitude;
        }
    }
    return 10.0 * magnitude;
}

std::optional<SpeedChart> speed_chart(
    const ReadSpeed& now, const std::optional<ReadSpeed>& before, const std::vector<SlowerStretch>& slower)
{
    SpeedChart chart;
    double most = 0.0;
    // The time spent on the hundredths drawn.
    const auto fill = [&](const ReadSpeed& speed, std::array<std::optional<double>, 100>& into) {
        std::uint64_t timed = 0;
        for (const auto& part : speed.parts) {
            if (part.hundredth < 100 && part.bytes > 0 && part.milliseconds > 0) {
                const double value = static_cast<double>(part.bytes) * 1000.0 /
                                     static_cast<double>(part.milliseconds) / mebibyte;
                into[part.hundredth] = value;
                most = std::max(most, value);
                timed += part.milliseconds;
            }
        }
        return timed;
    };
    if (fill(now, chart.now) < chart_least_milliseconds) {
        return std::nullopt;
    }
    if (before) {
        chart.compared = fill(*before, chart.before) > 0;
    }
    chart.slower = slower;
    // A little room above the highest value, up to the next line.
    chart.step = axis_step(most * 1.04);
    chart.top = std::ceil(most * 1.04 / chart.step) * chart.step;
    return chart;
}

std::optional<std::uint64_t> run_read_speed(const VerifyRunRecord& run, const std::vector<VerifyRunRecord>& history)
{
    // As app::read_speed_words says it.
    if (const auto compared = recorded_speed_comparison(run.counts_json); compared && compared->bytes_per_second > 0) {
        return compared->bytes_per_second;
    }
    // Over the hundredths read in a measurable time, as the comparison and
    // the chart take them.
    const auto whole = whole_read(run, history);
    return whole ? whole->bytes_per_second() : std::nullopt;
}

std::optional<int> stopped_percent(const VerifyRunRecord& run, const std::vector<VerifyRunRecord>& history)
{
    const auto whole = whole_read(run, history);
    if (!whole || whole->content_bytes == 0) {
        return std::nullopt;
    }
    const auto percent = whole->bytes() * 100 / whole->content_bytes;
    return static_cast<int>(std::min<std::uint64_t>(percent, 99));
}

std::string grouped(std::uint64_t count)
{
    auto digits = std::to_string(count);
    for (auto at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

ResultPage result_page(const app::Text& text, const RecordedRun& recorded, IdentityShown shown)
{
    ResultPage page;
    const auto& run = recorded.run;
    const auto counts = counts_of(run);
    const auto finished = app::local_time(run.finished, text);
    page.title = text.format("{} · {}", recorded.label, finished);
    page.tone = result_tone(run.result);
    page.heading = result_with_mode(text, run);
    page.incomplete = run.result == "INCOMPLETE";
    const bool media_read = !read_no_media(run.counts_json);

    // The banner. The volume label of the carrier the run read, else of the
    // archive's current one.
    std::string volume_label = recorded.volume_label;
    if (recorded.evidence) {
        const auto json = nlohmann::json::parse(recorded.evidence->volume_json, nullptr, false);
        if (json.is_object()) {
            volume_label = displayed_identity(volume_from_json(json), shown).volume_label;
        }
    }
    page.archive = joined(text, {recorded.label, volume_label, finished});
    if (run.result == "PASSED") {
        page.lines.push_back(text("100% of the registered content was read and matched"));
    }
    if (!media_read) {
        page.lines.push_back(text("Media: not read, as the manifest of this revision cannot be used"));
    }
    for (std::size_t start = 0; start < run.failure_classes.size();) {
        const auto end = std::min(run.failure_classes.find(',', start), run.failure_classes.size());
        if (end > start) {
            page.lines.push_back(text.format("Failure: {}",
                text(app::failure_class_words(std::string_view(run.failure_classes).substr(start, end - start)))));
        }
        start = end + 1;
    }
    if (const auto note = app::history_note(text, run); !note.empty()) {
        page.lines.push_back(note);
    }
    if (page.incomplete) {
        const auto whole = whole_read(run, recorded.history);
        const auto percent = stopped_percent(run, recorded.history);
        if (whole && percent) {
            page.lines.push_back(text.format("Stopped at {}%: {} of {} read.", *percent, format_bytes(whole->bytes()),
                format_bytes(whole->content_bytes)));
        } else {
            page.lines.push_back(text.format("Stopped before it reached a result; {} read and matched in this run.",
                format_bytes(run.coverage_bytes.value_or(0))));
        }
        page.lines.push_back(text("A report cannot be saved for a verification that has not finished."));
    }

    // Decision 0044: the device alerts, as the run's report gave them.
    page.alerts = app::health_alert_lines(text, displayed_devices(recorded_device_health(run.counts_json), shown));
    const auto compared = recorded_speed_comparison(run.counts_json);
    if (compared) {
        if (const auto line = app::slower_alert_line(text, *compared)) {
            page.alerts.push_back(*line);
        }
    }

    // The items, in the words of the report; "path:stream" names an
    // alternate data stream, as Windows writes it. What failed the run
    // first, then what warns, then what changes no result, each in the order
    // of its paths: among thousands of warnings the item that failed a run
    // never lies many pages down.
    const bool any_failing = std::ranges::any_of(recorded.items, fails_the_run);
    for (const auto& item : recorded.items) {
        page.items.push_back({{text(item.status), item_tone(run, item, any_failing)},
            item.stream.empty() ? item.path.p : item.path.p + ":" + item.stream,
            item.detail.empty() ? std::string{} : text(app::item_detail_words(item.detail))});
    }
    std::ranges::stable_sort(page.items, {}, [](const ResultItem& item) {
        return item.status.tone == Tone::Error ? 0 : item.status.tone == Tone::Warning ? 1 : 2;
    });
    page.items_heading = text.format("Items ({})", grouped(recorded.items.size()));
    page.items_first = run.result == "FAILED" || run.result == "PASSED_WITH_WARNINGS";
    page.items_shown = !page.incomplete || !recorded.items.empty();
    if (recorded.items.empty()) {
        page.items_none = run.result == "PASSED" ? text("Every entry was verified.")
                                                 : text("No entry was recorded as differing.");
    }

    // The metrics.
    std::uint64_t entries = 0;
    for (const auto& [status, count] : item_counts(counts)) {
        entries += count;
    }
    const auto speed = run_read_speed(run, recorded.history);
    page.metrics = {
        {text("Read and matched"), format_bytes(run.coverage_bytes.value_or(0))},
        {text("Entries compared"), grouped(entries)},
        {text("Duration"), duration_words(text, run.started, run.finished)},
        {text("Average read speed"), speed ? app::speed_words(text, *speed) : std::string("—")},
    };

    // The read speed of a FULL verification that reached a result and read
    // media (decision 0044).
    page.speed_shown = !page.incomplete && run.mode == "FULL" && media_read &&
                       (recorded_read_speed(run.counts_json) || run.coverage_bytes.value_or(0) > 0 ||
                           relocation_part(run));
    if (page.speed_shown) {
        if (relocation_part(run)) {
            page.speed_lines.push_back(text("A relocation does not record its read speed."));
        } else if (const auto whole = whole_read(run, recorded.history); !whole) {
            page.speed_lines.push_back(text("The version that ran this verification did not record read speed yet."));
        } else {
            // The run compared with, as it read the content.
            std::optional<ReadSpeed> before;
            if (compared) {
                if (const auto* baseline = run_named(recorded.history, compared->baseline_run)) {
                    before = whole_read(*baseline, recorded.history);
                }
            }
            page.chart = speed_chart(*whole, before, compared ? compared->slower_parts : std::vector<SlowerStretch>{});
            if (!page.chart) {
                page.speed_lines.push_back(
                    text("This verification read too little to chart its read speed by position in the content."));
            }
            if (const auto words = app::read_speed_words(text, whole, compared)) {
                page.speed_lines.push_back(text.format("Read speed: {}", *words));
                if (!compared) {
                    page.speed_lines.push_back(text("No earlier FULL verification of this archive read the same "
                                                    "content on this device, so there is nothing to compare with."));
                }
            }
        }
    }

    page.identity = identity_fields(text, recorded, shown);
    page.technical = technical_fields(text, recorded, counts, shown);
    return page;
}

std::optional<app::OperationRow> stopped_run(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const VerifyRunRecord& run, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    if (run.result != "INCOMPLETE") {
        return std::nullopt;
    }
    for (const auto& watch : watches) {
        const auto& operation = watch.operation();
        if (operation.kind != app::OperationKind::Verification || operation.archive_uuid != run.archive_uuid ||
            operation.run_id != run.run_id) {
            continue;
        }
        // As its row under Unfinished operations offers Resume.
        auto row = app::describe_operation(text, watch, now, wall_now);
        if (row.offers_resume) {
            return row;
        }
    }
    return std::nullopt;
}

} // namespace va::gui
