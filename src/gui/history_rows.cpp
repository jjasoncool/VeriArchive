#include "gui/history_rows.h"

#include "app/device_alerts.h"
#include "app/history.h"
#include "app/local_time.h"
#include "core/engine/device_alerts.h"
#include "gui/report.h"
#include "gui/run_result.h"

#include <algorithm>
#include <map>

namespace va::gui {

std::vector<ArchiveName> archive_names(const std::vector<ArchiveStatus>& statuses, IdentityShown shown)
{
    std::vector<ArchiveName> names;
    for (const auto& status : statuses) {
        names.push_back({status.archive.archive_uuid, display_label(status),
            status.header ? displayed_identity(status.header->identity, shown).volume_label : std::string{}});
    }
    return names;
}

std::string filter_words(const app::Text& text, const ArchiveName& name)
{
    return name.volume_label.empty() ? name.label : text.format("{} · {}", name.label, name.volume_label);
}

std::size_t history_count(const std::vector<VerifyRunRecord>& runs, const std::optional<Uuid>& only)
{
    if (!only) {
        return runs.size();
    }
    return static_cast<std::size_t>(std::ranges::count(runs, *only, &VerifyRunRecord::archive_uuid));
}

std::vector<HistoryRow> history_rows(const app::Text& text, const std::vector<VerifyRunRecord>& runs,
    const std::vector<ArchiveName>& names, const std::optional<Uuid>& only, std::size_t from, std::size_t limit)
{
    std::map<Uuid, std::string> labels;
    for (const auto& name : names) {
        labels[name.archive_uuid] = name.label;
    }
    std::vector<HistoryRow> rows;
    std::size_t listed = 0;  // the runs of the filter passed so far
    for (auto run = runs.rbegin(); run != runs.rend() && rows.size() < limit; ++run) {
        if (only && run->archive_uuid != *only) {
            continue;
        }
        if (listed++ < from) {
            continue;
        }
        HistoryRow row;
        row.run_id = run->run_id;
        row.archive_uuid = run->archive_uuid;
        row.result = {result_with_mode(text, *run), result_tone(run->result)};
        const auto label = labels.find(run->archive_uuid);
        row.title = text.format("{} · {}",
            label != labels.end() ? label->second : run->archive_uuid.substr(0, 8), app::local_time(run->finished, text));
        std::vector<std::string> parts;
        if (run->result == "INCOMPLETE") {
            if (const auto percent = stopped_percent(*run, runs)) {
                parts.push_back(text.format("Stopped at {}%", *percent));
            }
        }
        parts.push_back(format_bytes(run->coverage_bytes.value_or(0)));
        parts.push_back(duration_words(text, run->started, run->finished));
        if (const auto speed = run_read_speed(*run, runs)) {
            parts.push_back(app::speed_words(text, *speed));
        }
        for (const auto& part : parts) {
            if (!part.empty()) {
                row.meta = row.meta.empty() ? part : text.format("{} · {}", row.meta, part);
            }
        }
        if (const auto note = app::history_note(text, *run); !note.empty()) {
            row.marks.push_back({note, Tone::Neutral});
        }
        for (const auto& mark : app::alert_marks(text, run_alerts(run->counts_json))) {
            row.marks.push_back({mark, Tone::Error});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string no_runs_words(const app::Text& text, const std::optional<Uuid>& only)
{
    return only ? text("Never verified.") : text("No verification is recorded yet.");
}

ChainShown chain_words(const app::Text& text, const std::expected<HistoryCheck, IoError>& check,
    const std::vector<VerifyRunRecord>& runs, const std::vector<ArchiveName>& names)
{
    ChainShown shown;
    if (!check) {
        shown.tone = Tone::Error;
        shown.result = text.format("Cannot check the history: {}", text(check.error().message));
        return shown;
    }
    shown.limits = text(app::history_chain_limits);
    if (check->first_broken_run) {
        shown.tone = Tone::Error;
        const auto& broken = *check->first_broken_run;
        const auto run = std::ranges::find(runs, broken, &VerifyRunRecord::run_id);
        if (run == runs.end()) {
            shown.result = text.format("History BROKEN at run {}: {}", broken, text(check->problem));
            return shown;
        }
        // Its archive and when it ended, as its row names them.
        const auto name = std::ranges::find(names, run->archive_uuid, &ArchiveName::archive_uuid);
        shown.result = text.format("History BROKEN at run {} ({} · {}): {}", broken,
            name != names.end() ? name->label : run->archive_uuid.substr(0, 8), app::local_time(run->finished, text),
            text(check->problem));
        return shown;
    }
    shown.tone = Tone::Success;
    // Two calls: the compiler checks each literal template against its value.
    shown.result =
        check->runs == 1
            ? text.format("The history of every archive is intact: {} verification run, hash chain verified.", check->runs)
            : text.format("The history of every archive is intact: {} verification runs, hash chain verified.",
                  check->runs);
    return shown;
}

bool chain_check_holds(const std::expected<HistoryCheck, IoError>& check, std::size_t runs)
{
    return !check || check->first_broken_run || check->runs == runs;
}

} // namespace va::gui
