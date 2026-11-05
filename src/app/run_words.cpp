#include "app/run_words.h"

#include "common/time.h"

#include <algorithm>
#include <format>

namespace va::app {

std::string format_bytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

std::string bus_words(const std::string& bus)
{
    if (bus == "usb") {
        return "USB";
    }
    if (bus == "sata") {
        return "SATA";
    }
    if (bus == "nvme") {
        return "NVMe";
    }
    if (bus == "scsi") {
        return "SCSI";
    }
    return bus;
}

std::string grouped(std::uint64_t count)
{
    auto digits = std::to_string(count);
    for (auto at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

std::string duration_words(const Text& text, std::string_view started, std::string_view finished)
{
    const auto start = parse_timestamp(started);
    const auto end = parse_timestamp(finished);
    if (!start || !end) {
        return {};
    }
    const auto seconds = std::max<long long>(0, (*end - *start).count());
    if (seconds < 60) {
        return text.format("{} s", seconds);
    }
    if (seconds < 3600) {
        return text.format("{} min {} s", seconds / 60, seconds % 60);
    }
    return text.format("{} h {} min", seconds / 3600, (seconds % 3600) / 60);
}

std::string result_with_mode(const Text& text, const VerifyRunRecord& run)
{
    if (run.mode == "FULL") {
        return text.format("FULL VERIFY {}", text(run.result));
    }
    return text.format("{} {}", text(run.mode), text(run.result));
}

Tone result_tone(std::string_view result)
{
    if (result == "PASSED") {
        return Tone::Success;
    }
    if (result == "PASSED_WITH_WARNINGS") {
        return Tone::Warning;
    }
    if (result == "INCOMPLETE") {
        return Tone::Neutral;
    }
    return Tone::Error;
}

} // namespace va::app
