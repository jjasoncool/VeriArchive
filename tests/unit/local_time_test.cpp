#include "test_support.h"
#include "app/local_time.h"
#include "platform/platform_services.h"

#include <chrono>

namespace {

using namespace std::chrono_literals;
using va::app::format_local_time;
using va::app::Language;
using va::app::Text;

void formatting_tests()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto stamp = "2026-10-14T00:13:53Z";
    CHECK(format_local_time(stamp, 8h, english) == "2026-10-14 08:13:53 (UTC+8)");
    CHECK(format_local_time(stamp, 8h, chinese) == "2026-10-14 08:13:53（UTC+8）");
    // The local date can differ from the UTC date.
    CHECK(format_local_time(stamp, -5h, english) == "2026-10-13 19:13:53 (UTC-5)");
    CHECK(format_local_time(stamp, 5h + 30min, english) == "2026-10-14 05:43:53 (UTC+5:30)");
    CHECK(format_local_time(stamp, -(3h + 30min), english) == "2026-10-13 20:43:53 (UTC-3:30)");
    CHECK(format_local_time(stamp, 0s, english) == "2026-10-14 00:13:53 (UTC)");
    // Text that is not a timestamp is shown as it is.
    CHECK(format_local_time("", 8h, english).empty());
    CHECK(format_local_time("yesterday", 8h, chinese) == "yesterday");
}

// The date of a moment as the clock shows it where it is `offset` ahead of
// UTC, which names the copy of a database kept before its upgrade (decision
// 0032): the local date, which can differ from the UTC date.
void local_date_tests()
{
    using va::app::local_date;
    const auto evening = std::chrono::sys_days(2026y / 10 / 16) + 20h + 13min;
    CHECK(local_date(evening, 8h) == 2026y / 10 / 17);
    CHECK(local_date(evening, 0s) == 2026y / 10 / 16);
    CHECK(local_date(std::chrono::sys_days(2026y / 10 / 17) + 2h, -5h) == 2026y / 10 / 16);
    CHECK(local_date(std::chrono::sys_days(2026y / 10 / 17) + 2h, 5h + 30min) == 2026y / 10 / 17);
}

// The local date of a moment in the system's time zone is the date at the
// system's offset from UTC at that moment, daylight saving time included:
// every hour of a winter day and of a summer day, so that wherever the clock
// is not at UTC, some hour's local date is not its UTC date.
void system_local_date_tests()
{
    using va::app::local_date;
    for (const auto day : {std::chrono::sys_days(2026y / 1 / 15), std::chrono::sys_days(2026y / 7 / 15)}) {
        for (auto hour = 0h; hour < 24h; ++hour) {
            const std::chrono::sys_seconds moment = day + hour;
            CHECK(local_date(moment) == local_date(moment, va::platform::utc_offset(moment)));
        }
    }
}

// The system's offset is plausible: at most 14 hours, whole quarter hours,
// and summer and winter differ by at most two hours (daylight saving time).
void system_offset_tests()
{
    const auto winter = va::platform::utc_offset(std::chrono::sys_days(2026y / 1 / 15));
    const auto summer = va::platform::utc_offset(std::chrono::sys_days(2026y / 7 / 15));
    for (const auto offset : {winter, summer}) {
        CHECK(offset <= 14h && offset >= -14h);
        CHECK(offset % 15min == 0s);
    }
    CHECK(summer - winter <= 2h && winter - summer <= 2h);
}

} // namespace

void local_time_tests()
{
    formatting_tests();
    local_date_tests();
    system_local_date_tests();
    system_offset_tests();
}
