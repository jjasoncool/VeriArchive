#pragma once
// Convenience for small files (repository.json, config.json): write all bytes
// through IFileWriter so the destination is replaced atomically and durably.
#include "core/interfaces.h"

#include <string_view>

namespace va {

std::expected<void, IoError> write_file_atomically(
    IFileWriter& writer,
    const std::filesystem::path& destination,
    std::string_view contents);

// Reads a small file completely (manifest copies, identity files).
std::expected<std::string, IoError> read_whole_file(
    IFileSystemAccess& files, const std::filesystem::path& path);

} // namespace va
