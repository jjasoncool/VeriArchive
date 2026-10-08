#pragma once
// Text encoding helpers. VeriArchive keeps every string in UTF-8 internally;
// these functions convert native names (UTF-16 on Windows, raw bytes on Linux)
// without ever throwing on malformed input.
#include <filesystem>
#include <string>
#include <string_view>

namespace va {

// Display/persistence form of a path. Malformed native names are converted
// lossily (invalid sequences become U+FFFD) instead of failing.
std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view value);

bool is_valid_utf8(std::string_view bytes);

// Replaces every invalid UTF-8 sequence with U+FFFD.
std::string utf8_lossy(std::string_view bytes);

// Converts UTF-16 code units to UTF-8. An unpaired surrogate becomes U+FFFD and
// sets *lossless to false (when lossless is not null).
std::string utf16_to_utf8(std::u16string_view units, bool* lossless = nullptr);
std::u16string utf8_to_utf16(std::string_view utf8);

} // namespace va
