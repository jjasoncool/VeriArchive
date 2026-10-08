#pragma once
// One function per command group; each returns the process exit code.
#include "cli/cli_support.h"

namespace va::cli {

int repo_command(const Arguments& arguments);
int media_command(const Arguments& arguments);
int register_command(const Arguments& arguments);
int verify_command(const Arguments& arguments);
int archive_command(const Arguments& arguments);
int history_command(const Arguments& arguments);
int export_sha256sums_command(const Arguments& arguments);
int export_command(const Arguments& arguments);
int import_command(const Arguments& arguments);

} // namespace va::cli
