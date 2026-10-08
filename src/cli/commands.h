#pragma once
// One function per command group; each returns the process exit code.
#include "cli/cli_support.h"

namespace va::cli {

int repo_command(const Arguments& arguments);
int media_command(const Arguments& arguments);

} // namespace va::cli
