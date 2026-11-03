#include "gui/ended_notification.h"

#include "app/device_alerts.h"

#include <cstddef>

namespace va::gui {

std::string ended_notification_body(std::string_view report)
{
    std::string body;
    std::size_t lines = 0;
    while (!report.empty()) {
        const auto end = report.find('\n');
        const auto line = report.substr(0, end);
        report = end == std::string_view::npos ? std::string_view{} : report.substr(end + 1);
        if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
            continue;
        }
        // The first two lines, then each device alert (decision 0044).
        if (lines >= 2 && !line.starts_with(app::alert_mark)) {
            continue;
        }
        if (lines > 0) {
            body += '\n';
        }
        body += line;
        ++lines;
    }
    return body;
}

} // namespace va::gui
