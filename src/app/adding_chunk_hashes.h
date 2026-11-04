#pragma once
// Adding chunk hashes (decision 0033) as the GUI and the command line show
// it: what verify, archive list|show and archive abandon-verification give
// of such verifications, in text and in --json (always English, decision
// 0017).
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "common/error.h"
#include "common/types.h"
#include "core/engine/abandon_verification.h"
#include "core/engine/verify_engine.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace va::app {

// What a run that adds chunk hashes says of them once it has finished
// (decision 0033, point 10), in `text`'s language: "chunk hashes added:
// revision N records them (chunk size 64 MiB, C chunks, Merkle root R)", or
// "chunk hashes not added: <why>", as verify's text output gives it after the
// items; with `sentence`, as the GUI's report gives it ("Chunk hashes added:
// ..."). Empty for a run that adds none, or that has not finished.
std::string chunk_hashes_line(const Text& text, const VerifyOutcome& outcome, bool sentence = false);

// verify --json's "chunk_hashes" of a run that adds chunk hashes (decision
// 0033, point 10): `chunk_size`; `added_revision`, `chunks` and
// `merkle_root`, null while no revision records them; and `not_added`, null
// unless a finished run records none. As an object of that one member, which
// the run's object takes over; an empty object for a run that adds none.
nlohmann::json chunk_hashes_member(const VerifyOutcome& outcome);

// archive list --json's and archive show --json's "unfinished_verification"
// of an archive whose verification that adds chunk hashes is unfinished
// (decision 0033, point 9): `root`, `revision` (the one it verifies),
// `run_id`, `chunk_size`, `started`, `updated` (null when its record has
// none), `state` (`operation`, from verification_operation, judged from one
// read as Archives judges its first, state_of_one_read),
// `resumable`, `files_done` and `bytes_done` (null where the checkpoints are
// not counted), `files_total`, `bytes_total`, and `process` (`id`,
// `program`, `host`) when its record names one. As an object of that one
// member, which the archive's object takes over.
nlohmann::json unfinished_verification_member(const UnfinishedVerification& verification,
    const UnfinishedOperation& operation, std::chrono::sys_seconds wall_now);

// archive abandon-verification --json's verification that adds chunk hashes,
// abandoned or to be (decision 0033, point 8): `archive_uuid`, `label`,
// `revision` (the one it verifies), `root`, `run_id`, `started`, and its
// `saved_progress` (`files`, `bytes`).
nlohmann::json abandonable_verification_json(const AbandonableVerification& verification);

// Without --confirm: what abandoning would remove, as the member
// "verification" of an object, beside which the command line puts the error
// that asks for --confirm and its exit code.
nlohmann::json abandonable_verification_member(const AbandonableVerification& verification);

} // namespace va::app
