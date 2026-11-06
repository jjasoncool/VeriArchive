#include "gui/archive_rows.h"

#include "app/device_alerts.h"
#include "app/history.h"
#include "app/local_time.h"
#include "app/plain_words.h"
#include "common/time.h"
#include "core/engine/device_alerts.h"
#include "core/engine/read_speed.h"
#include "core/engine/recorded_manifest.h"
#include "gui/report.h"

#include <algorithm>

namespace va::gui {
namespace {

// Parts of a line, joined as the interface joins them: "a · b" (a・b).
std::string joined(const app::Text& text, const std::vector<std::string>& parts)
{
    std::string line;
    for (const auto& part : parts) {
        if (part.empty()) {
            continue;
        }
        line = line.empty() ? part : text.format("{} · {}", line, part);
    }
    return line;
}

std::string years_ago(const app::Text& text, long long years, long long months)
{
    // One call per wording: the compiler checks each literal template
    // against its values.
    if (months == 0) {
        return years == 1 ? text("1 year ago") : text.format("{} years ago", years);
    }
    if (years == 1) {
        return months == 1 ? text("1 year 1 month ago") : text.format("1 year {} months ago", months);
    }
    return months == 1 ? text.format("{} years 1 month ago", years)
                       : text.format("{} years {} months ago", years, months);
}

// The run whose result the archive's row and banner give: its last FULL
// verification, or where it has none, its last of another mode.
const std::optional<VerifyRunRecord>& shown_run(const ArchiveStatus& status)
{
    return status.last_full ? status.last_full : status.last_any;
}

// The result of `run` as a row and a banner say it of an archive: alone for
// a FULL verification, with its mode for another.
std::string result_words(const app::Text& text, const VerifyRunRecord& run)
{
    return run.mode == "FULL" ? text(run.result) : result_with_mode(text, run);
}

} // namespace

const char* tone_class(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return "va-success";
    case Tone::Warning:
        return "va-warning";
    case Tone::Error:
        return "va-error";
    case Tone::Neutral:
        break;
    }
    return "va-neutral";
}

std::string age_words(const app::Text& text, std::string_view then, std::chrono::sys_seconds now)
{
    const auto moment = parse_timestamp(then);
    if (!moment) {
        return {};
    }
    const auto seconds = (now - *moment).count();
    if (seconds < 3600) {
        return text("less than an hour ago");
    }
    if (seconds < 86400) {
        const auto hours = seconds / 3600;
        return hours == 1 ? text("1 hour ago") : text.format("{} hours ago", hours);
    }
    const auto days = seconds / 86400;
    if (days < 14) {
        return days == 1 ? text("1 day ago") : text.format("{} days ago", days);
    }
    if (days < 60) {
        return text.format("{} weeks ago", days / 7);
    }
    // Never "12 months": the last days of a year are 11 months, as 360 days
    // and more are 12 months of 30 days.
    if (days < 365) {
        return text.format("{} months ago", std::min<long long>(days / 30, 11));
    }
    return years_ago(text, days / 365, std::min<long long>((days % 365) / 30, 11));
}

Pill result_pill(const app::Text& text, const VerifyRunRecord& run)
{
    return {text(run.result), result_tone(run.result)};
}

ArchiveRow archive_row(const app::Text& text, const ArchiveStatus& status, IdentityShown shown,
    std::chrono::sys_seconds now)
{
    ArchiveRow row;
    row.archive_uuid = status.archive.archive_uuid;
    row.label = display_label(status);
    std::vector<std::string> parts;
    if (status.header) {
        // The carrier as the latest revision recorded it, or the sample
        // identity in the screenshot mode.
        const auto identity = displayed_identity(status.header->identity, shown);
        row.volume_label = identity.volume_label;
        if (!identity.devices.empty()) {
            parts.push_back(identity.devices.front().model);
        }
        parts.push_back(format_bytes(identity.capacity_bytes));
    }
    if (status.last_full) {
        parts.push_back(text.format("Last FULL verify {}", age_words(text, status.last_full->finished, now)));
    }
    row.subtitle = joined(text, parts);

    const auto& run = shown_run(status);
    row.result = run ? Pill{result_words(text, *run), result_tone(run->result)} : Pill{text("Never verified")};
    const bool overdue = status.full_overdue || status.any_overdue;
    if (overdue) {
        row.marks.push_back({text("OVERDUE"), Tone::Warning});
    }
    // Decision 0044: a device alert of the last verification shows beside
    // it, as a failure does, until a later one gives none.
    const auto alerts = app::archive_alerts(status);
    for (const auto& mark : app::alert_marks(text, alerts)) {
        row.marks.push_back({mark, Tone::Error});
    }
    // The archive's status where it is not REGISTERED, as `archive list`
    // shows it: RETIRED (已停用), REGISTRATION_INCOMPLETE (註冊未完成), the
    // first registration of an archive that has not finished.
    if (status.archive.status != "REGISTERED") {
        row.marks.push_back({text(status.archive.status), Tone::Neutral});
    }

    // Decision 0028: an archive whose latest manifest cannot be used is
    // listed like the others, with what is wrong and how to fix it. A
    // retired archive whose latest manifest is lost shows as retired with
    // its manifest lost (point 9).
    if (status.unusable_manifest) {
        const auto message = text(plain_message(*status.unusable_manifest));
        row.problem =
            status.manifest_lost ? text.format("Manifest lost: {}", message) : text.format("Problem: {}", message);
        row.problem_alerts = !status.manifest_lost;
    }

    const bool failed = status.last_any && status.last_any->result == "FAILED";
    if (failed || alerts.any() || row.problem_alerts || (run && result_tone(run->result) == Tone::Error)) {
        row.tone = Tone::Error;
    } else if (!run) {
        row.tone = overdue ? Tone::Warning : Tone::Neutral;
    } else if (overdue || result_tone(run->result) == Tone::Warning) {
        row.tone = Tone::Warning;
    } else {
        row.tone = Tone::Success;
    }
    return row;
}

ResultBanner result_banner(const app::Text& text, const ArchiveStatus& status, const Policy& policy,
    std::chrono::sys_seconds now)
{
    ResultBanner banner;
    const auto& run = shown_run(status);
    if (run) {
        banner.heading = result_with_mode(text, *run);
        banner.tone = result_tone(run->result);
    } else {
        banner.heading = text("Never verified");
    }
    if (status.last_full) {
        const auto& full = *status.last_full;
        // How long ago first, which the operator looks for, then when
        // (decision 0049).
        banner.lines.push_back(text.format("Last FULL verify: {}, {}", age_words(text, full.finished, now),
            app::local_time(full.finished, text)));
    } else {
        banner.lines.push_back(text("No FULL verification yet. A FULL verification tells whether the media still "
                                    "holds the content that the archive's current revision recorded."));
    }
    // The last verification of any mode, where it is not the one above.
    if (status.last_any && (!run || status.last_any->run_id != run->run_id)) {
        const auto& any = *status.last_any;
        banner.lines.push_back(text.format("Last verify: {}, {}, {}", result_with_mode(text, any),
            app::local_time(any.finished, text), age_words(text, any.finished, now)));
    }
    // R-105: which limit fired.
    banner.overdue = status.full_overdue || status.any_overdue;
    const auto limit = [&](int days, bool full) {
        if (full) {
            return days == 1 ? text("OVERDUE: no FULL verify for more than 1 day")
                             : text.format("OVERDUE: no FULL verify for more than {} days", days);
        }
        return days == 1 ? text("OVERDUE: no verify of any mode for more than 1 day")
                         : text.format("OVERDUE: no verify of any mode for more than {} days", days);
    };
    if (status.full_overdue) {
        banner.lines.push_back(limit(policy.full_verify_max_age_days, true));
    }
    if (status.any_overdue) {
        banner.lines.push_back(limit(policy.any_verify_max_age_days, false));
    }
    if (status.archive.status == "RETIRED") {
        banner.lines.push_back(text("Retired: this archive is no longer expected to be verified, and takes no new "
                                    "revisions."));
    }
    return banner;
}

std::vector<AlertLine> device_alert_lines(const app::Text& text, const ArchiveStatus& status, IdentityShown shown)
{
    std::vector<AlertLine> lines;
    if (status.last_run) {
        for (auto& line : app::health_alert_lines(
                 text, displayed_devices(recorded_device_health(status.last_run->counts_json), shown))) {
            lines.push_back({std::move(line), {}});
        }
    }
    if (status.last_full) {
        if (const auto compared = recorded_speed_comparison(status.last_full->counts_json);
            compared && compared->alerts()) {
            if (auto line = app::slower_alert_line(text, *compared)) {
                lines.push_back({std::move(*line), status.last_full->run_id});
            }
        }
    }
    return lines;
}

std::vector<Field> device_fields(const app::Text& text, const ArchiveStatus& status, IdentityShown shown)
{
    std::vector<Field> fields;
    if (!status.header) {
        return fields;
    }
    const auto identity = displayed_identity(status.header->identity, shown);
    fields.push_back({text("Volume"), identity.volume_label.empty() ? text("(none)") : identity.volume_label});
    fields.push_back({text("Capacity"), format_bytes(identity.capacity_bytes)});
    for (const auto& device : identity.devices) {
        fields.push_back({text("Model"), device.model.empty() ? text("not known") : device.model});
        if (!device.bus.empty()) {
            fields.push_back({text("Bus"), bus_words(device.bus)});
        }
        if (!device.serial.empty()) {
            fields.push_back({text("Serial"), device.serial});
        } else if (!device.wwn.empty()) {
            fields.push_back({text("WWN"), device.wwn});
        } else {
            fields.push_back({text("Serial"), text("not known")});
        }
    }
    // As the last run asked them, named as the views name devices.
    if (status.last_run) {
        const auto devices = displayed_devices(recorded_device_health(status.last_run->counts_json), shown);
        std::string health;
        for (const auto& device : devices) {
            if (!device.health) {
                continue;
            }
            const auto words =
                devices.size() == 1 ? app::health_words(text, *device.health) : app::device_health_words(text, device);
            health += (health.empty() ? "" : "\n") + words;
        }
        if (!health.empty()) {
            fields.push_back({text("Health in the last verification"), health});
        }
    }
    if (status.last_full) {
        const auto& counts = status.last_full->counts_json;
        if (const auto speed = app::read_speed_words(
                text, recorded_read_speed(counts), recorded_speed_comparison(counts))) {
            fields.push_back({text("Read speed of the last FULL verify"), *speed});
        }
    }
    return fields;
}

std::vector<Field> content_fields(const app::Text& text, const ArchiveStatus& status)
{
    std::vector<Field> fields;
    const auto& archive = status.archive;
    if (status.header) {
        const auto& header = *status.header;
        fields.push_back({text("Root"), header.root_hint.empty() ? text("not known") : header.root_hint,
            !header.root_hint.empty()});
        fields.push_back({text("Files"), joined(text, {app::count_of(text, header.counts.files, app::Counted::File),
                                                     format_bytes(header.counts.bytes)})});
        fields.push_back(
            {text("Revision"), text.format("{}, recorded {}", header.revision, app::local_time(header.created, text))});
        fields.push_back({text("Chunk hashes"), header.chunk_size != 0
                                                    ? text.format("yes (chunk size {})", format_bytes(header.chunk_size))
                                                    : text("none")});
    } else {
        fields.push_back({text("Root"), text("not known")});
        fields.push_back({text("Revision"), std::to_string(archive.current_revision)});
    }
    fields.push_back({text("Archive UUID"), archive.archive_uuid});
    return fields;
}

bool lists_adding_chunk_hashes(const ArchiveStatus& status)
{
    return !(status.header && status.header->chunk_size != 0);
}

Choice adding_choice(const app::Text& text, const std::expected<std::optional<std::string>, IoError>& why_not,
    bool adding_unfinished)
{
    if (!why_not) {
        return {false, text.format("Cannot tell whether chunk hashes can be added: {}", text(why_not.error().message))};
    }
    if (*why_not) {
        return {false, text(**why_not)};
    }
    if (adding_unfinished) {
        return {false, text("A verification of this archive that adds chunk hashes is unfinished: resume it, or "
                            "abandon it under Unfinished operations.")};
    }
    return {true, text("Every file is read once; when every entry matches, the next revision records the same "
                       "entries with their chunk hashes. Any difference records the verification alone.")};
}

Choice new_revision_choice(const app::Text& text, const ArchiveStatus& status)
{
    if (!status.header || status.header->root_hint.empty()) {
        return {false, text("The archive's root is not known: choose its folder or volume in Register Archive.")};
    }
    if (status.archive.status == "RETIRED") {
        return {false, text("A retired archive takes no new revisions.")};
    }
    return {true, text("After an intended change of its content: reads the whole archive again and takes what it "
                       "holds now as the new baseline.")};
}

Choice compare_choice(const app::Text& text, std::size_t archives)
{
    if (archives < 2) {
        return {false, text("The repository has no other archive to cross-check with.")};
    }
    return {true, text("Uses the manifests in the repository; no media is read.")};
}

std::vector<CopyLink> copy_links(const app::Text& text, const ArchiveStatus& status,
    const std::vector<ArchiveStatus>& all, IdentityShown shown)
{
    std::vector<CopyLink> links;
    const auto& uuid = status.archive.archive_uuid;
    const auto volume_of = [&](const ArchiveStatus& other) {
        return other.header ? displayed_identity(other.header->identity, shown).volume_label : std::string{};
    };
    if (status.header && status.header->copy_of && !status.header->copy_of->empty()) {
        const auto& original = *status.header->copy_of;
        CopyLink link;
        link.other = original;
        link.how = text("This archive is recorded as a copy of it");
        link.reference = original;
        link.copy = uuid;
        const auto found = std::ranges::find(all, original, [](const ArchiveStatus& other) -> const Uuid& {
            return other.archive.archive_uuid;
        });
        if (found != all.end()) {
            link.label = display_label(*found);
            link.volume_label = volume_of(*found);
            link.listed = true;
        } else {
            // Decision 0005: the original may be an archive of another repository.
            link.label = original.substr(0, 8);
            link.volume_label = text("not in this repository");
        }
        links.push_back(std::move(link));
    }
    for (const auto& other : all) {
        if (other.archive.archive_uuid != uuid && other.header && other.header->copy_of &&
            *other.header->copy_of == uuid) {
            links.push_back({other.archive.archive_uuid, display_label(other), volume_of(other),
                text("Recorded as a copy of this archive"), true, uuid, other.archive.archive_uuid});
        }
    }
    return links;
}

std::vector<RunRow> recent_runs(const app::Text& text, const std::vector<VerifyRunRecord>& runs, std::size_t limit)
{
    std::vector<RunRow> rows;
    for (auto run = runs.rbegin(); run != runs.rend() && rows.size() < limit; ++run) {
        RunRow row;
        row.run_id = run->run_id;
        row.result = result_pill(text, *run);
        const auto when = app::local_time(run->finished, text);
        row.title = run->mode == "FULL" ? when : text.format("{} · {}", text(run->mode), when);
        row.meta = joined(text, {format_bytes(run->coverage_bytes.value_or(0)),
                                    duration_words(text, run->started, run->finished)});
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

std::string stopped_verification_words(
    const app::Text& text, const app::OperationRow& row, bool adds_chunk_hashes, bool carrier_confirmed)
{
    // A verification past a changed enclosure adds no chunk hashes (the
    // engine refuses both at once).
    if (carrier_confirmed && !adds_chunk_hashes) {
        return text.format("The last FULL verification of this archive stopped at {}. Resume continues it without "
                           "reading again what it read. Only once it is resumed and passes is the new enclosure "
                           "recorded.",
            row.progress);
    }
    return adds_chunk_hashes
               ? text.format("The last FULL verification of this archive that adds chunk hashes stopped at {}. Resume "
                             "continues it without reading again what it read.",
                     row.progress)
               : text.format("The last FULL verification of this archive stopped at {}. Resume continues it without "
                             "reading again what it read.",
                     row.progress);
}

std::string stopped_relocation_words(const app::Text& text, const app::OperationRow& row)
{
    return text.format("The relocation of this archive to {} stopped at {}. Resume continues it without reading again "
                       "what matched.",
        row.root, row.progress);
}

DetailsOperations details_operations(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const Uuid& archive, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    DetailsOperations shown;
    for (const auto& watch : watches) {
        const auto& operation = watch.operation();
        const bool relocation = operation.kind == app::OperationKind::Relocation;
        if (operation.archive_uuid != archive || (operation.kind != app::OperationKind::Verification && !relocation)) {
            continue;
        }
        shown.adding_unfinished = shown.adding_unfinished || operation.adds_chunk_hashes;
        if (shown.stopped) {
            continue;
        }
        // As its row under Unfinished operations offers Resume.
        auto row = app::describe_operation(text, watch, now, wall_now);
        if (row.offers_resume) {
            shown.words = relocation ? stopped_relocation_words(text, row)
                                     : stopped_verification_words(
                                           text, row, operation.adds_chunk_hashes, operation.carrier_confirmed);
            shown.stopped = std::move(row);
        }
    }
    return shown;
}

VerifyRequest details_resume(
    const app::UnfinishedOperation& stopped, const std::optional<InterruptedRun>& here, const Config& config)
{
    if (!here || here->archive_uuid != stopped.archive_uuid || here->run_id != stopped.run_id) {
        return app::verification_resume(stopped, config);
    }
    // As the window ran it: a root that more than one volume holds is not
    // looked for again by the archive's uuid; strict metadata as it was
    // chosen (R-51), or as the policy sets it; the readers as the config's
    // override says (R-111: the window has no --io).
    VerifyRequest request;
    request.resume_run = here->run_id;
    request.root = here->root;
    if (!here->root) {
        request.archive_uuid = here->archive_uuid;
    }
    request.strict_metadata = here->strict_metadata || config.policy.strict_metadata;
    request.rotational_override = config.rotational_override;
    return request;
}

std::string archive_has_a_job(const app::Text& text, const std::string& label)
{
    return text.format("{} already has a job that waits or works: stop it in Jobs first, or wait for it to end.",
        label);
}

std::string verified_toast(const app::Text& text, const std::string& label, const std::string& result)
{
    return text.format("{}: {}", label, text.format("FULL VERIFY {}", text(result)));
}

std::string not_verified_toast(const app::Text& text, const std::string& name)
{
    return text.format("Not verified: {}", name);
}

} // namespace va::gui
