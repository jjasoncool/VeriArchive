#include "app/history.h"

#include "core/engine/carrier_confirmation.h"
#include "core/engine/device_alerts.h"
#include "core/engine/relocate_engine.h"

namespace va::app {

std::string history_note(const Text& text, const VerifyRunRecord& run)
{
    if (const auto part = relocation_part(run)) {
        if (part->interrupted_attempt) {
            return text("interrupted relocation attempt");
        }
        return text.format("relocation from r{}", part->from_revision);
    }
    // Decision 0047: how the enclosure was confirmed, as the media evidence
    // of the run notes it too.
    if (const auto confirmed = carrier_confirmed(run)) {
        if (confirmed->confirmation.how == CarrierConfirmation::How::DeviceSerial) {
            return text("enclosure changed, same device by its serial");
        }
        return confirmed->confirmation.device
                   ? text("enclosure changed, confirmed by the operator after reading the device's serial")
                   : text("enclosure changed, confirmed by the operator without the serial");
    }
    return {};
}

nlohmann::json history_run_json(const VerifyRunRecord& run)
{
    nlohmann::json entry{{"run_id", run.run_id}, {"revision", run.revision}, {"mode", run.mode},
        {"started", run.started}, {"finished", run.finished}, {"result", run.result},
        {"failure_classes", run.failure_classes}, {"host", run.host}};
    entry["coverage_bytes"] = run.coverage_bytes ? nlohmann::json(*run.coverage_bytes) : nullptr;
    // The relocation a run belongs to (decision 0026): an interrupted
    // attempt, or the run that recorded the relocation's revision.
    const auto part = relocation_part(run);
    entry["relocation"] = part ? nlohmann::json{{"run_id", part->relocation}, {"from_revision", part->from_revision},
                                     {"interrupted_attempt", part->interrupted_attempt}}
                               : nlohmann::json(nullptr);
    entry["device_alerts"] = run_alerts_json(run_alerts(run.counts_json));
    // Decision 0047: the confirmation it went on with past a changed enclosure.
    const auto confirmed = carrier_confirmed(run);
    entry["carrier_confirmed"] = confirmed
                                     ? carrier_confirmation_json(confirmed->confirmation, confirmed->from_revision)
                                     : nlohmann::json(nullptr);
    return entry;
}

} // namespace va::app
