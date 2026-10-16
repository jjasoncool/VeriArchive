#pragma once
// Factory for the platform's process table (IProcessTable in core/interfaces.h):
// which process runs an attempt of a registration, and whether it still runs
// (decision 0024).
#include "core/interfaces.h"

#include <memory>

namespace va::platform {

std::unique_ptr<IProcessTable> make_process_table();

} // namespace va::platform
