#pragma once
// Timestamps are UTC, ISO 8601, whole seconds: "2026-10-08T05:00:00Z".
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace va {

std::string current_timestamp();

// Parses the format above; nullopt for anything else.
std::optional<std::chrono::sys_seconds> parse_timestamp(std::string_view text);

} // namespace va
