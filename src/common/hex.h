#pragma once
// Lowercase hexadecimal encoding, used for hashes and for raw (non-UTF-8) names.
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace va {

std::string to_hex(std::span<const std::byte> bytes);
std::string to_hex(std::string_view bytes);

// Accepts lowercase or uppercase digits; returns nullopt for odd length or a non-hex digit.
std::optional<std::vector<std::byte>> from_hex(std::string_view text);

} // namespace va
