#include "gui/report.h"

#include <format>

namespace va::gui {

Report fixed_message(std::string english)
{
    return [english = std::move(english)](const app::Text& text) { return text(english); };
}

Report unexpected_error(std::string what)
{
    // The message goes through text() too: some exceptions carry a sentence the
    // catalog knows, such as "cannot initialise SHA-256".
    return [what = std::move(what)](const app::Text& text) {
        return text.format("Unexpected error: {}", text(what));
    };
}

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

} // namespace va::gui
