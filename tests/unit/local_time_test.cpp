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
    system_offset_tests();
}
