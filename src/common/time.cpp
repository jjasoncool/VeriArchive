#include "common/time.h"

#include <chrono>
#include <format>

namespace va {

std::string current_timestamp()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", now);
}

} // namespace va
