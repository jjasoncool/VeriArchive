#pragma once
// Device alerts in the words of the interfaces (decision 0044): what a device
// reports of its own health. The repository and --json keep the codes.
#include "app/text.h"
#include "common/types.h"

#include <string>
#include <string_view>
#include <vector>

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

// A device as people tell it apart: its model and its serial, or its WWN
// where it has no serial: "WDC WD80EFZZ (serial WD-SAMPLE)".
std::string device_name(const Text& text, const PhysicalDeviceInfo& device);

// "WDC WD80EFZZ (serial WD-SAMPLE): healthy", for a device that was asked.
std::string device_health_words(const Text& text, const PhysicalDeviceInfo& device);

// The device alert of each device whose health alerts (health_alerts), one
// line each, led by alert_mark: what it reports, and to copy its archives to
// another device and verify the copies. None where no device alerts.
std::vector<std::string> health_alert_lines(const Text& text, const std::vector<PhysicalDeviceInfo>& devices);

} // namespace va::app
