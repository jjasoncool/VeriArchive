#include "app/local_time.h"

#include "common/time.h"
#include "platform/platform_services.h"

#include <format>

namespace va::app {
namespace {

// "UTC", "UTC+8", "UTC-5", "UTC+5:30".
std::string offset_text(std::chrono::seconds offset)
{
    const auto total_minutes = std::chrono::duration_cast<std::chrono::minutes>(offset).count();
    if (total_minutes == 0) {
        return "UTC";
    }
    const char sign = total_minutes < 0 ? '-' : '+';
    const auto magnitude = total_minutes < 0 ? -total_minutes : total_minutes;
    const auto hours = magnitude / 60;
    const auto minutes = magnitude % 60;
    return minutes == 0 ? std::format("UTC{}{}", sign, hours) : std::format("UTC{}{}:{:02}", sign, hours, minutes);
}

} // namespace

std::string format_local_time(std::string_view utc_timestamp, std::chrono::seconds offset, const Text& text)
{
    const auto moment = parse_timestamp(utc_timestamp);
    if (!moment) {
        return std::string(utc_timestamp);
    }
    // A sys_seconds value that holds the local clock reading, only for writing it out.
    const auto local = *moment + offset;
    // Chinese full-width brackets carry their own spacing; English needs a space.
    const char* separator = text.language() == Language::English ? " " : "";
    return std::format("{:%F %T}", local) + separator + text.parenthesized(offset_text(offset));
}

std::string local_time(std::string_view utc_timestamp, const Text& text)
{
    const auto moment = parse_timestamp(utc_timestamp);
    if (!moment) {
        return std::string(utc_timestamp);
    }
    return format_local_time(utc_timestamp, platform::utc_offset(*moment), text);
}

std::chrono::year_month_day local_date(std::chrono::sys_seconds moment, std::chrono::seconds offset)
{
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(moment + offset)};
}

std::chrono::year_month_day local_date(std::chrono::sys_seconds moment)
{
    return local_date(moment, platform::utc_offset(moment));
}

} // namespace va::app
