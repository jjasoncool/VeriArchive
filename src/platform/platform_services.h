#pragma once
#include <filesystem>
#include <expected>
#include "common/error.h"

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path();
std::expected<std::filesystem::path, IoError> user_config_dir();
std::expected<std::filesystem::path, IoError> user_home();
bool writable_directory(const std::filesystem::path& directory);
bool network_filesystem(const std::filesystem::path& path);
bool process_is_elevated();

} // namespace va::platform