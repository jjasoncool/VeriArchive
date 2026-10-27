#pragma once
// What the report of a FULL verification says besides its result and items,
// on the command line (`verify --full`) and in the GUI's Verify Archive, each
// in its own words.
#include "app/text.h"
#include "core/engine/verify_engine.h"

#include <optional>
#include <string>

namespace va::app {

enum class ReportedIn { CommandLine, Window };

// Decision 0010: a run whose content matched (PASSED_WITH_WARNINGS) and that
// has Unbaselined entries, which could not be read when the archive was
// registered, says how to take a baseline for them: register a new revision
// of the archive, which reads it all again (`register <root> --new-revision`,
// the root as shell_argument writes it, or the new revision box of
// Register Archive), the root the run read named. Nothing for any other run:
// after a FAILED one, a new revision would take entries that differ from the
// baseline for good ones.
// Nothing for a retired archive either (its identity details "retired"),
// which takes no new revision (decision 0003).
std::optional<std::string> unbaselined_advice(const Text& text, const VerifyOutcome& outcome, ReportedIn where);

} // namespace va::app
