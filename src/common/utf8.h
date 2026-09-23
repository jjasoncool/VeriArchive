#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace va {

std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view value);

} // namespace va