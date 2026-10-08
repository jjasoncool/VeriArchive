// `repo init | info | rebuild-index | verify-history`.
// info shows the active RepositoryRoot, its uuid and how it was selected (R-85).
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/engine/rebuild_index.h"
#include "platform/file_system.h"

#include <iostream>

namespace va::cli {
namespace {

int info(const Arguments& arguments, const Session& session)
{
    const auto& context = session.context;
    if (arguments.json) {
        print_json({
            {"repository_root", path_to_utf8(context.repo.root)},
            {"repository_uuid", context.repository_uuid},
            {"repo_selected_by", context.repo_selected_by},
            {"config_file", path_to_utf8(context.config_file)},
        });
    } else {
        std::cout << "RepositoryRoot: " << path_to_utf8(context.repo.root)
                  << "\nrepository_uuid: " << context.repository_uuid
                  << "\nrepo_selected_by: " << context.repo_selected_by
                  << "\nconfig_file: " << path_to_utf8(context.config_file)
                  << "\nBack up RepositoryRoot: losing it loses all verification history.\n";
    }
    return exit_code::passed;
}

// R-18 / R-90: recompute the hash chain over every verification run.
int verify_history(const Arguments& arguments, const Session& session)
{
    auto store = ArchiveStore::open(session.context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto check = store->check_history(session.context.repository_uuid);
    if (!check) {
        return report_failure(arguments, {exit_code::repository, check.error().message});
    }
    const bool intact = !check->first_broken_run;
    if (arguments.json) {
        print_json({{"runs", check->runs}, {"intact", intact},
            {"first_broken_run", check->first_broken_run.value_or("")}, {"problem", check->problem}});
    } else if (intact) {
        std::cout << "History intact: " << check->runs << " verification runs, hash chain verified.\n"
                  << "V1 detects edited or deleted rows; signed checkpoints arrive in V1.1.\n";
    } else {
        std::cout << "History BROKEN at run " << *check->first_broken_run << ": " << check->problem << '\n';
    }
    return intact ? exit_code::passed : exit_code::failed;
}

// R-80: rebuild index tables from manifests/.
int rebuild(const Arguments& arguments, const Session& session)
{
    auto store = ArchiveStore::open(session.context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    const auto files = platform::make_file_system_access();
    const auto report = rebuild_index(*store, *files, session.context.repo);
    if (!report) {
        return report_failure(arguments, {exit_code::repository, report.error().message});
    }
    if (arguments.json) {
        print_json({{"archives", report->archives}, {"revisions", report->revisions},
            {"entries", report->entries}, {"skipped", report->skipped}});
    } else {
        std::cout << "Rebuilt the index from manifests: " << report->archives << " archives, "
                  << report->revisions << " revisions, " << report->entries << " entries.\n";
        for (const auto& skipped : report->skipped) {
            std::cout << "  skipped: " << skipped << '\n';
        }
    }
    return report->skipped.empty() ? exit_code::passed : exit_code::passed_with_warnings;
}

} // namespace

int repo_command(const Arguments& arguments)
{
    if (const auto problem = check_options(arguments, {}, {})) {
        return report_failure(arguments, *problem);
    }
    const auto action = arguments.words.size() == 2 ? arguments.words[1] : std::string{};
    if (action != "init" && action != "info" && action != "rebuild-index" && action != "verify-history") {
        return report_failure(arguments, {exit_code::usage,
            "usage: repo init | info | rebuild-index | verify-history"});
    }

    auto session = open_session(arguments, action == "init");
    if (!session) {
        return report_failure(arguments, session.error());
    }
    if (action == "rebuild-index") {
        return rebuild(arguments, *session);
    }
    if (action == "verify-history") {
        return verify_history(arguments, *session);
    }
    return info(arguments, *session);
}

} // namespace va::cli
