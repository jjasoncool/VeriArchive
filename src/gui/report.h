#pragma once
// A message or report that can be worded in any interface language (decision
// 0017). A view keeps the one it shows, so a language change can show it
// again in the new language.
//
// Plain C++ without GTK, so the unit tests can use it.
#include "app/text.h"

#include <cstdint>
#include <functional>
#include <string>

namespace va::gui {

using Report = std::function<std::string(const app::Text&)>;

// What VeriArchive detects and what this version does not (R-02): every
// verify report and the result page of every run carry it (decision 0017),
// and Settings says it.
inline constexpr const char* guarantee =
    "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) "
    "is not part of this version.";

// A fixed English message (or engine sentence) as a Report.
Report fixed_message(std::string english);

// "Unexpected error: ..." for an exception's message.
Report unexpected_error(std::string what);

// A size as the views show it: "173.0 B", "1.2 GiB", "9.4 TiB".
std::string format_bytes(std::uint64_t bytes);

} // namespace va::gui
