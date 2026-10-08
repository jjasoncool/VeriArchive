#pragma once
// Factory for the platform's read-side file access (IFileSystemAccess in
// core/interfaces.h): directory listing, uncached reads, link targets.
#include "core/interfaces.h"

#include <memory>

namespace va::platform {

std::unique_ptr<IFileSystemAccess> make_file_system_access();

} // namespace va::platform
