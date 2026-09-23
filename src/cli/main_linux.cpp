#include "cli/cli.h"

int main(int argc, char* argv[]) {
    return va::cli::run(std::vector<std::string>(argv + 1, argv + argc));
}