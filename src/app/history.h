#pragma once
// The verification history as people read it (`history <uuid>`, the GUI's
// History and a run's result page). Every run is a verification of its
// archive; some are also a relocation's (decision 0026), and the history says
// so beside them.
#include "app/text.h"
#include "core/repository/archive_store.h"

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
// the new carrier that recorded the relocation's revision. Empty for any other
// run.
std::string history_note(const Text& text, const VerifyRunRecord& run);

} // namespace va::app
