#pragma once
// Factory for the platform's durable file writer (see IFileWriter in core/interfaces.h).
#include "core/interfaces.h"

#include <memory>

namespace va::platform {

std::unique_ptr<IFileWriter> make_file_writer();

} // namespace va::platform
