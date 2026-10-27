#pragma once
// Times for people (decision 0017). Timestamps are stored and exported in UTC
// ("2026-10-14T00:13:53Z", common/time.h). The interface shows them in local
// time together with the offset from UTC, "2026-10-14 08:13:53 (UTC+8)", so the
// reader sees their own clock and the exact moment is still unambiguous.
// --json output keeps the UTC timestamps.
#include "app/text.h"

#include <chrono>
#include <string>
#include <string_view>

namespace va::app {

// `utc_timestamp` moved by `offset` and written with that offset, in the
// brackets of the text's language. Anything that is not a timestamp (such as
// the empty `finished` of a run that never ended) is returned unchanged.
std::string format_local_time(std::string_view utc_timestamp, std::chrono::seconds offset, const Text& text);

// The same in the system's time zone, with the offset in force at that moment
// (daylight saving time included).
std::string local_time(std::string_view utc_timestamp, const Text& text);

// The date that the clock shows at `moment` where it is `offset` ahead of
// UTC, and in the system's time zone: what names a file made for people, as
// the copy of the database kept before an upgrade (decision 0032).
std::chrono::year_month_day local_date(std::chrono::sys_seconds moment, std::chrono::seconds offset);
std::chrono::year_month_day local_date(std::chrono::sys_seconds moment);

} // namespace va::app
