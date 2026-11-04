#pragma once
// Device alerts in the words of the interfaces (decision 0044): what a device
// reports of its own health, and a FULL verification that read markedly
// slower than the last one of the archive on that device. The repository and
// --json keep the codes and numbers.
#include "app/text.h"
#include "common/types.h"
#include "core/engine/archive_status.h"
#include "core/engine/device_alerts.h"
#include "core/engine/read_speed.h"

#include <cstdint>
#include <optional>
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

// A speed as progress shows one: "182 MiB/s".
std::string speed_words(const Text& text, std::uint64_t bytes_per_second);

// How fast a FULL verification read the content, said after "Read speed: ":
// "182 MiB/s" (over the hundredths it read in a measurable time,
// ReadSpeed::bytes_per_second), and where it was compared, "182 MiB/s; the
// last FULL verification of this archive on this device, which finished
// 2026-11-01 14:03:00 (UTC+8), read 180 MiB/s". Nothing where it read
// nothing in a measurable time.
std::optional<std::string> read_speed_words(
    const Text& text, const std::optional<ReadSpeed>& speed, const std::optional<SpeedComparison>& compared);

// The device alert of a verification that read markedly slower than the one
// it was compared with, led by alert_mark: how much slower, where in the
// content, and what else makes reading slower; nothing where it did not.
std::optional<std::string> slower_alert_line(const Text& text, const SpeedComparison& compared);

// The short marks of a run's device alerts, led by alert_mark: "⚠ device
// reports a problem", "⚠ reading slower"; none for a run without any. Beside
// each run of the history.
std::vector<std::string> alert_marks(const Text& text, const RunAlerts& alerts);

// The device alerts that Archives marks an archive with, beside its
// last verification, until a later one gives none: the health that the
// devices reported in its latest run (an INCOMPLETE one included: it asked
// them as it started), and the read of its latest FULL verification that
// reached a result.
RunAlerts archive_alerts(const ArchiveStatus& status);

} // namespace va::app
