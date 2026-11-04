#pragma once
// What the window says around its pages (decision 0046): the folder of the
// open repository under the sidebar's title, and how a message it tells in an
// alert is laid out there.
//
// Plain C++ without GTK, so the unit tests can check it.
#include <cstddef>
#include <filesystem>
#include <string>

namespace va::gui {

// The last folder name of RepositoryRoot `root`, also where `root` ends with
// a separator; empty for the root of a volume, which has none.
std::string repository_folder(const std::filesystem::path& root);

// The most columns (app::display_width: two for a Chinese character) of an
// alert's heading.
inline constexpr std::size_t alert_heading_columns = 60;

// A message as an alert tells it (tell). An alert's heading is a few words in
// large bold type: a short first line (a result, such as "Not abandoned:
// ...") is the heading and the lines after it go under it; a message whose
// first line is long (a refusal of one long sentence) goes under no heading,
// whole. Blank lines and spaces at its end, and blank lines between the
// heading and the rest, are left out.
struct AlertWords {
    std::string heading;   // empty: none
    std::string body;      // empty: none
};

AlertWords alert_words(const std::string& message);

} // namespace va::gui
