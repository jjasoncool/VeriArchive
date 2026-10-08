#pragma once
#include <filesystem>
#include <expected>
#include <string>
#include "common/error.h"

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path();
std::expected<std::filesystem::path, IoError> user_config_dir();
std::expected<std::filesystem::path, IoError> user_home();
bool writable_directory(const std::filesystem::path& directory);
bool network_filesystem(const std::filesystem::path& path);
bool process_is_elevated();

// Recorded in manifests and history rows (created_by).
std::string os_name();   // "windows" or "linux"
std::string host_name();

// The operating system's interface language, e.g. "zh-TW" or "en_US.UTF-8";
// empty when unknown (decision 0017).
std::string ui_language();
// Makes the console show UTF-8 text (Windows consoles default to a legacy code
// page such as Big5); does nothing where UTF-8 is already the norm.
void prepare_console();

} // namespace va::platform