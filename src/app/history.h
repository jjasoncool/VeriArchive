#pragma once
// The verification history as people read it (`history <uuid>`, the GUI's
// History and a run's result page). Every run is a verification of its
// archive; some are also a relocation's (decision 0026), and some went on past
// a changed enclosure with a carrier confirmation (decision 0047): the history
// says so beside them.
#include "app/text.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace va::app {

// What a check of the history's hash chain (R-18, R-90: `repo
// verify-history`, Check history chain) tells, shown after a chain that
// holds. The chain links each run to the one before it, so a changed run
// breaks it, and so does a removed run that later runs follow; the newest runs
// removed, or runs changed by someone who computes the chain again, leave a
// chain that holds. Signed history checkpoints (R-76), history signatures in
// the interface, would tell those; the text names no version of the program
// for them (decision 0017).
inline constexpr std::string_view history_chain_limits =
    "Checking the chain detects a recorded run that was changed or removed, but not the removal of the newest "
    "runs, nor runs changed with the chain computed again; history signatures, which would detect those, are "
    "not part of this version.";

// What else a run is, in the words shown beside it: "interrupted relocation
// attempt" for an attempt of a relocation that was interrupted (recorded
// INCOMPLETE against the revision it verifies, it never verified the
// archive's carrier), and "relocation from rN" for the FULL verification of
// the new carrier that recorded the relocation's revision. A run that went on
// past a changed enclosure (decision 0047), whatever its result, says how the
// enclosure was confirmed: "enclosure changed, same device by its serial"
// (the device's own serial was the archive's confirmed device), "enclosure
// changed, confirmed by the operator after reading the device's serial", or
// "enclosure changed, confirmed by the operator without the serial". Empty
// for any other run.
std::string history_note(const Text& text, const VerifyRunRecord& run);

// One run as `history --json` gives it: run_id, revision, mode, started,
// finished (UTC), result, failure_classes, host, coverage_bytes (null where
// not recorded); `relocation`: the relocation the run belongs to (decision
// 0026: run_id, from_revision, interrupted_attempt), or null; `device_alerts`
// (decision 0044); `carrier_confirmed`: the confirmation it went on with past
// a changed enclosure (decision 0047: how, device where read, the enclosures,
// from_revision), or null. Never translated.
nlohmann::json history_run_json(const VerifyRunRecord& run);

} // namespace va::app
