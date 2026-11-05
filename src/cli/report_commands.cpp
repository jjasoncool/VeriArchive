// `report <run_id> --out FILE.html` (decision 0048): the verification report
// of one recorded run that reached a result, as one self-contained HTML file,
// the same file that the window's Save report... makes (app/run_report_html.h),
// in the language of --lang or the config file. The run is named by its full
// id, as `history` lists it. Refused: an INCOMPLETE run, which claims nothing
// (R-21), an unknown run id, a FILE that is there already, which a report
// never replaces (the window's save dialog asks first; here nothing can), and
// a FILE in the root of an archive attached now (app::roots_holding), which
// its next FULL verification would report as added, as the window refuses
// it. It reads the run's records and writes one small file, so it shows no
// progress and Ctrl+C has nothing to stop (rule 7).
#include "cli/commands.h"

#include "app/archive_roots.h"
#include "app/run_report_html.h"
#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/archive_status.h"
#include "core/write_file.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

#include <filesystem>
#include <iostream>
#include <system_error>

namespace va::cli {

int report_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {}, {"--out"})) {
        return report_failure(arguments, *problem);
    }
    const auto out = arguments.value("--out");
    if (arguments.words.size() != 2 || !out) {
        return report_failure(arguments, {exit_code::usage, "usage: report <run_id> --out FILE.html"});
    }
    const auto& run_id = arguments.words[1];
    const auto destination = app::typed_path(*out);
    // A report never replaces a file: the one there may be another run's.
    std::error_code error;
    const auto there = std::filesystem::symlink_status(destination, error);
    if (std::filesystem::exists(there)) {
        return report_failure(arguments,
            {exit_code::usage, path_to_utf8(destination) + " exists already; a report never replaces a file, so name "
                                                           "another one"});
    }
    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    // Only read: the report changes nothing in the repository.
    auto store = ArchiveStore::open_read_only(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    auto facts = app::read_report_facts(*store, session->context.repository_uuid, run_id);
    if (!facts) {
        return report_failure(arguments, {exit_code::repository, facts.error().message});
    }
    if (!*facts) {
        return report_failure(arguments, {exit_code::usage, "no run " + run_id});
    }
    const auto& run = (*facts)->recorded.run;
    if (run.result == "INCOMPLETE") {
        return report_failure(arguments, {exit_code::usage, "run " + run_id +
                                                                " did not finish (INCOMPLETE); a report is made only "
                                                                "of a run that reached a result"});
    }
    // Never into an archive's root, as its volume is now, also through
    // another name of the folder (a symbolic link, a junction, subst).
    const auto repository_files = platform::make_file_system_access();
    const auto media = platform::make_media_enumerator();
    const auto folder = destination.parent_path();
    if (const auto holding = app::roots_holding(*store, *repository_files, *media, session->context.repo, folder);
        !holding.empty()) {
        return report_failure(
            arguments, {exit_code::usage, app::report_refusal_in_root(app::Text(), folder, holding.front())});
    }
    // The volume label of the archive's current carrier, from its manifest's
    // header, as the window names it for a run whose evidence names none.
    StatusSelection selection;
    selection.only = run.archive_uuid;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (const auto statuses = archive_statuses(
            *store, *repository_files, session->context.repo, session->context.config.policy, now, selection);
        statuses && !statuses->empty() && statuses->front().header) {
        (*facts)->recorded.volume_label = statuses->front().header->identity.volume_label;
    }
    const auto host = host_info();
    (*facts)->made = {current_timestamp(), host.host, host.app_version};
    const auto html = app::run_report_html(arguments.text, **facts);
    if (!html) {
        return report_failure(arguments, {exit_code::usage, html.error()});
    }
    if (auto written = write_file_atomically(*session->writer, destination, *html); !written) {
        return report_failure(arguments, {exit_code::repository, written.error().message});
    }
    if (arguments.json) {
        print_json({{"run_id", run.run_id}, {"archive_uuid", run.archive_uuid}, {"result", run.result},
            {"out", path_to_utf8(destination)}});
    } else {
        const auto& text = arguments.text;
        std::cout << text.format("Wrote the verification report of run {} ({}: {}) to {}", run.run_id,
                         (*facts)->recorded.label, app::result_with_mode(text, run), path_to_utf8(destination))
                  << '\n';
    }
    return exit_code::passed;
}

} // namespace va::cli
