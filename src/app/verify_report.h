#pragma once
// What the report of a FULL verification says besides its result and items,
// on the command line (`verify --full`) and in the GUI's Verify Archive, each
// in its own words.
#include "app/text.h"
#include "core/engine/verify_engine.h"

#include <optional>
#include <string>
#include <string_view>

namespace va::app {

enum class ReportedIn { CommandLine, Window };

// Decision 0010: a run whose content matched (PASSED_WITH_WARNINGS) and that
// has Unbaselined entries, which could not be read when the archive was
// registered, says how to take a baseline for them: register a new revision
// of the archive, which reads it all again (`register <root> --new-revision`,
// the root as command_line_argument writes it, or the new revision box of
// Register Archive), the root the run read named. Nothing for any other run:
// after a FAILED one, a new revision would take entries that differ from the
// baseline for good ones.
// Nothing for a retired archive either (its identity details "retired"),
// which takes no new revision (decision 0003).
std::optional<std::string> unbaselined_advice(const Text& text, const VerifyOutcome& outcome, ReportedIn where);

// `argument` (UTF-8) written so that a shell hands it to a program unchanged:
// as it is where it holds only letters, digits (any non-ASCII text too) and
// - _ . / \ :, as a volume root such as X:\ does; otherwise in double
// quotes, the backslashes before the closing quote doubled, as the Windows
// command line reads \" as a quote that belongs to the argument (X:\" would
// swallow what follows). A POSIX shell reads \\ in double quotes as one \ too.
std::string command_line_argument(std::string_view argument);

} // namespace va::app
