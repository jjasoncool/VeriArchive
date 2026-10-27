#pragma once
// FULL verification (HANDOFF §B11, M3): identify the media, validate the
// manifest against the repository record, re-read and re-hash every entry, and
// record a hash-chained result. Only "FULL VERIFY PASSED" means every registered
// byte was read and matched (§A6).
#include "core/engine/candidates.h"
#include "core/identity/matcher.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

struct VerifyRequest {
    std::optional<std::filesystem::path> root;  // verify the archive at this path, or
    std::optional<Uuid> archive_uuid;           // find this archive on the attached volumes
    bool strict_metadata = false;               // R-51: metadata differences fail the run
    std::optional<bool> rotational;             // --io hdd|ssd
    std::map<std::string, bool> rotational_override = {}; // io.rotational_override of the config, by serial (R-111)
    std::optional<std::string> resume_run;      // R-104: continue an INCOMPLETE run
};

struct VerifyServices {
    IFileSystemAccess& archive_files;
    IFileSystemAccess& repository_files;
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    std::string repository_uuid;
    HostInfo host;
};

// A finished (or interrupted) run, already recorded in the history chain.
struct VerifyOutcome {
    std::string run_id;
    std::string resumed_from;                   // R-104: the INCOMPLETE run this one continues
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    std::filesystem::path root;
    std::string result;                         // PASSED | PASSED_WITH_WARNINGS | FAILED | INCOMPLETE
    MatchOutcome identity;
    std::vector<std::string> failure_classes;   // §A6 run-level failure classes
    std::vector<std::string> warnings;
    std::map<std::string, std::uint64_t> counts; // item status -> number of entries
    std::vector<VerifyItemRecord> items;        // every non-Verified item (D7)
    std::uint64_t coverage_bytes = 0;           // bytes read and hashed in this run
    std::uint64_t registered_bytes = 0;
    // False when the run FAILED before the media was looked for, as the
    // manifest of the archive's revision cannot be used (decision 0028):
    // nothing was read, no media evidence recorded, and `root` and
    // `identity` say nothing.
    bool media_read = true;
};

// Verification could not start: the media needs an operator decision
// (MIGRATED, AMBIGUOUS, UNKNOWN, previous carrier) or something is missing.
// An archive named by its uuid, or by the run of it to resume, is known
// before the media is looked for: when the manifest of its current revision
// cannot be used, the verification does not stop but FAILS, and is recorded
// (decision 0028).
struct VerifyStop {
    enum class Kind { Decision, NotFound, Repository };
    Kind kind;
    std::string message;
    MatchOutcome identity;
    // An archive named by its uuid that more than one attached volume holds
    // (decision 0022, point 7): the archive's root on each, for the user
    // interfaces to word what to do. Empty for every other stop.
    std::vector<std::filesystem::path> roots_found = {};
};

std::expected<VerifyOutcome, VerifyStop> verify_full(
    VerifyServices& services,
    const VerifyRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
