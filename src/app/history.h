#pragma once
// The verification history as people read it (`history <uuid>`, the GUI's
// Verification History). Every run is a verification of its archive; some are
// also a relocation's (decision 0026), and the history says so beside them.
#include "app/text.h"
#include "core/repository/archive_store.h"

#include <string>

namespace va::app {

// What else a run is, in the words shown beside it: "interrupted relocation
// attempt" for an attempt of a relocation that was interrupted (recorded
// INCOMPLETE against the revision it verifies, it never verified the
// archive's carrier), and "relocation from rN" for the FULL verification of
// the new carrier that recorded the relocation's revision. Empty for any other
// run.
std::string history_note(const Text& text, const VerifyRunRecord& run);

} // namespace va::app
