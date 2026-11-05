#pragma once
// The result page of one recorded run (decision 0046) as the window shows it.
// What the page says is the app's (app/run_result.h), which the verification
// report of the run says too (decision 0048): the page words it with the
// media identity and the computer that the views show (displayed_identity),
// the sample ones in the screenshot mode. The window's own part is Resume of
// an INCOMPLETE run that it can continue.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/run_result.h"
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "core/repository/archive_store.h"
#include "gui/archive_rows.h"
#include "gui/displayed_identity.h"

#include <chrono>
#include <optional>
#include <vector>

namespace va::gui {

using app::RecordedRun;
using app::ResultItem;
using app::ResultPage;
using app::SpeedChart;
using app::axis_step;
using app::axis_words;
using app::changes_no_result;
using app::chart_least_milliseconds;
using app::chart_stretches;
using app::fails_the_run;
using app::grouped;
using app::item_tone;
using app::run_read_speed;
using app::speed_chart;
using app::stopped_percent;

// The result page of `recorded` as the window shows it: its devices, volume
// and computer as `shown` says (shown_identity).
ResultPage result_page(const app::Text& text, const RecordedRun& recorded, IdentityShown shown);

// The stopped verification that the run `run` left and that Resume
// continues, as the details' Resume finds it (details_operations): the
// latest read of the unfinished operations (`watches`) lists it with its run
// and offers Resume on it; nothing for any other run.
std::optional<app::OperationRow> stopped_run(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const VerifyRunRecord& run, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

} // namespace va::gui
