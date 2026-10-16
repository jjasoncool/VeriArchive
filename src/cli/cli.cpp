// Entry point shared by main_win.cpp and main_linux.cpp: parses the command line
// and dispatches to the command group named by the first word.
#include "cli/cli.h"

#include "app/session.h"
#include "cli/commands.h"
#include "platform/platform_services.h"

#include <iostream>

namespace va::cli {
namespace {

void print_usage(const app::Text& text)
{
    std::cout << text(
        "Usage: veriarchive-cli [--config P] [--repository P] [--lang en|zh-TW] [--json] <command>\n"
        "       veriarchive-cli --version\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy]\n"
        "           [--resume [--confirm]] [--lost-manifest UUID [--confirm]]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID] [--add-chunk-hashes]\n"
        "  archive list | show <uuid> | retire <uuid> [--confirm]\n"
        "  archive relocate <uuid> <root_path> [--resume]\n"
        "  archive compare <uuid> <uuid>\n"
        "  archive abandon-registration <uuid> [--confirm] | abandon-relocation <uuid> [--confirm]\n"
        "  history <uuid>\n"
        "  export-sha256sums <uuid> --out FILE\n"
        "  export <uuid> --out DIR | import <file.vam> [--force-rollback]\n");
}

} // namespace

int run(std::vector<std::string> raw_arguments)
{
    platform::prepare_console();
    std::string error;
    auto arguments = parse_arguments(raw_arguments, error);
    // Chosen before a usage error is reported, so that error also follows --lang
    // and the --config file's language (decision 0017).
    arguments.text = app::Text(app::interface_language(arguments.language, arguments.config));
    // A usage error follows --json too (decision 0017): as JSON on stdout,
    // `error` and `exit_code`, as every command's failure.
    if (!error.empty()) {
        return report_failure(arguments, {exit_code::usage, error});
    }
    // The version of this program (decision 0037), as repo info and the
    // GUI's Settings show it.
    if (arguments.words.empty() && arguments.has("--version")) {
        if (arguments.json) {
            print_json({{"app_version", program_version()}});
        } else {
            std::cout << arguments.text.format("VeriArchive version {}", program_version()) << '\n';
        }
        return exit_code::passed;
    }
    if (arguments.words.empty()) {
        if (raw_arguments.empty()) {
            print_usage(arguments.text);
            return exit_code::passed;
        }
        if (arguments.json) {
            return report_failure(arguments,
                {exit_code::usage, "a command is needed; veriarchive-cli without arguments shows the commands"});
        }
        print_usage(arguments.text);
        return exit_code::usage;
    }

    const auto& command = arguments.words.front();
    if (command == "repo") {
        return repo_command(arguments);
    }
    if (command == "media") {
        return media_command(arguments);
    }
    if (command == "register") {
        return register_command(arguments);
    }
    if (command == "verify") {
        return verify_command(arguments);
    }
    if (command == "archive") {
        return archive_command(arguments);
    }
    if (command == "history") {
        return history_command(arguments);
    }
    if (command == "export-sha256sums") {
        return export_sha256sums_command(arguments);
    }
    if (command == "export") {
        return export_command(arguments);
    }
    if (command == "import") {
        return import_command(arguments);
    }

    // An unknown command: said, then the usage; with --json said as JSON.
    report_failure(arguments, {exit_code::usage, "unknown command " + command});
    if (!arguments.json) {
        print_usage(arguments.text);
    }
    return exit_code::usage;
}

} // namespace va::cli
