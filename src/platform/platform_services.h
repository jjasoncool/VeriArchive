#pragma once
#include <chrono>
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
// empty when unknown (decision 0017). On Linux it may be a ':'-separated
// priority list taken from LANGUAGE, such as "fr:zh_TW:en", which
// app::resolve_language splits.
std::string ui_language();
// How far the system's local time is ahead of UTC at `time`, with daylight
// saving time as it was then (Taiwan: +8 h). Zero when it cannot be determined.
std::chrono::seconds utc_offset(std::chrono::sys_seconds time);
// Makes std::cout and std::cerr show UTF-8 text correctly in a Windows console
// window, whatever its code page (often a legacy one such as Big5): text for a
// console is written as UTF-16 through WriteConsoleW. The console's code page is
// not changed, so there is nothing to restore at exit. Redirected output (a pipe
// or a file) stays UTF-8 bytes. Does nothing on Linux, where UTF-8 is the norm.
void prepare_console();

} // namespace va::platform