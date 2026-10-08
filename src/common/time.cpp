#include "common/time.h"

#include <chrono>
#include <format>

namespace va {

std::string current_timestamp()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", now);
}

std::optional<std::chrono::sys_seconds> parse_timestamp(std::string_view text)
{
    // "YYYY-MM-DDTHH:MM:SSZ": fixed positions, so plain digit parsing suffices.
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t start, std::size_t length) -> std::optional<int> {
        int value = 0;
        for (std::size_t index = start; index < start + length; ++index) {
            if (text[index] < '0' || text[index] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (text[index] - '0');
        }
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year(*year),
        std::chrono::month(static_cast<unsigned>(*month)), std::chrono::day(static_cast<unsigned>(*day))};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    return std::chrono::sys_days(date) + std::chrono::hours(*hour) + std::chrono::minutes(*minute) +
           std::chrono::seconds(*second);
}

} // namespace va
