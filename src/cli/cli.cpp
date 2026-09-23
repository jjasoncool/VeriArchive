#include "common/utf8.h"
#include "core/repository/repository.h"
#include "platform/platform_services.h"
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace va::cli {

namespace {

constexpr int usage_error = 10;
constexpr int repository_error = 11;

void print_usage() {
    std::cout << "Usage: veriarchive-cli [--config P] [--repository P] repo init|info\n";
}

bool parse_global_options(
    const std::vector<std::string>& arguments,
    RepositoryOptions& options,
    std::size_t& command_index) {
    while (command_index < arguments.size() && arguments[command_index].starts_with("--")) {
        const auto& option = arguments[command_index];
        if ((option != "--repository" && option != "--config") ||
            command_index + 1 >= arguments.size()) {
            return false;
        }

        const auto path = path_from_utf8(arguments[command_index + 1]);
        if (option == "--repository") {
            options.explicit_repository = path;
        } else {
            options.explicit_config = path;
        }
        command_index += 2;
    }
    return true;
}

bool is_repository_command(
    const std::vector<std::string>& arguments,
    std::size_t command_index) {
    if (command_index + 2 != arguments.size() || arguments[command_index] != "repo") {
        return false;
    }
    const auto& action = arguments[command_index + 1];
    return action == "init" || action == "info";
}

} // namespace

int run(std::vector<std::string> arguments) {
    RepositoryOptions options;
    const auto executable = platform::executable_path();
    const auto config_directory = platform::user_config_dir();
    const auto home = platform::user_home();
    if (!executable || !config_directory || !home) {
        const auto& error = !executable ? executable.error() :
                            !config_directory ? config_directory.error() : home.error();
        std::cerr << error.message << '\n';
        return repository_error;
    }
    options.executable_dir = executable->parent_path();
    options.user_config_dir = *config_directory;
    options.user_home = *home;

    if (const char* environment_repository = std::getenv("VERIARCHIVE_REPOSITORY")) {
        options.environment_repository = path_from_utf8(environment_repository);
    }

    std::size_t command_index = 0;
    if (!parse_global_options(arguments, options, command_index)) {
        std::cerr << "unknown or incomplete option\n";
        return usage_error;
    }
    if (!is_repository_command(arguments, command_index)) {
        print_usage();
        return arguments.empty() ? 0 : usage_error;
    }

    const RepositoryProbes probes{
        platform::writable_directory,
        platform::network_filesystem,
    };
    options.initialise = arguments[command_index + 1] == "init";
    auto context = resolve_repository(options, probes);
    if (!context) {
        std::cerr << context.error().message << '\n';
        return repository_error;
    }

    auto result = options.initialise ? initialise_repository(*context) : open_repository(*context);
    if (!result) {
        std::cerr << result.error().message << '\n';
        return repository_error;
    }

    if (platform::process_is_elevated()) {
        std::cerr << "warning: VeriArchive is running elevated; "
                     "repository and home resolution may differ\n";
    }

    std::cout << "RepositoryRoot: " << path_to_utf8(context->repo.root)
              << "\nrepository_uuid: " << context->repository_uuid
              << "\nrepo_selected_by: " << context->repo_selected_by << '\n';
    return 0;
}

} // namespace va::cli