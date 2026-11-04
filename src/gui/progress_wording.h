#pragma once
// How the window words an operation's progress for a bar (BarState): files
// read and hashed (a registration, a verification, a relocation), manifest
// entries compared with no bytes read (a comparison), or the bytes of a
// manifest copied (an export, R-94).
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/progress_estimate.h"
#include "app/text.h"
#include "core/interfaces.h"
#include "gui/job_shown.h"

#include <chrono>
#include <optional>

namespace va::gui {

// What the items of a progress update are: files read and hashed (register,
// verify, relocate), manifest entries compared (compare), or the bytes copied
// of a file whose size is known (export), whose updates count no items.
enum class Counting { Files, Entries, Bytes };

// Words an operation's progress for a bar (BarState), at most five times a
// second, and always for the report that completes the count, so a finished
// operation's bar ends full rather than where the update before left it.
// Every report feeds the estimate, also those not shown. `now` is when the
// report came: the clock of the estimate and of the five times a second.
class ProgressWording {
public:
    ProgressWording(const app::Text& text, Counting counting) : text_(text), counting_(counting) {}
    std::optional<BarState> update(const Progress& progress,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    app::Text text_;
    Counting counting_;
    // The engines report regularly (hashing five times a second), so a long
    // silence is a pause, such as the computer sleeping.
    app::ProgressEstimate estimate_{app::ProgressEstimate::Reports::regular};
    std::chrono::steady_clock::time_point last_{};
};

} // namespace va::gui
