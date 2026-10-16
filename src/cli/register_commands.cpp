// `register <root_path>`: create an archive (or a new revision of one) from a
// directory tree (M3). Exit code 2 when some files could not be read at
// registration (decision 0010).
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
        return exit_code::identity_decision;
    case RegisterFailure::Kind::Incomplete:
        return exit_code::incomplete;
    default:
        return exit_code::repository;
    }
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
    return {
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
}

} // namespace

int register_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments,
            {"--new-revision", "--force-new-archive", "--resume", "--write-media-copy"}, {"--label", "--io", "--copy-of", "--resume"})) {
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
            "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume]"});
    }

    RegisterRequest request;
    request.root = user_path(words[1]);
    request.resume = arguments.has("--resume") || arguments.value("--resume").has_value();
    request.write_media_copy = arguments.has("--write-media-copy");
    request.label = arguments.value("--label");
    request.copy_of = arguments.value("--copy-of");
    request.new_revision = arguments.has("--new-revision");
    request.force_new_archive = arguments.has("--force-new-archive");
    if (const auto io = arguments.value("--io")) {
        if (*io != "hdd" && *io != "ssd") {
            return report_failure(arguments, {exit_code::usage, "--io takes hdd or ssd"});
        }
        request.rotational = *io == "hdd";
    }
    // The error_code overload: a root that cannot be checked is reported, not thrown.
    std::error_code root_error;
    if (!std::filesystem::is_directory(request.root, root_error)) {
        return report_failure(arguments, {exit_code::usage, "not a directory: " + words[1]});
    }

    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
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
