#pragma once
// A message or report that can be worded in any interface language (decision
// 0017). A view keeps the one it shows, so a language change can show it
// again in the new language.
//
// Plain C++ without GTK, so the unit tests can use it.
#include "app/run_words.h"
#include "app/text.h"

#include <functional>
#include <string>

namespace va::gui {

using Report = std::function<std::string(const app::Text&)>;

// What VeriArchive detects and what this version does not (R-02): every
// verify report and the result page of every run carry it (decision 0017),
// and Settings says it; a size and a device's bus as the views show them
// ("1.2 GiB", "USB"). The app's, which the verification report and the
// command line say too (app/run_words.h).
using app::bus_words;
using app::format_bytes;
using app::guarantee;

// A fixed English message (or engine sentence) as a Report.
Report fixed_message(std::string english);

// "Unexpected error: ..." for an exception's message.
Report unexpected_error(std::string what);

} // namespace va::gui
