#include "app/history.h"

#include "app/local_time.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/device_alerts.h"
#include "core/engine/relocate_engine.h"

#include <algorithm>

namespace va::app {

ChainShown chain_words(const Text& text, const std::expected<HistoryCheck, IoError>& check,
    const std::vector<VerifyRunRecord>& runs, const std::map<Uuid, std::string>& labels)
{
    ChainShown shown;
    if (!check) {
        shown.tone = Tone::Error;
        shown.result = text.format("Cannot check the history: {}", text(check.error().message));
        return shown;
    }
    shown.limits = text(history_chain_limits);
    if (check->first_broken_run) {
        shown.tone = Tone::Error;
        const auto& broken = *check->first_broken_run;
        const auto run = std::ranges::find(runs, broken, &VerifyRunRecord::run_id);
        if (run == runs.end()) {
            shown.result = text.format("History BROKEN at run {}: {}", broken, text(check->problem));
            return shown;
        }
        // Its archive and when it ended, as its row in History names them.
        const auto label = labels.find(run->archive_uuid);
        shown.result = text.format("History BROKEN at run {} ({} · {}): {}", broken,
            label != labels.end() ? label->second : run->archive_uuid.substr(0, 8),
            local_time(run->finished, text), text(check->problem));
        return shown;
    }
    shown.tone = Tone::Success;
    // Two calls: the compiler checks each literal template against its value.
    shown.result =
        check->runs == 1
            ? text.format("The history of every archive is intact: {} verification run, hash chain verified.", check->runs)
            : text.format("The history of every archive is intact: {} verification runs, hash chain verified.",
                  check->runs);
    return shown;
}

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
