#include "cli/cli.h"
#include "common/utf8.h"

#include <filesystem>

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        arguments.push_back(va::path_to_utf8(std::filesystem::path(argv[index])));
    }
    return va::cli::run(std::move(arguments));
}