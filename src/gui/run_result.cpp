#include "gui/run_result.h"

namespace va::gui {

ResultPage result_page(const app::Text& text, const RecordedRun& recorded, IdentityShown shown)
{
    return app::result_page(text, recorded, shown_identity(shown));
}

std::optional<app::OperationRow> stopped_run(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const VerifyRunRecord& run, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now)
{
    if (run.result != "INCOMPLETE") {
        return std::nullopt;
    }
    for (const auto& watch : watches) {
        const auto& operation = watch.operation();
        if (operation.kind != app::OperationKind::Verification || operation.archive_uuid != run.archive_uuid ||
            operation.run_id != run.run_id) {
            continue;
        }
        // As its row under Unfinished operations offers Resume.
        auto row = app::describe_operation(text, watch, now, wall_now);
        if (row.offers_resume) {
            return row;
        }
    }
    return std::nullopt;
}

} // namespace va::gui
