// `archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path>`
// and `history <uuid>`.
// Archives are always shown with their last FULL verify and last verify of any
// mode, with dates and OVERDUE flags, never a bare "integrity status" (R-21).
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/engine/archive_status.h"
#include "core/engine/relocate_engine.h"
#include "platform/file_system.h"
#include "platform/interrupt.h"
#include "platform/media_enumerator.h"

#include <iostream>

namespace va::cli {
namespace {

// "Last FULL verify: PASSED on <date>" or, with the mode, "Last verify: FULL
// PASSED on <date>", plus OVERDUE; one translated line, so the mode stays in it
// in every language (R-21).
std::string last_verify_line(const app::Text& text, const std::optional<VerifyRunRecord>& run, bool with_mode,
    bool overdue)
{
    std::string line;
    if (!run) {
        line = with_mode ? text("Last verify: never") : text("Last FULL verify: never");
    } else if (with_mode) {
        line = text.format("Last verify: {} {} on {}", text(run->mode), text(run->result), run->finished);
    } else {
        line = text.format("Last FULL verify: {} on {}", text(run->result), run->finished);
    }
    return overdue ? line + "  " + text("OVERDUE") : line;
}

// "ManifestHashMismatch,MediaIdentityMismatch" with each failure class translated.
std::string failure_classes_text(const app::Text& text, std::string_view classes)
{
    std::string result;
    for (std::size_t start = 0; start <= classes.size();) {
        auto end = classes.find(',', start);
        if (end == std::string_view::npos) {
            end = classes.size();
        }
        if (start > 0) {
            result += ',';
        }
        result += text(classes.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

nlohmann::json run_json(const std::optional<VerifyRunRecord>& run)
{
    if (!run) {
        return nullptr;
    }
    return {{"run_id", run->run_id}, {"mode", run->mode}, {"result", run->result},
        {"finished", run->finished}};
}

nlohmann::json status_json(const ArchiveStatus& status)
{
    nlohmann::json json{
        {"archive_uuid", status.archive.archive_uuid},
        {"label", status.archive.label},
        {"display_label", display_label(status)},
        {"status", status.archive.status},
        {"revision", status.archive.current_revision},
        {"root_subpath", status.archive.root_subpath},
        {"created", status.archive.created},
        {"last_full_verify", run_json(status.last_full)},
        {"last_verify", run_json(status.last_any)},
        {"full_verify_overdue", status.full_overdue},
        {"verify_overdue", status.any_overdue},
    };
    if (status.header) {
        const auto& identity = status.header->identity;
        json["volume_label"] = identity.volume_label;
        json["capacity_bytes"] = identity.capacity_bytes;
        json["model"] = identity.devices.empty() ? "" : identity.devices.front().model;
        if (status.header->copy_of) {
            json["copy_of"] = *status.header->copy_of;
        }
    }
    return json;
}

// Accepts a full uuid or a unique prefix of at least 8 characters.
std::expected<ArchiveStatus, Failure> find_status(const std::vector<ArchiveStatus>& statuses,
    const std::string& word)
{
    std::optional<ArchiveStatus> found;
    for (const auto& status : statuses) {
        const auto& uuid = status.archive.archive_uuid;
        if (uuid == word || (word.size() >= 8 && uuid.starts_with(word))) {
            if (found) {
                return std::unexpected(Failure{exit_code::usage, "several archives start with " + word});
            }
            found = status;
        }
    }
    if (!found) {
        return std::unexpected(Failure{exit_code::usage, "no archive " + word});
    }
    return *found;
}

struct Opened {
    Session session;
    ArchiveStore store;
    std::vector<ArchiveStatus> statuses;
};

std::expected<Opened, Failure> open_statuses(const Arguments& arguments)
{
    auto session = open_session(arguments, false);
    if (!session) {
        return std::unexpected(session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return std::unexpected(Failure{exit_code::repository, store.error().message});
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto statuses = archive_statuses(*store, session->context.config.policy, now);
    if (!statuses) {
        return std::unexpected(Failure{exit_code::repository, statuses.error().message});
    }
    return Opened{std::move(*session), std::move(*store), std::move(*statuses)};
}

int list(const Arguments& arguments, const Opened& opened)
{
    if (arguments.json) {
        auto list = nlohmann::json::array();
        for (const auto& status : opened.statuses) {
            list.push_back(status_json(status));
        }
        print_json({{"archives", std::move(list)}});
        return exit_code::passed;
    }
    const auto& text = arguments.text;
    if (opened.statuses.empty()) {
        std::cout << text("No archives registered.") << '\n';
    }
    for (const auto& status : opened.statuses) {
        std::cout << display_label(status) << "  " << status.archive.archive_uuid.substr(0, 8)
                  << "  r" << status.archive.current_revision
                  << (status.archive.status == "REGISTERED" ? "" : "  " + text(status.archive.status));
        if (status.header) {
            const auto& identity = status.header->identity;
            std::cout << "  \"" << identity.volume_label << "\"  "
                      << (identity.devices.empty() ? "" : identity.devices.front().model);
        }
        std::cout << "\n    " << last_verify_line(text, status.last_full, false, status.full_overdue)
                  << "\n    " << last_verify_line(text, status.last_any, true, status.any_overdue) << '\n';
    }
    return exit_code::passed;
}

int show(const Arguments& arguments, Opened& opened)
{
    const auto status = find_status(opened.statuses, arguments.words[2]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    const auto revisions = opened.store.revisions(status->archive.archive_uuid);
    if (!revisions) {
        return report_failure(arguments, {exit_code::repository, revisions.error().message});
    }
    if (arguments.json) {
        auto json = status_json(*status);
        auto list = nlohmann::json::array();
        for (const auto& revision : *revisions) {
            list.push_back({{"revision", revision.revision}, {"created", revision.created},
                {"manifest", revision.manifest_relpath}});
        }
        json["revisions"] = std::move(list);
        if (status->header) {
            json["identity"] = {{"filesystem_type", status->header->identity.filesystem_type},
                {"filesystem_uuid", status->header->identity.filesystem_uuid},
                {"counts", {{"files", status->header->counts.files}, {"dirs", status->header->counts.dirs},
                               {"links", status->header->counts.links}, {"bytes", status->header->counts.bytes}}}};
        }
        print_json(json);
        return exit_code::passed;
    }
    const auto& text = arguments.text;
    std::cout << display_label(*status) << "  " << status->archive.archive_uuid << "\n  "
              << text.format("status: {}  revision: {}", text(status->archive.status),
                     status->archive.current_revision)
              << "\n  " << text.format("registered: {}", status->archive.created) << "\n  "
              << text.format("root_subpath: {}",
                     status->archive.root_subpath.empty() ? text("(volume root)") : status->archive.root_subpath)
              << '\n';
    if (status->header) {
        const auto& header = *status->header;
        std::cout << "  "
                  << text.format("filesystem: {} {}  label \"{}\"", header.identity.filesystem_type,
                         header.identity.filesystem_uuid, header.identity.volume_label)
                  << '\n';
        for (const auto& device : header.identity.devices) {
            std::cout << "  "
                      << (device.serial.empty() ? text.format("device: {}  wwn {}", device.model, device.wwn)
                                                : text.format("device: {}  serial {}", device.model, device.serial))
                      << std::endl;
        }
        std::cout << "  "
                  << text.format("content: {} files, {} empty directories, {} links, {} bytes", header.counts.files,
                         header.counts.dirs, header.counts.links, header.counts.bytes)
                  << '\n';
        if (header.copy_of) {
            std::cout << "  " << text.format("copy of: {}", *header.copy_of) << '\n';
        }
    }
    std::cout << "  " << last_verify_line(text, status->last_full, false, status->full_overdue) << "\n  "
              << last_verify_line(text, status->last_any, true, status->any_overdue) << "\n  "
              << text("revisions:") << '\n';
    for (const auto& revision : *revisions) {
        std::cout << "    r" << revision.revision << "  " << revision.created << "  "
                  << revision.manifest_relpath << '\n';
    }
    return exit_code::passed;
}

int retire(const Arguments& arguments, Opened& opened)
{
    const auto status = find_status(opened.statuses, arguments.words[2]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    auto archive = status->archive;
    archive.status = "RETIRED";
    if (auto updated = opened.store.update_archive(archive); !updated) {
        return report_failure(arguments, {exit_code::repository, updated.error().message});
    }
    if (arguments.json) {
        print_json({{"archive_uuid", archive.archive_uuid}, {"status", archive.status}});
    } else {
        std::cout << arguments.text.format("Retired {} ({}). It is no longer expected to be verified, is still "
                                           "recognised when attached, and keeps its history.",
                         display_label(*status), archive.archive_uuid)
                  << '\n';
    }
    return exit_code::passed;
}

} // namespace

// Decision 0004: accept a copy (or a new enclosure) as the archive's carrier
// after a FULL verify of every entry; VeriArchive itself copies nothing.
int relocate(const Arguments& arguments, Opened& opened)
{
    const auto status = find_status(opened.statuses, arguments.words[2]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    RelocateRequest request{status->archive.archive_uuid, user_path(arguments.words[3]), std::nullopt};
    if (const auto io = arguments.value("--io")) {
        if (*io != "hdd" && *io != "ssd") {
            return report_failure(arguments, {exit_code::usage, "--io takes hdd or ssd"});
        }
        request.rotational = *io == "hdd";
    }
    const auto archive_files = platform::make_file_system_access();
    const auto repository_files = platform::make_file_system_access();
    const auto media = platform::make_media_enumerator();
    RelocateServices services{*archive_files, *repository_files, *opened.session.writer, *media,
        opened.store, opened.session.context.repo, opened.session.context.repository_uuid, host_info()};
    CancelToken cancel;
    platform::cancel_on_interrupt(cancel);
    CliProgress progress(arguments);
    const auto outcome = relocate_archive(services, request, cancel, progress);
    progress.finish();

    const auto& text = arguments.text;
    if (!outcome) {
        if (outcome.error().kind == RelocateFailure::Kind::Differs && !arguments.json) {
            std::cout << text.format("RELOCATION REFUSED: {}", text(outcome.error().message)) << '\n';
            for (const auto& item : outcome.error().items) {
                std::cout << "  " << text(item.status) << "  " << item.path.p << '\n';
            }
            return exit_code::failed;
        }
        const auto kind = outcome.error().kind;
        const int code = kind == RelocateFailure::Kind::Incomplete ? exit_code::incomplete
                         : kind == RelocateFailure::Kind::Differs  ? exit_code::failed
                         : kind == RelocateFailure::Kind::Refused  ? exit_code::identity_decision
                                                                   : exit_code::repository;
        return report_failure(arguments, {code, outcome.error().message});
    }
    if (arguments.json) {
        print_json({{"archive_uuid", outcome->archive_uuid}, {"revision", outcome->revision},
            {"result", outcome->result}, {"run_id", outcome->run_id},
            {"manifest", path_to_utf8(outcome->manifest)}, {"counts", outcome->counts}});
    } else {
        std::cout << text.format("Relocated {} ({}) to {} as revision {}", outcome->label, outcome->archive_uuid,
                         path_to_utf8(request.root), outcome->revision)
                  << "\n  "
                  << text.format("FULL VERIFY {} of the new carrier (run {})", text(outcome->result),
                         outcome->run_id)
                  << '\n';
        for (const auto& item : outcome->items) {
            std::cout << "  " << text(item.status) << "  " << item.path.p
                      << (item.detail.empty() ? "" : "  " + text.parenthesized(text(item.detail)))
                      << '\n';
        }
    }
    return outcome->result == "PASSED" ? exit_code::passed : exit_code::passed_with_warnings;
}

int archive_command(const Arguments& arguments)
{
    const bool relocate_command = arguments.words.size() == 4 && arguments.words[1] == "relocate";
    if (const auto problem = check_options(arguments, {}, relocate_command ? std::initializer_list<std::string_view>{"--io"}
                                                                           : std::initializer_list<std::string_view>{})) {
        return report_failure(arguments, *problem);
    }
    const bool list_command = arguments.words.size() == 2 && arguments.words[1] == "list";
    const bool uuid_command = arguments.words.size() == 3 &&
                              (arguments.words[1] == "show" || arguments.words[1] == "retire");
    if (!list_command && !uuid_command && !relocate_command) {
        return report_failure(arguments, {exit_code::usage,
            "usage: archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path> [--io hdd|ssd]"});
    }
    auto opened = open_statuses(arguments);
    if (!opened) {
        return report_failure(arguments, opened.error());
    }
    if (list_command) {
        return list(arguments, *opened);
    }
    if (relocate_command) {
        return relocate(arguments, *opened);
    }
    return arguments.words[1] == "show" ? show(arguments, *opened) : retire(arguments, *opened);
}

int history_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {}, {})) {
        return report_failure(arguments, *problem);
    }
    if (arguments.words.size() != 2) {
        return report_failure(arguments, {exit_code::usage, "usage: history <uuid>"});
    }
    auto opened = open_statuses(arguments);
    if (!opened) {
        return report_failure(arguments, opened.error());
    }
    const auto status = find_status(opened->statuses, arguments.words[1]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    const auto runs = opened->store.verify_runs(status->archive.archive_uuid);
    if (!runs) {
        return report_failure(arguments, {exit_code::repository, runs.error().message});
    }
    if (arguments.json) {
        auto list = nlohmann::json::array();
        for (const auto& run : *runs) {
            nlohmann::json entry{{"run_id", run.run_id}, {"revision", run.revision}, {"mode", run.mode},
                {"started", run.started}, {"finished", run.finished}, {"result", run.result},
                {"failure_classes", run.failure_classes}, {"host", run.host}};
            entry["coverage_bytes"] = run.coverage_bytes ? nlohmann::json(*run.coverage_bytes) : nullptr;
            list.push_back(std::move(entry));
        }
        print_json({{"archive_uuid", status->archive.archive_uuid}, {"runs", std::move(list)}});
        return exit_code::passed;
    }
    const auto& text = arguments.text;
    std::cout << display_label(*status) << "  " << status->archive.archive_uuid << '\n';
    if (runs->empty()) {
        std::cout << "  " << text("never verified") << '\n';
    }
    for (const auto& run : *runs) {
        std::cout << "  "
                  << text.format("{}  {} VERIFY {}  r{}  {} bytes", run.finished, text(run.mode), text(run.result),
                         run.revision, run.coverage_bytes.value_or(0))
                  << (run.failure_classes.empty() ? "" : "  " + failure_classes_text(text, run.failure_classes))
                  << "  " << run.run_id << '\n';
    }
    return exit_code::passed;
}

} // namespace va::cli
