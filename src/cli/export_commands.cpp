// `export-sha256sums <uuid> --out FILE`: a GNU sha256sum file for the latest
// revision (R-67). Check it from the archive root with `sha256sum -c FILE`.
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/sha256sums.h"
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

} // namespace va::cli
