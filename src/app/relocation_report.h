#pragma once
// What `archive relocate` gives of a relocation (decisions 0004, 0026, 0032),
// in text, worded in `text`'s language, and in --json, always English
// (decision 0017). A relocation compares by chunk as a FULL verify does
// (decision 0032), so its FULL verification of the new carrier gives the
// same warnings, and the entries that differ the same details, such as the
// chunks of a file that differ.
#include "app/text.h"
#include "core/engine/relocate_engine.h"

#include <nlohmann/json.hpp>

#include <string>

namespace va::app {

// The text of a relocation that recorded its revision, to the new root
// `root`: "Relocated L (U) to R as revision N", then, indented, the FULL
// verification of the new carrier, each of its warnings ("warning: ...") and
// each item with its detail in plain words ("MetadataMismatch  photos/a.jpg
// (modification time)"); one line each, each ending with a newline.
std::string relocation_report(const Text& text, const RelocateOutcome& outcome, const std::string& root);

// archive relocate --json of a relocation that recorded its revision:
// `archive_uuid`, `revision`, `result`, `run_id`, `manifest`, `counts`,
// `warnings` and `resumed`.
nlohmann::json relocation_json(const RelocateOutcome& outcome);

// The text of a relocation refused because the new carrier does not hold the
// recorded content (RelocateFailure::Kind::Differs): "RELOCATION REFUSED:
// <why>", then, indented, each entry that is not Verified, with its detail
// ("Modified  big.bin  (content differs: chunk 1 of 3 differs, from byte
// 67,108,864)"); one line each, each ending with a newline.
std::string relocation_refusal(const Text& text, const RelocateFailure& failure);

// What the window's job of a relocation says of it (decision 0046): as
// relocation_refusal, but where the relocation's saved progress could not be
// removed (RelocateFailure::refusing, `cause`), that Abandon... under
// Unfinished operations removes it, in place of `archive abandon-relocation`.
std::string relocation_refusal_in_window(const Text& text, const RelocateFailure& failure);

} // namespace va::app
