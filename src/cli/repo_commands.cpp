// `repo init` and `repo info` (R-85: show RepositoryRoot, repository_uuid and
// how the repository was selected).
#include "cli/commands.h"

#include "common/utf8.h"

#include <iostream>

namespace va::cli {

int repo_command(const Arguments& arguments)
{
    if (arguments.words.size() != 2 ||
        (arguments.words[1] != "init" && arguments.words[1] != "info")) {
        return report_failure(arguments, {exit_code::usage, "usage: repo init|info"});
    }

    auto session = open_session(arguments, arguments.words[1] == "init");
    if (!session) {
        return report_failure(arguments, session.error());
    }
    const auto& context = session->context;

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

} // namespace va::cli
