// Moving data out of and into the repository:
//  - `export-sha256sums <uuid> --out FILE`: GNU sha256sum file for the latest
//    revision (R-67); check it from the archive root with `sha256sum -c FILE`.
//  - `export <uuid> --out DIR` and `import <file.vam> [--force-rollback]` (R-87).
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/sha256sums.h"
#include "core/engine/transfer.h"
#include "core/write_file.h"
#include "platform/file_system.h"

#include <iostream>

namespace va::cli {

int export_sha256sums_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {}, {"--out"})) {
        return report_failure(arguments, *problem);
    }
    const auto out = arguments.value("--out");
    if (arguments.words.size() != 2 || !out) {
        return report_failure(arguments, {exit_code::usage, "usage: export-sha256sums <uuid> --out FILE"});
    }
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto archive = store->find_archive(arguments.words[1]);
    if (!archive || !*archive) {
        return report_failure(arguments, {exit_code::usage, "no archive " + arguments.words[1]});
    }
    const auto files = platform::make_file_system_access();
    const auto manifest = load_recorded_manifest(*store, *files, session->context.repo,
        (*archive)->archive_uuid, (*archive)->current_revision);
    if (!manifest) {
        return report_failure(arguments, {exit_code::failed,
            manifest.error().failure_class + ": " + manifest.error().message});
    }

    const auto sums = sha256sums(manifest->entries);
    const auto destination = user_path(*out);
    if (auto written = write_file_atomically(*session->writer, destination, sums.text); !written) {
        return report_failure(arguments, {exit_code::repository, written.error().message});
    }
    if (arguments.json) {
        print_json({{"archive_uuid", (*archive)->archive_uuid}, {"out", path_to_utf8(destination)},
            {"skipped", sums.skipped}});
    } else {
        std::cout << "Wrote " << path_to_utf8(destination) << " for " << (*archive)->label
                  << " revision " << (*archive)->current_revision
                  << ". Check it from the archive root with: sha256sum -c <file>\n";
        for (const auto& skipped : sums.skipped) {
            std::cout << "  skipped: " << skipped << '\n';
        }
    }
    return exit_code::passed;
}

int export_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {}, {"--out"})) {
        return report_failure(arguments, *problem);
    }
    const auto out = arguments.value("--out");
    if (arguments.words.size() != 2 || !out) {
        return report_failure(arguments, {exit_code::usage, "usage: export <uuid> --out DIR"});
    }
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto files = platform::make_file_system_access();
    const auto exported = export_archive(*store, *files, *session->writer, session->context.repo,
        arguments.words[1], user_path(*out));
    if (!exported) {
        return report_failure(arguments, {exit_code::failed, exported.error()});
    }
    if (arguments.json) {
        print_json({{"manifest", path_to_utf8(exported->manifest)},
            {"identity", path_to_utf8(exported->identity)}});
    } else {
        std::cout << "Exported " << path_to_utf8(exported->manifest) << "\n     and "
                  << path_to_utf8(exported->identity) << '\n';
    }
    return exit_code::passed;
}

int import_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {"--force-rollback"}, {})) {
        return report_failure(arguments, *problem);
    }
    if (arguments.words.size() != 2) {
        return report_failure(arguments, {exit_code::usage, "usage: import <file.vam> [--force-rollback]"});
    }
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto files = platform::make_file_system_access();
    const auto imported = import_manifest(*store, *files, *session->writer, session->context.repo,
        user_path(arguments.words[1]), arguments.has("--force-rollback"));
    if (!imported) {
        return report_failure(arguments, {exit_code::failed,
            imported.error().failure_class + ": " + imported.error().message});
    }
    if (arguments.json) {
        print_json({{"archive_uuid", imported->archive_uuid}, {"label", imported->label},
            {"revision", imported->revision}, {"already_present", imported->already_present},
            {"new_archive", imported->new_archive}});
    } else if (imported->already_present) {
        std::cout << "Already present: " << imported->label << " (" << imported->archive_uuid
                  << ") revision " << imported->revision << ", identical bytes.\n";
    } else {
        std::cout << "Imported " << imported->label << " (" << imported->archive_uuid << ") revision "
                  << imported->revision << (imported->new_archive ? " as a new archive" : "") << ".\n";
    }
    return exit_code::passed;
}

} // namespace va::cli
