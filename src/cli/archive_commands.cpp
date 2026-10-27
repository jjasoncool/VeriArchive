// `archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid>
// <root_path> [--resume] | compare <uuid> <uuid> | abandon-registration
// <uuid> | abandon-relocation <uuid>` and `history <uuid>`. Archives are always shown
// with their last FULL verify and last verify of any mode, with dates and
// OVERDUE flags, never a bare "integrity status" (R-21), and with their
// unfinished registration and relocation, one line each (decision 0026).
#include "cli/commands.h"

#include "app/history.h"
#include "app/local_time.h"
#include "app/plain_words.h"
#include "app/unfinished_operations.h"

#include "common/utf8.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/abandon_relocation.h"
#include "core/engine/archive_status.h"
#include "core/engine/compare_archives.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/retire_archive.h"
#include "platform/file_system.h"
#include "platform/interrupt.h"
#include "platform/media_enumerator.h"
#include "platform/process_table.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

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
        line = text.format("Last verify: {} {} on {}", text(run->mode), text(run->result),
            app::local_time(run->finished, text));
    } else {
        line = text.format("Last FULL verify: {} on {}", text(run->result), app::local_time(run->finished, text));
    }
    return overdue ? line + "  " + text("OVERDUE") : line;
}

nlohmann::json run_json(const std::optional<VerifyRunRecord>& run)
{
    if (!run) {
        return nullptr;
    }
    return {{"run_id", run->run_id}, {"mode", run->mode}, {"result", run->result},
        {"finished", run->finished}};
}

// An archive's unfinished registration and relocation (decisions 0022, 0026),
// each as the Archive List judges it from one read, its attempt judged
// through this computer's process table (decision 0024), and as --json gives
// it: where it reads, its progress from its checkpoints, and its state.
struct UnfinishedOfArchive {
    std::optional<app::UnfinishedOperation> registration;
    std::optional<app::UnfinishedOperation> relocation;
    nlohmann::json registration_json;  // null when there is none
    nlohmann::json relocation_json;    // null when there is none
};
using UnfinishedByArchive = std::map<Uuid, UnfinishedOfArchive>;

nlohmann::json optional_text(const std::string& text)
{
    return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
}

nlohmann::json process_json(const app::UnfinishedOperation& operation)
{
    return {{"id", operation.process->process.id}, {"program", operation.process->process.program},
        {"host", operation.process->host}};
}

// The unfinished registrations and relocations of the repository, by
// archive, read in one read transaction. Read only.
std::expected<UnfinishedByArchive, Failure> unfinished_by_archive(ArchiveStore& store)
{
    std::vector<PendingRegistration> registrations;
    std::vector<UnfinishedRelocation> relocations;
    const auto read = store.read_transaction([&]() -> std::expected<void, IoError> {
        auto pending = store.pending_registrations();
        if (!pending) {
            return std::unexpected(pending.error());
        }
        registrations = std::move(*pending);
        auto moving = store.unfinished_relocations();
        if (!moving) {
            return std::unexpected(moving.error());
        }
        relocations = std::move(*moving);
        return {};
    });
    if (!read) {
        return std::unexpected(Failure{exit_code::repository, read.error().message});
    }
    const auto processes = platform::make_process_table();
    const app::AttemptCheck attempts{processes.get(), host_info().host, {}};
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto optional_count = [](const std::optional<std::uint64_t>& count) {
        return count ? nlohmann::json(*count) : nlohmann::json(nullptr);
    };
    UnfinishedByArchive result;
    for (const auto& registration : registrations) {
        auto operation = app::registration_operation(registration, attempts);
        nlohmann::json json{
            {"root", registration.root},
            {"revision", registration.revision},
            {"started", registration.started},
            {"updated", optional_text(registration.updated)},
            {"stopped", optional_text(registration.stopped)},
            {"state", app::state_code(app::state_of_one_read(operation, wall_now))},
            {"files_done", registration.done_counted ? nlohmann::json(registration.files_done) : nullptr},
            {"bytes_done", registration.done_counted ? nlohmann::json(registration.bytes_done) : nullptr},
            {"files_total", optional_count(registration.files_total)},
            {"bytes_total", optional_count(registration.bytes_total)},
        };
        if (operation.process) {
            json["process"] = process_json(operation);
        }
        auto& unfinished = result[registration.archive_uuid];
        unfinished.registration = std::move(operation);
        unfinished.registration_json = std::move(json);
    }
    for (const auto& relocation : relocations) {
        auto operation = app::relocation_operation(relocation, attempts);
        nlohmann::json json{
            {"root", relocation.root},
            {"revision", relocation.revision},
            {"run_id", relocation.run_id},
            {"started", relocation.started},
            {"updated", optional_text(relocation.updated)},
            {"stopped", optional_text(relocation.stopped)},
            {"differs", optional_text(relocation.differs)},
            {"state", app::state_code(app::state_of_one_read(operation, wall_now))},
            {"resumable", relocation.resumable},
            {"files_done", relocation.done_counted ? nlohmann::json(relocation.files_done) : nullptr},
            {"bytes_done", relocation.done_counted ? nlohmann::json(relocation.bytes_done) : nullptr},
            {"files_total", relocation.files_total},
            {"bytes_total", relocation.bytes_total},
        };
        if (operation.process) {
            json["process"] = process_json(operation);
        }
        auto& unfinished = result[relocation.archive_uuid];
        unfinished.relocation = std::move(operation);
        unfinished.relocation_json = std::move(json);
    }
    return result;
}

// The lines of an archive's unfinished registration and relocation, as
// `archive list` and `archive show` print them under the archive.
void print_unfinished(const app::Text& text, const UnfinishedByArchive& unfinished, const Uuid& archive_uuid,
    std::string_view indent)
{
    const auto found = unfinished.find(archive_uuid);
    if (found == unfinished.end()) {
        return;
    }
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (const auto* operation : {&found->second.registration, &found->second.relocation}) {
        if (*operation) {
            std::cout << indent << app::describe_for_archive(text, **operation, wall_now) << '\n';
        }
    }
}

// An archive whose latest manifest cannot be used is listed with that
// problem, in the plain message that ends with how to fix it (decision 0028);
// a retired archive whose latest manifest is lost, as retired with its
// manifest lost, which is no problem the operator owes the repository any
// more (point 9).
void print_manifest_problem(const app::Text& text, const ArchiveStatus& status, std::string_view indent)
{
    if (!status.unusable_manifest) {
        return;
    }
    const auto message = text(plain_message(*status.unusable_manifest));
    std::cout << indent
              << (status.manifest_lost ? text.format("manifest lost: {}", message) : text.format("problem: {}", message))
              << '\n';
}

// A manifest that cannot be used, as --json gives it (decision 0028).
nlohmann::json manifest_problem_json(const UnusableManifest& unusable)
{
    return {{"kind", kind_code(unusable.kind)}, {"revision", unusable.revision}, {"manifest", unusable.manifest},
        {"message", plain_message(unusable)}};
}

// The problem of the manifest of an earlier revision of the archive, where
// `archive show` read one that cannot be used.
const UnusableManifest* earlier_problem(const ArchiveStatus& status, std::uint32_t revision)
{
    const auto found = std::ranges::find_if(
        status.earlier_unusable, [&](const UnusableManifest& unusable) { return unusable.revision == revision; });
    return found == status.earlier_unusable.end() ? nullptr : &*found;
}

nlohmann::json status_json(const ArchiveStatus& status, const UnfinishedByArchive& unfinished)
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
    if (status.unusable_manifest) {
        json["manifest_problem"] = manifest_problem_json(*status.unusable_manifest);
    }
    // Only a retired archive whose latest manifest is lost has it (decision
    // 0028, point 9), as only an archive whose manifest cannot be used has
    // `manifest_problem`.
    if (status.manifest_lost) {
        json["manifest_lost"] = true;
    }
    if (const auto found = unfinished.find(status.archive.archive_uuid); found != unfinished.end()) {
        if (!found->second.registration_json.is_null()) {
            json["unfinished_registration"] = found->second.registration_json;
        }
        if (!found->second.relocation_json.is_null()) {
            json["unfinished_relocation"] = found->second.relocation_json;
        }
    }
    return json;
}

// Accepts a full uuid or a unique prefix of at least 8 characters (names_archive).
std::expected<ArchiveStatus, Failure> find_status(const std::vector<ArchiveStatus>& statuses,
    const std::string& word)
{
    std::optional<ArchiveStatus> found;
    for (const auto& status : statuses) {
        const auto& uuid = status.archive.archive_uuid;
        if (names_archive(uuid, word)) {
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

// Every archive's status, or only that of the archive `named` (a uuid or a
// unique prefix, as find_archive_uuid takes it): a command about one archive
// reads no other archive's manifest. `headers`: whether the command shows
// what the latest revision's header holds, which is read from its manifest;
// `earlier`: whether it shows the problem of an earlier revision's manifest
// that cannot be used (`archive show`, decision 0028).
std::expected<Opened, Failure> open_statuses(const Arguments& arguments,
    const std::optional<std::string>& named = std::nullopt, bool headers = true, bool earlier = false)
{
    auto session = open_session(arguments, false);
    if (!session) {
        return std::unexpected(session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return std::unexpected(Failure{exit_code::repository, store.error().message});
    }
    StatusSelection selection{std::nullopt, headers, earlier};
    if (named) {
        auto uuid = find_archive_uuid(*store, *named);
        if (!uuid) {
            return std::unexpected(uuid.error());
        }
        selection.only = std::move(*uuid);
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto repository_files = platform::make_file_system_access();
    auto statuses = archive_statuses(
        *store, *repository_files, session->context.repo, session->context.config.policy, now, selection);
    if (!statuses) {
        return std::unexpected(Failure{exit_code::repository, statuses.error().message});
    }
    return Opened{std::move(*session), std::move(*store), std::move(*statuses)};
}

// Decision 0028: the listing worked, but an archive's latest manifest cannot
// be used, or (in `archive show`) an earlier one, which the operator must
// fix: exit 2, as rebuild-index exits after skipping a manifest. Not for the
// latest manifest of a retired archive that is lost (point 9): the operator
// declared it so, and it stops nothing; an earlier revision's is still left
// out of the index by `repo rebuild-index` (point 6).
int listed_code(const std::vector<ArchiveStatus>& statuses)
{
    const bool problem = std::ranges::any_of(statuses, [](const ArchiveStatus& status) {
        return (status.unusable_manifest.has_value() && !status.manifest_lost) || !status.earlier_unusable.empty();
    });
    return problem ? exit_code::passed_with_warnings : exit_code::passed;
}

int list(const Arguments& arguments, Opened& opened)
{
    const auto unfinished = unfinished_by_archive(opened.store);
    if (!unfinished) {
        return report_failure(arguments, unfinished.error());
    }
    if (arguments.json) {
        auto list = nlohmann::json::array();
        for (const auto& status : opened.statuses) {
            list.push_back(status_json(status, *unfinished));
        }
        print_json({{"archives", std::move(list)}});
        return listed_code(opened.statuses);
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
        std::cout << '\n';
        print_manifest_problem(text, status, "    ");
        std::cout << "    " << last_verify_line(text, status.last_full, false, status.full_overdue) << "\n    "
                  << last_verify_line(text, status.last_any, true, status.any_overdue) << '\n';
        print_unfinished(text, *unfinished, status.archive.archive_uuid, "    ");
    }
    return listed_code(opened.statuses);
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
    const auto unfinished = unfinished_by_archive(opened.store);
    if (!unfinished) {
        return report_failure(arguments, unfinished.error());
    }
    if (arguments.json) {
        auto json = status_json(*status, *unfinished);
        auto list = nlohmann::json::array();
        for (const auto& revision : *revisions) {
            nlohmann::json item{{"revision", revision.revision}, {"created", revision.created},
                {"manifest", revision.manifest_relpath}};
            if (const auto* problem = earlier_problem(*status, revision.revision)) {
                item["manifest_problem"] = manifest_problem_json(*problem);
            }
            list.push_back(std::move(item));
        }
        json["revisions"] = std::move(list);
        if (status->header) {
            json["identity"] = {{"filesystem_type", status->header->identity.filesystem_type},
                {"filesystem_uuid", status->header->identity.filesystem_uuid},
                {"counts", {{"files", status->header->counts.files}, {"dirs", status->header->counts.dirs},
                               {"links", status->header->counts.links}, {"bytes", status->header->counts.bytes}}}};
        }
        print_json(json);
        return listed_code({*status});
    }
    const auto& text = arguments.text;
    std::cout << display_label(*status) << "  " << status->archive.archive_uuid << "\n  "
              << text.format("status: {}  revision: {}", text(status->archive.status),
                     status->archive.current_revision)
              << "\n  " << text.format("registered: {}", app::local_time(status->archive.created, text)) << "\n  "
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
    print_manifest_problem(text, *status, "  ");
    std::cout << "  " << last_verify_line(text, status->last_full, false, status->full_overdue) << "\n  "
              << last_verify_line(text, status->last_any, true, status->any_overdue) << '\n';
    print_unfinished(text, *unfinished, status->archive.archive_uuid, "  ");
    std::cout << "  " << text("revisions:") << '\n';
    for (const auto& revision : *revisions) {
        std::cout << "    r" << revision.revision << "  " << app::local_time(revision.created, text) << "  "
                  << revision.manifest_relpath << '\n';
        // An earlier revision's manifest that cannot be used stops the
        // matching that reads the earlier carriers (decision 0028).
        if (const auto* problem = earlier_problem(*status, revision.revision)) {
            std::cout << "      " << text.format("problem: {}", text(plain_message(*problem))) << '\n';
        }
    }
    return listed_code({*status});
}

// `archive retire <uuid> [--confirm]` (decision 0003): the archive is no
// longer expected to be verified. Where its latest manifest is lost, retiring
// it declares that its device is gone as well, and so needs --confirm after
// saying what it means; without it nothing is written (exit 12, decision
// 0028, point 9).
int retire(const Arguments& arguments, Opened& opened)
{
    const auto status = find_status(opened.statuses, arguments.words[2]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    const auto repository_files = platform::make_file_system_access();
    const auto retired = retire_archive(opened.store, *repository_files, opened.session.context.repo,
        RetireRequest{status->archive.archive_uuid, arguments.has("--confirm")});
    const auto& text = arguments.text;
    if (!retired) {
        const auto& failure = retired.error();
        const int code =
            failure.kind == RetireFailure::Kind::Repository ? exit_code::repository : exit_code::identity_decision;
        if (failure.kind != RetireFailure::Kind::Unconfirmed || !failure.retirement ||
            !failure.retirement->lost_manifest) {
            return report_failure(arguments, {code, failure.message});
        }
        // What retiring the archive means, as a registration over the lost
        // manifest says what it would register before it is confirmed.
        const auto& retirement = *failure.retirement;
        const auto& lost = *retirement.lost_manifest;
        if (arguments.json) {
            print_json({{"error", failure.message}, {"exit_code", code},
                {"retirement", {{"archive_uuid", retirement.archive_uuid}, {"label", retirement.label},
                                   {"revision", retirement.revision}, {"manifest_problem", manifest_problem_json(lost)}}}});
            return code;
        }
        std::cout << text.format("Lost manifest of {} ({}), revision {}", retirement.label, retirement.archive_uuid,
                         retirement.revision)
                  << "\n  " << text(what_is_wrong_with(lost)) << '\n'
                  << text.format("Retiring the archive declares that manifest lost for good: do this only where no "
                                 "backup of the repository, export of revision {} or media copy holds it, and the "
                                 "device that held the archive is gone as well (broken, lost or wiped); otherwise put "
                                 "the manifest back with `import`, or register the archive's next revision from its "
                                 "folder or volume with `register <root_path> --lost-manifest {}`. The archive keeps "
                                 "its verification history and its hash chain, but can no longer be verified or take "
                                 "a new revision. It no longer stops the registration of a new folder or volume or a "
                                 "relocation: retired, it is no longer taken as a possible owner of one, as identity "
                                 "matching cannot use its latest manifest.",
                         retirement.revision, retirement.archive_uuid)
                  << '\n';
        return report_failure(arguments, {code, failure.message});
    }
    if (arguments.json) {
        nlohmann::json json{{"archive_uuid", retired->archive_uuid}, {"status", "RETIRED"}};
        // Only a retirement of an archive whose latest manifest is lost has
        // them, as `archive list` gives them (decision 0028, point 9).
        if (retired->lost_manifest) {
            json["manifest_lost"] = true;
            json["manifest_problem"] = manifest_problem_json(*retired->lost_manifest);
        }
        print_json(json);
    } else if (retired->lost_manifest) {
        std::cout << text.format("Retired {} ({}), whose manifest of revision {} is lost. It keeps its verification "
                                 "history and its hash chain; it can no longer be verified or take a new revision, "
                                 "and no longer stops the registration of a new folder or volume or a relocation.",
                         display_label(*status), retired->archive_uuid, retired->revision)
                  << '\n';
    } else {
        std::cout << text.format("Retired {} ({}). It is no longer expected to be verified, is still "
                                 "recognised when attached, and keeps its history.",
                         display_label(*status), retired->archive_uuid)
                  << '\n';
    }
    return exit_code::passed;
}

// Shown under every comparison in text output; translated there.
constexpr const char* compare_scope =
    "This compares what each registration read. A FULL verify of each archive proves that its carrier "
    "still holds that content.";

int comparison_code(const std::string& result)
{
    if (result == "IDENTICAL") {
        return exit_code::passed;
    }
    if (result == "IDENTICAL_WITH_WARNINGS") {
        return exit_code::passed_with_warnings;
    }
    return exit_code::failed; // DIFFERENT
}

nlohmann::json compared_archive_json(const Uuid& uuid, const std::string& label, std::uint32_t revision,
    const std::string& registered)
{
    return {{"uuid", uuid}, {"label", label}, {"revision", revision}, {"registered", registered}};
}

nlohmann::json comparison_json(const ArchiveComparison& comparison)
{
    auto differences = nlohmann::json::array();
    for (const auto& difference : comparison.differences) {
        differences.push_back({{"p", difference.path}, {"stream", difference.stream},
            {"status", difference.status}, {"detail", difference.detail}});
    }
    return {
        {"archive_a", compared_archive_json(comparison.archive_a, comparison.label_a, comparison.revision_a,
                          comparison.registered_a)},
        {"archive_b", compared_archive_json(comparison.archive_b, comparison.label_b, comparison.revision_b,
                          comparison.registered_b)},
        {"result", comparison.result},
        {"same", comparison.same},
        {"entries_a", comparison.entries_a},
        {"entries_b", comparison.entries_b},
        {"counts", comparison.counts},
        {"differences", std::move(differences)},
    };
}

void print_comparison(const app::Text& text, const ArchiveComparison& comparison)
{
    // Like verification's result line (R-21), the first line says what was done
    // and its result in every language.
    std::cout << text.format("COMPARE {}  {} ({}) r{}  vs  {} ({}) r{}", text(comparison.result),
                     comparison.label_a, comparison.archive_a, comparison.revision_a, comparison.label_b,
                     comparison.archive_b, comparison.revision_b)
              // Details such as "unreadable in A" name the archives A and B.
              << "\n  "
              << text.format("A: {}, registered {}, {} entries", comparison.label_a,
                     app::local_time(comparison.registered_a, text), comparison.entries_a)
              << "\n  "
              << text.format("B: {}, registered {}, {} entries", comparison.label_b,
                     app::local_time(comparison.registered_b, text), comparison.entries_b)
              << "\n  " << text.format("same: {}", comparison.same);
    for (const auto& [status, count] : comparison.counts) {
        std::cout << "  " << text(status) << " " << count;
    }
    std::cout << '\n';
    for (const auto& difference : comparison.differences) {
        // An alternate data stream (V1.1) is written "file:stream", as Windows writes it.
        std::cout << "  " << text(difference.status) << "  " << difference.path
                  << (difference.stream.empty() ? "" : ":" + difference.stream)
                  << (difference.detail.empty() ? ""
                                                 : "  " + text.parenthesized(text(app::item_detail_words(difference.detail))))
                  << '\n';
    }
    std::cout << "  " << text(compare_scope) << '\n';
}

// `archive compare <uuid> <uuid>`: whether two archives hold the same entries,
// from their manifests in the repository; no media is read (decision 0020).
// Each archive is named like in `archive show`: a full uuid or a unique prefix
// of at least 8 characters.
int compare(const Arguments& arguments)
{
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto archive_a = find_archive_uuid(*store, arguments.words[2]);
    if (!archive_a) {
        return report_failure(arguments, archive_a.error());
    }
    const auto archive_b = find_archive_uuid(*store, arguments.words[3]);
    if (!archive_b) {
        return report_failure(arguments, archive_b.error());
    }

    const auto repository_files = platform::make_file_system_access();
    CancelToken cancel;
    const platform::CancelOnInterrupt interrupt(cancel);
    CliProgress progress(arguments, ProgressUnit::entries);
    // Loading the manifests comes first and reports no progress; for large
    // archives it takes seconds, so say what is happening.
    progress.show(arguments.text("comparing: reading the manifests"));
    const auto comparison = compare_archives(*store, *repository_files, session->context.repo, *archive_a,
        *archive_b, cancel, progress);
    progress.finish();
    if (!comparison) {
        const auto& failure = comparison.error();
        if (failure.failure_class == "Cancelled") {
            return report_failure(arguments, {exit_code::incomplete, failure.message});
        }
        // Every other failure means a manifest could not be used (bytes that
        // are not the recorded ones, no revision yet). As in `export`, the
        // failure class leads the message so the reader sees which check failed.
        return report_failure(arguments, {exit_code::failed, failure.failure_class + ": " + failure.message});
    }
    if (arguments.json) {
        print_json(comparison_json(*comparison));
    } else {
        print_comparison(arguments.text, *comparison);
    }
    return comparison_code(comparison->result);
}

// An unfinished registration as --json shows it, abandoned or to be.
nlohmann::json abandonable_json(const AbandonableRegistration& registration)
{
    return {
        {"archive_uuid", registration.archive_uuid},
        {"label", registration.label},
        {"revision", registration.revision},
        {"root", registration.root},
        {"started", registration.created},
        {"saved_progress", {{"files", registration.files}, {"bytes", registration.bytes}}},
        {"first_registration", registration.first_registration},
        {"media_copy", registration.media_copy},
    };
}

// `archive abandon-registration <uuid> [--confirm]` (decision 0025): gives up
// an unfinished registration on purpose, such as one whose media failed. Its
// plan and saved progress go; recorded revisions, manifests and the history
// stay. Without --confirm it only says what would be lost (exit 12).
int abandon(const Arguments& arguments)
{
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto archive_uuid = find_archive_uuid(*store, arguments.words[2]);
    if (!archive_uuid) {
        return report_failure(arguments, archive_uuid.error());
    }
    const auto processes = platform::make_process_table();
    AbandonServices services{*store, *processes, host_info().host};
    const AbandonRequest request{*archive_uuid, std::nullopt, std::nullopt};
    const auto failed = [&](const AbandonFailure& failure) {
        return report_failure(arguments, {failure.kind == AbandonFailure::Kind::Refused ? exit_code::identity_decision
                                                                                       : exit_code::repository,
                                             failure.message});
    };
    const auto& text = arguments.text;

    // The confirmation: what would be lost, and how to go ahead.
    if (!arguments.has("--confirm")) {
        const auto registration = abandonable_registration(services, request);
        if (!registration) {
            return failed(registration.error());
        }
        const std::string confirm = "run the command again with --confirm to abandon it";
        if (arguments.json) {
            print_json({{"error", confirm}, {"exit_code", exit_code::identity_decision},
                {"registration", abandonable_json(*registration)}});
            return exit_code::identity_decision;
        }
        std::cout << text.format("Unfinished registration of {} ({}), revision {}", registration->label,
                         registration->archive_uuid, registration->revision)
                  << "\n  " << text.format("root: {}", registration->root)
                  << "\n  " << text.format("started: {}", app::local_time(registration->created, text))
                  << "\n  "
                  << text.format("saved progress: {} files, {} bytes", registration->files, registration->bytes)
                  << '\n'
                  << text("Abandoning it removes its saved progress, and it can never be resumed afterwards. "
                          "Recorded revisions, manifests and the verification history stay as they are.")
                  << '\n';
        if (registration->first_registration) {
            std::cout << text("The archive has no revision yet: it is removed with its registration.") << '\n';
        }
        return report_failure(arguments, {exit_code::identity_decision, confirm});
    }

    const auto abandoned = abandon_registration(services, request);
    if (!abandoned) {
        return failed(abandoned.error());
    }
    if (arguments.json) {
        auto json = abandonable_json(*abandoned);
        json["archive_removed"] = abandoned->first_registration;
        print_json(json);
        return exit_code::passed;
    }
    std::cout << text.format("Abandoned the unfinished registration of {} ({}), revision {}: its saved progress "
                             "({} files, {} bytes) was removed.",
                     abandoned->label, abandoned->archive_uuid, abandoned->revision, abandoned->files,
                     abandoned->bytes)
              << '\n';
    if (abandoned->first_registration) {
        std::cout << "  " << text("The archive had no revision: it is no longer listed.") << '\n';
        // The media is never written to remove it (R-03).
        if (abandoned->media_copy) {
            std::cout << "  "
                      << text("note: the media may still hold .veriarchive/identity.json naming this archive; "
                              "delete it before registering the folder or volume again, unless you register it "
                              "with --write-media-copy.")
                      << '\n';
        }
    }
    return exit_code::passed;
}

// An unfinished relocation as --json shows it, abandoned or to be.
nlohmann::json abandonable_relocation_json(const AbandonableRelocation& relocation)
{
    return {
        {"archive_uuid", relocation.archive_uuid},
        {"label", relocation.label},
        {"revision", relocation.revision},
        {"root", relocation.root},
        {"run_id", relocation.run_id},
        {"started", relocation.started},
        {"saved_progress", {{"files", relocation.files}, {"bytes", relocation.bytes}}},
    };
}

// `archive abandon-relocation <uuid> [--confirm]` (decision 0026): gives up an
// unfinished relocation on purpose, such as one whose new carrier failed. Its
// record and saved progress go; the archive, its revisions, manifests and the
// history stay. Without --confirm it only says what would be lost (exit 12).
int abandon_relocation_command(const Arguments& arguments)
{
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto archive_uuid = find_archive_uuid(*store, arguments.words[2]);
    if (!archive_uuid) {
        return report_failure(arguments, archive_uuid.error());
    }
    const auto processes = platform::make_process_table();
    AbandonServices services{*store, *processes, host_info().host};
    const RelocationAbandonRequest request{*archive_uuid, std::nullopt};
    const auto failed = [&](const AbandonFailure& failure) {
        return report_failure(arguments, {failure.kind == AbandonFailure::Kind::Refused ? exit_code::identity_decision
                                                                                       : exit_code::repository,
                                             failure.message});
    };
    const auto& text = arguments.text;

    // The confirmation: what would be lost, and how to go ahead.
    if (!arguments.has("--confirm")) {
        const auto relocation = abandonable_relocation(services, request);
        if (!relocation) {
            return failed(relocation.error());
        }
        const std::string confirm = "run the command again with --confirm to abandon it";
        if (arguments.json) {
            print_json({{"error", confirm}, {"exit_code", exit_code::identity_decision},
                {"relocation", abandonable_relocation_json(*relocation)}});
            return exit_code::identity_decision;
        }
        std::cout << text.format("Unfinished relocation of {} ({}), revision {}", relocation->label,
                         relocation->archive_uuid, relocation->revision)
                  << "\n  " << text.format("new root: {}", relocation->root)
                  << "\n  " << text.format("started: {}", app::local_time(relocation->started, text))
                  << "\n  "
                  << text.format("saved progress: {} files, {} bytes", relocation->files, relocation->bytes)
                  << '\n'
                  << text("Abandoning it removes its saved progress, and it can never be resumed afterwards. The "
                          "archive, its revisions, manifests and the verification history stay as they are.")
                  << '\n';
        return report_failure(arguments, {exit_code::identity_decision, confirm});
    }

    const auto abandoned = abandon_relocation(services, request);
    if (!abandoned) {
        return failed(abandoned.error());
    }
    if (arguments.json) {
        print_json(abandonable_relocation_json(*abandoned));
        return exit_code::passed;
    }
    std::cout << text.format("Abandoned the unfinished relocation of {} ({}): its saved progress ({} files, {} "
                             "bytes) was removed.",
                     abandoned->label, abandoned->archive_uuid, abandoned->files, abandoned->bytes)
              << '\n';
    return exit_code::passed;
}

} // namespace

// Decision 0004: accept a copy (or a new enclosure) as the archive's carrier
// after a FULL verify of every entry; VeriArchive itself copies nothing. With
// --resume it continues the archive's unfinished relocation without reading
// again what matched before (decision 0026).
int relocate(const Arguments& arguments, Opened& opened)
{
    const auto status = find_status(opened.statuses, arguments.words[2]);
    if (!status) {
        return report_failure(arguments, status.error());
    }
    // `archive relocate <uuid> --resume <root_path>` reads the root as the option's value.
    const auto root_text = arguments.words.size() == 4 ? arguments.words[3] : arguments.value("--resume").value_or("");
    RelocateRequest request{status->archive.archive_uuid, app::typed_root(root_text), std::nullopt};
    request.resume = arguments.has("--resume") || arguments.value("--resume").has_value();
    // R-111: the config's per-device override; --io wins over it.
    request.rotational_override = opened.session.context.config.rotational_override;
    if (const auto io = arguments.value("--io")) {
        if (*io != "hdd" && *io != "ssd") {
            return report_failure(arguments, {exit_code::usage, "--io takes hdd or ssd"});
        }
        request.rotational = *io == "hdd";
    }
    // Decision 0029: a new root on a network share, or one written in the
    // \\?\ form, is refused before anything of it is read, and so is one that
    // is no folder.
    if (const auto problem = app::root_not_taken(request.root, root_text)) {
        return report_failure(arguments, {exit_code::usage, *problem});
    }
    const auto archive_files = platform::make_file_system_access();
    const auto repository_files = platform::make_file_system_access();
    const auto media = platform::make_media_enumerator();
    const auto processes = platform::make_process_table();
    RelocateServices services{*archive_files, *repository_files, *opened.session.writer, *media, opened.store,
        opened.session.context.repo, opened.session.context.repository_uuid, host_info(), *processes};
    CancelToken cancel;
    const platform::CancelOnInterrupt interrupt(cancel);
    CliProgress progress(arguments);
    const auto outcome = relocate_archive(services, request, cancel, progress);
    progress.finish();

    const auto& text = arguments.text;
    if (!outcome) {
        // A new carrier that differs (decision 0004): each entry that
        // differs, in text with its detail as a verification lists it, and
        // in --json as a verification's `items`.
        if (outcome.error().kind == RelocateFailure::Kind::Differs) {
            if (arguments.json) {
                auto items = nlohmann::json::array();
                for (const auto& item : outcome.error().items) {
                    items.push_back({{"p", item.path.p}, {"status", item.status}, {"detail", item.detail}});
                }
                print_json({{"error", outcome.error().message}, {"exit_code", exit_code::failed},
                    {"items", std::move(items)}});
                return exit_code::failed;
            }
            std::cout << text.format("RELOCATION REFUSED: {}", text(outcome.error().message)) << '\n';
            for (const auto& item : outcome.error().items) {
                std::cout << "  " << text(item.status) << "  " << item.path.p
                          << (item.detail.empty() ? ""
                                                  : "  " + text.parenthesized(text(app::item_detail_words(item.detail))))
                          << '\n';
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
            {"result", outcome->result}, {"run_id", outcome->run_id}, {"warnings", outcome->warnings},
            {"manifest", path_to_utf8(outcome->manifest)}, {"counts", outcome->counts},
            {"resumed", outcome->resumed}});
    } else {
        std::cout << text.format("Relocated {} ({}) to {} as revision {}", outcome->label, outcome->archive_uuid,
                         path_to_utf8(request.root), outcome->revision)
                  << "\n  "
                  << text.format("FULL VERIFY {} of the new carrier (run {})", text(outcome->result),
                         outcome->run_id)
                  << '\n';
        for (const auto& warning : outcome->warnings) {
            std::cout << "  " << text.format("warning: {}", text(warning)) << '\n';
        }
        for (const auto& item : outcome->items) {
            std::cout << "  " << text(item.status) << "  " << item.path.p
                      << (item.detail.empty() ? ""
                                              : "  " + text.parenthesized(text(app::item_detail_words(item.detail))))
                      << '\n';
        }
    }
    return outcome->result == "PASSED" ? exit_code::passed : exit_code::passed_with_warnings;
}

int archive_command(const Arguments& arguments)
{
    // `relocate <uuid> <root_path> [--resume]`, or `relocate <uuid> --resume <root_path>`.
    const bool resume_root = arguments.value("--resume").has_value();
    const bool relocate_command = arguments.words.size() >= 2 && arguments.words[1] == "relocate" &&
                                  ((arguments.words.size() == 4 && !resume_root) ||
                                      (arguments.words.size() == 3 && resume_root));
    const bool abandon_command = arguments.words.size() == 3 && arguments.words[1] == "abandon-registration";
    const bool relocation_abandon_command =
        arguments.words.size() == 3 && arguments.words[1] == "abandon-relocation";
    // `retire` takes --confirm, needed where the archive's latest manifest is
    // lost (decision 0028, point 9).
    const bool retire_command = arguments.words.size() == 3 && arguments.words[1] == "retire";
    const bool confirmed_command = abandon_command || relocation_abandon_command || retire_command;
    if (const auto problem = check_options(arguments,
            confirmed_command  ? std::initializer_list<std::string_view>{"--confirm"}
            : relocate_command ? std::initializer_list<std::string_view>{"--resume"}
                               : std::initializer_list<std::string_view>{},
            relocate_command ? std::initializer_list<std::string_view>{"--io", "--resume"}
                             : std::initializer_list<std::string_view>{})) {
        return report_failure(arguments, *problem);
    }
    const bool list_command = arguments.words.size() == 2 && arguments.words[1] == "list";
    const bool uuid_command = arguments.words.size() == 3 &&
                              (arguments.words[1] == "show" || arguments.words[1] == "retire");
    const bool compare_command = arguments.words.size() == 4 && arguments.words[1] == "compare";
    if (!list_command && !uuid_command && !relocate_command && !compare_command && !abandon_command &&
        !relocation_abandon_command) {
        return report_failure(arguments, {exit_code::usage,
            "usage: archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid> <root_path> [--io "
            "hdd|ssd] [--resume] | compare <uuid> <uuid> | abandon-registration <uuid> [--confirm] | "
            "abandon-relocation <uuid> [--confirm]"});
    }
    // Compare needs no verification status of every archive, only the two
    // manifests; abandoning only the archive's unfinished registration or
    // relocation.
    if (compare_command) {
        return compare(arguments);
    }
    if (abandon_command) {
        return abandon(arguments);
    }
    if (relocation_abandon_command) {
        return abandon_relocation_command(arguments);
    }
    // `list` shows every archive's latest header; `show` the named archive's;
    // `retire` and `relocate` none (a relocation reads what it matches itself).
    auto opened = list_command ? open_statuses(arguments)
                               : open_statuses(arguments, arguments.words[2], arguments.words[1] == "show",
                                     arguments.words[1] == "show");
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
    // The history is in the repository's records alone (R-18): no manifest is read.
    auto opened = open_statuses(arguments, arguments.words[1], false);
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
            // The relocation a run belongs to (decision 0026): an interrupted
            // attempt, or the run that recorded the relocation's revision.
            const auto part = relocation_part(run);
            entry["relocation"] = part ? nlohmann::json{{"run_id", part->relocation},
                                             {"from_revision", part->from_revision},
                                             {"interrupted_attempt", part->interrupted_attempt}}
                                       : nlohmann::json(nullptr);
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
        const auto note = app::history_note(text, run);
        std::cout << "  "
                  << text.format("{}  {} VERIFY {}  r{}  {} bytes", app::local_time(run.finished, text),
                         text(run.mode), text(run.result), run.revision, run.coverage_bytes.value_or(0))
                  << (run.failure_classes.empty() ? "" : "  " + app::failure_classes_words(text, run.failure_classes))
                  << "  " << run.run_id << (note.empty() ? "" : "  " + note) << '\n';
    }
    return exit_code::passed;
}

} // namespace va::cli
