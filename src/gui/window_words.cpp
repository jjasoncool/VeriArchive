#include "gui/window_words.h"

#include "app/progress_estimate.h"
#include "common/utf8.h"

#include <algorithm>

namespace va::gui {

std::string repository_folder(const std::filesystem::path& root)
{
    auto name = root.filename();
    if (name.empty() && root.has_relative_path()) {
        // ".../repository/": its last element is the empty one after the separator.
        name = root.parent_path().filename();
    }
    return path_to_utf8(name);
}

AlertWords alert_words(const std::string& message)
{
    const auto last = message.find_last_not_of("\n ");
    const auto lines = last == std::string::npos ? std::string{} : message.substr(0, last + 1);
    const auto end = lines.find('\n');
    AlertWords words;
    words.heading = lines.substr(0, end);
    if (end != std::string::npos) {
        words.body = lines.substr(end + 1);
        words.body.erase(0, std::min(words.body.find_first_not_of('\n'), words.body.size()));
    }
    if (app::display_width(words.heading) > alert_heading_columns) {
        words.body = lines;
        words.heading.clear();
    }
    return words;
}

} // namespace va::gui
