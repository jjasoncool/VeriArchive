// `register <root_path>`: create an archive (or a new revision of one) from a
// directory tree (M3). Exit code 2 when some files could not be read at
// registration (decision 0010). With `--lost-manifest <uuid>`, the next
// revision of an archive whose latest manifest is lost, once confirmed
// (decision 0028, point 8).
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/engine/register_engine.h"
#include "platform/file_system.h"
#include "platform/interrupt.h"
#include "platform/media_enumerator.h"
#include "platform/process_table.h"

#include <iostream>

namespace va::cli {
namespace {

int failure_code(RegisterFailure::Kind kind)
{
    switch (kind) {
    case RegisterFailure::Kind::Invalid:
        return exit_code::usage;
    case RegisterFailure::Kind::Refused:
    case RegisterFailure::Kind::Unconfirmed:
        return exit_code::identity_decision;
    case RegisterFailure::Kind::Incomplete:
        return exit_code::incomplete;
    default:
        return exit_code::repository;
    }
}

// A registration over a lost manifest as --json gives it before it is
// confirmed (decision 0028, point 8).
nlohmann::json lost_manifest_json(const LostManifestRegistration& registration)
{
    const auto& manifest = registration.manifest;
    auto clones = nlohmann::json::array();
    for (const auto& clone : registration.possible_clones) {
        clones.push_back({{"archive_uuid", clone.archive_uuid}, {"label", clone.label},
            {"match", std::string(to_string(clone.match))}});
    }
    auto same_root = nlohmann::json::array();
    for (const auto& same : registration.same_root) {
        same_root.push_back({{"archive_uuid", same.archive_uuid}, {"label", same.label},
            {"match", std::string(to_string(same.match))}});
    }
    return {
        {"archive_uuid", registration.archive_uuid},
        {"label", registration.label},
        {"lost_revision", registration.lost_revision},
        {"revision", registration.revision},
        {"root", registration.root},
        {"manifest_problem", {{"kind", kind_code(manifest.kind)}, {"revision", manifest.revision},
                                 {"manifest", manifest.manifest}, {"message", plain_message(manifest)}}},
        {"possible_clones", std::move(clones)},
        {"same_root", std::move(same_root)},
    };
}

// Decision 0028, point 8: what a registration over a lost manifest would do,
// said before the operator confirms it, as `archive abandon-registration`
// says what it would remove: the root's content becomes the new baseline
// without being compared with the lost revision.
int unconfirmed(const Arguments& arguments, const RegisterFailure& failure)
{
    const auto& registration = *failure.lost_manifest;
    const int code = failure_code(failure.kind);
    if (arguments.json) {
        print_json({{"error", failure.message}, {"exit_code", code}, {"registration", lost_manifest_json(registration)}});
        return code;
    }
    const auto& text = arguments.text;
    std::cout << text.format("Lost manifest of {} ({}), revision {}", registration.label, registration.archive_uuid,
                     registration.lost_revision)
              << "\n  " << text(what_is_wrong_with(registration.manifest)) << "\n  "
              << text.format("root: {}", registration.root) << '\n';
    // An archive whose root this root overlaps on a volume that may be a
    // clone of this one, or this one itself: the operator, who knows which
    // device is which, confirms only where the root is not that archive's.
    for (const auto& clone : registration.possible_clones) {
        std::cout << "  "
                  << text.format("possibly a clone: this root overlaps the root of archive {} ({}) on a volume that "
                                 "this one matches only as {}: this volume may be a sector-by-sector clone of that "
                                 "archive's, or that archive's own; confirm only where this root is not that "
                                 "archive's",
                         clone.label, clone.archive_uuid, text(to_string(clone.match)))
                  << '\n';
    }
    // An archive whose very root this root is: the archive named may be its
    // twin, the same folder registered on purpose as both; the operator, who
    // named the archive, confirms only where it is. On a volume that this one
    // matches only as MIGRATED or MATCH_WEAK, this volume may be a clone of
    // that archive's as well, or that archive's own: the operator confirms
    // where it is not that archive's too.
    for (const auto& same : registration.same_root) {
        std::cout << "  "
                  << (same.match == MatchResult::Match
                             ? text.format("the same root as archive {} ({}): this root is that archive's, on the volume "
                                           "and the device that its latest manifest records; confirm only where this "
                                           "very folder was registered on purpose as both that archive and this one "
                                           "(--force-new-archive)",
                                   same.label, same.archive_uuid)
                             : text.format("the same root as archive {} ({}) on a volume that this one matches only as "
                                           "{}: this volume may be a sector-by-sector clone of that archive's, or that "
                                           "archive's own; confirm only where this very folder was registered on "
                                           "purpose as both that archive and this one (--force-new-archive), or where "
                                           "this volume is not that archive's",
                                   same.label, same.archive_uuid, text(to_string(same.match))))
                  << '\n';
    }
    std::cout << text.format("Registering revision {} from this root takes what it holds now as the archive's new "
                             "baseline, without comparing it with revision {}, whose manifest is lost: damage since "
                             "revision {} was registered would not be found. Do this only where no backup of the "
                             "repository, export of revision {} or media copy holds that manifest. The archive "
                             "keeps its verification history and its hash chain, and never goes back to an earlier "
                             "revision.",
                     registration.revision, registration.lost_revision, registration.lost_revision,
                     registration.lost_revision)
              << '\n';
    return report_failure(arguments, {code, failure.message});
}

void print_outcome(const app::Text& text, const RegisterOutcome& outcome, const std::filesystem::path& root)
{
    std::cout << text.format("Registered {} ({}) revision {}", outcome.label, outcome.archive_uuid,
                     outcome.revision)
              << "\n  " << text.format("root: {}", path_to_utf8(root))
              << "\n  " << text.format("files: {}  empty directories: {}  links: {}  hashed bytes: {}",
                     outcome.counts.files, outcome.counts.dirs, outcome.counts.links, outcome.counts.bytes)
              << "\n  " << text.format("manifest: {}", path_to_utf8(outcome.manifest)) << '\n';
    if (!outcome.unreadable.empty()) {
        std::cout << "  "
                  << text.format("unreadable at registration ({}; recorded without hashes, never reported "
                                 "as verified):",
                         outcome.unreadable.size())
                  << '\n';
        for (const auto& entry : outcome.unreadable) {
            std::cout << "    " << text(entry.unreadable) << "  " << entry.path.p << '\n';
        }
    }
    for (const auto& mount_point : outcome.skipped_mount_points) {
        std::cout << "  " << text.format("skipped mount point: {}", mount_point.p) << '\n';
    }
    for (const auto& warning : outcome.warnings) {
        std::cout << "  " << text.format("warning: {}", text(warning)) << '\n';
    }
    // Decision 0028, point 8: the record of the revision whose manifest was
    // lost left the index, as `repo rebuild-index` leaves out an earlier
    // revision whose manifest it cannot read.
    if (outcome.lost_revision && outcome.lost_revision_left_out) {
        std::cout << "  "
                  << text.format("revision {}, whose manifest was lost, was left out of the index; its verifications "
                                 "stay in the history",
                         *outcome.lost_revision)
                  << '\n';
    }
    // D5: without a media copy, the repository holds the only copy of the baseline.
    if (outcome.media_copy.empty()) {
        std::cout << "  "
                  << text("note: the baseline exists only in the repository; back up RepositoryRoot "
                          "(or register with --write-media-copy to keep a copy on the media).")
                  << '\n';
    } else {
        std::cout << "  " << text.format("media copy: {}", path_to_utf8(outcome.media_copy)) << '\n';
    }
}

nlohmann::json outcome_json(const RegisterOutcome& outcome)
{
    auto unreadable = nlohmann::json::array();
    for (const auto& entry : outcome.unreadable) {
        unreadable.push_back({{"p", entry.path.p}, {"reason", entry.unreadable}});
    }
    auto mount_points = nlohmann::json::array();
    for (const auto& mount_point : outcome.skipped_mount_points) {
        mount_points.push_back(mount_point.p);
    }
    nlohmann::json json{
        {"archive_uuid", outcome.archive_uuid},
        {"label", outcome.label},
        {"revision", outcome.revision},
        {"counts", {{"files", outcome.counts.files}, {"dirs", outcome.counts.dirs},
                       {"links", outcome.counts.links}, {"bytes", outcome.counts.bytes}}},
        {"unreadable", std::move(unreadable)},
        {"skipped_mount_points", std::move(mount_points)},
        {"warnings", outcome.warnings},
        {"manifest", path_to_utf8(outcome.manifest)},
        {"media_copy", path_to_utf8(outcome.media_copy)},
    };
    // Only a registration over a lost manifest has it (decision 0028, point 8).
    if (outcome.lost_revision) {
        json["lost_manifest"] = {{"revision", *outcome.lost_revision}, {"left_out", outcome.lost_revision_left_out}};
    }
    return json;
}

} // namespace

int register_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments,
            {"--new-revision", "--force-new-archive", "--resume", "--write-media-copy", "--confirm"},
            {"--label", "--io", "--copy-of", "--resume", "--lost-manifest"})) {
        return report_failure(arguments, *problem);
    }
    // `register --resume <root>` reads the root as the option's value.
    auto words = arguments.words;
    if (const auto value = arguments.value("--resume"); value && words.size() == 1) {
        words.push_back(*value);
    }
    if (words.size() != 2) {
        return report_failure(arguments, {exit_code::usage,
            "usage: register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision] "
            "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume] "
            "[--lost-manifest UUID [--confirm]]"});
    }

    RegisterRequest request;
    request.root = app::typed_root(words[1]);
    request.resume = arguments.has("--resume") || arguments.value("--resume").has_value();
    request.write_media_copy = arguments.has("--write-media-copy");
    request.label = arguments.value("--label");
    request.copy_of = arguments.value("--copy-of");
    request.new_revision = arguments.has("--new-revision");
    request.force_new_archive = arguments.has("--force-new-archive");
    request.confirmed = arguments.has("--confirm");
    if (const auto io = arguments.value("--io")) {
        if (*io != "hdd" && *io != "ssd") {
            return report_failure(arguments, {exit_code::usage, "--io takes hdd or ssd"});
        }
        request.rotational = *io == "hdd";
    }
    // Decision 0029: a root on a network share, or written in the \\?\ form,
    // is refused before anything of it is read, and so is one that is no
    // folder. The Archive List's Resume asks the same (decision 0022, point 7).
    if (const auto problem = app::root_not_taken(request.root, words[1])) {
        return report_failure(arguments, {exit_code::usage, *problem});
    }

    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    // The archive whose manifest is lost, named as `archive show` names one:
    // its uuid, or a unique prefix of at least 8 characters.
    if (const auto lost = arguments.value("--lost-manifest")) {
        auto archive_uuid = find_archive_uuid(*store, *lost);
        if (!archive_uuid) {
            return report_failure(arguments, archive_uuid.error());
        }
        request.lost_manifest_of = std::move(*archive_uuid);
    }
    const auto archive_files = platform::make_file_system_access();
    const auto repository_files = platform::make_file_system_access();
    const auto media = platform::make_media_enumerator();
    const auto processes = platform::make_process_table();
    RegisterServices services{*archive_files, *repository_files, *session->writer, *session->writer, *media,
        *store, session->context.repo, host_info(), *processes};

    CancelToken cancel;
    platform::cancel_on_interrupt(cancel);
    CliProgress progress(arguments);
    const auto outcome = register_archive(services, request, cancel, progress);
    progress.finish();
    if (!outcome) {
        if (outcome.error().kind == RegisterFailure::Kind::Unconfirmed && outcome.error().lost_manifest) {
            return unconfirmed(arguments, outcome.error());
        }
        return report_failure(arguments, {failure_code(outcome.error().kind), outcome.error().message});
    }

    if (arguments.json) {
        print_json(outcome_json(*outcome));
    } else {
        print_outcome(arguments.text, *outcome, request.root);
    }
    return outcome->unreadable.empty() ? exit_code::passed : exit_code::passed_with_warnings;
}

} // namespace va::cli
