// Entry point shared by main_win.cpp and main_linux.cpp: parses the command line
// and dispatches to the command group named by the first word.
#include "cli/cli.h"

#include "cli/commands.h"

#include <iostream>

namespace va::cli {
namespace {

void print_usage()
{
    std::cout <<
        "Usage: veriarchive-cli [--config P] [--repository P] [--json] <command>\n"
        "\n"
        "  repo init | info\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "  archive list | show <uuid> | retire <uuid>\n"
        "  history <uuid>\n";
}

} // namespace

int run(std::vector<std::string> raw_arguments)
{
    std::string error;
    const auto arguments = parse_arguments(raw_arguments, error);
    if (!arguments) {
        std::cerr << "error: " << error << '\n';
        return exit_code::usage;
    }
    if (arguments->words.empty()) {
        print_usage();
        return raw_arguments.empty() ? exit_code::passed : exit_code::usage;
    }

    const auto& command = arguments->words.front();
    if (command == "repo") {
        return repo_command(*arguments);
    }
    if (command == "media") {
        return media_command(*arguments);
    }
    if (command == "register") {
        return register_command(*arguments);
    }
    if (command == "verify") {
        return verify_command(*arguments);
    }
    if (command == "archive") {
        return archive_command(*arguments);
    }
    if (command == "history") {
        return history_command(*arguments);
    }

    print_usage();
    return exit_code::usage;
}

} // namespace va::cli
