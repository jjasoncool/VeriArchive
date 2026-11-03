#pragma once
// Device alerts in the words of the interfaces (decision 0044): what a device
// reports of its own health. The repository and --json keep the codes.
#include "app/text.h"
#include "common/types.h"

#include <string>
#include <string_view>

namespace va::app {

// What leads every line of a device alert, in a report, a notification and
// `media list`, so that it stands out from the lines of the result.
inline constexpr std::string_view alert_mark = "⚠ ";

// One condition that a device reports, in plain English ("predicting-failure":
// "predicting failure"); a code this does not know as it is.
std::string health_condition_words(std::string_view condition);

// A device's health in the language of `text`: "healthy", "cannot be read"
// (Unknown), "warning", "unhealthy", followed by its conditions in brackets:
// "warning (predicting failure, bad sectors)".
std::string health_words(const Text& text, const DeviceHealth& health);

} // namespace va::app
