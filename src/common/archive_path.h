#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include "common/error.h"

namespace va {

struct ArchivePath {
    std::string p;
    std::string enc;
    std::string raw;
};

std::expected<ArchivePath, IoError> archive_path_from_native(const std::filesystem::path& relative);
std::expected<std::filesystem::path, IoError> archive_path_to_native(const ArchivePath& path);

} // namespace va