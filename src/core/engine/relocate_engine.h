#pragma once
// Relocation (decision 0004): the operator copied an archive to a new carrier
// with their own tools, or put its disk in a new enclosure (MIGRATED).
// VeriArchive copies nothing; it FULL-verifies the new carrier against the latest
// revision and, if every entry matches, writes the next revision with the same
// content hashes and the new carrier's identity, so the archive keeps its uuid
// and history and stays verifiable.
//
// A relocation resumes like a FULL verification (R-104, decision 0026). While
// it has not finished it keeps a record of its own in the repository settings
// (relocation_key, one per archive: never a verification's "run:<run_id>"),
// and checkpoints the entries whose content matched into verify_progress
// under its run_id. `archive relocate --resume` continues it without reading
// those entries again, while the archive is still at the revision it verifies
// and the new root is on the volume and in the folder it started on, and never
// while the process that runs its attempt still runs (decision 0024), nor once
// an attempt found that the new carrier does not hold the recorded content
// (its record then says "differs", and the relocation waits to be abandoned).
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

struct RelocateRequest {
    Uuid archive_uuid;
    std::filesystem::path root;             // the archive root on the new carrier
    std::optional<bool> rotational;         // --io hdd|ssd (R-111)
    bool resume = false;                    // continue the archive's unfinished relocation (decision 0026)
    std::map<std::string, bool> rotational_override = {}; // io.rotational_override of the config, by serial (R-111)
};

struct RelocateServices {
    IFileSystemAccess& archive_files;
    IFileSystemAccess& repository_files;
    IFileWriter& writer;
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    std::string repository_uuid;
    HostInfo host;
    IProcessTable& processes;               // which process runs an attempt, and whether it still runs
};

struct RelocateOutcome {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;             // the new revision
    std::string result;                     // PASSED or PASSED_WITH_WARNINGS
    std::string run_id;                     // the FULL verification that accepted the carrier
    // Run-level warnings of that verification (§A6), as a FULL verify gives
    // them: the portability warnings that the revision records, and each file
    // whose whole-file hash or chunk hashes cannot be relied on, though the
    // file matched, carried with rN's hashes (decision 0032).
    std::vector<std::string> warnings;
    std::map<std::string, std::uint64_t> counts;
    std::vector<VerifyItemRecord> items;
    std::filesystem::path manifest;
    bool resumed = false;                   // it continued an unfinished relocation (decision 0026)
};

struct RelocateFailure {
    enum class Kind { Refused, Differs, Media, Repository, Incomplete };
    Kind kind;
    std::string message;
    std::vector<VerifyItemRecord> items;    // for Differs: what does not match
};

std::expected<RelocateOutcome, RelocateFailure> relocate_archive(
    RelocateServices& services,
    const RelocateRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

// The part a recorded run plays in a relocation (decision 0026), as the
// relocation wrote it into the run's counts: an interrupted attempt is an
// INCOMPLETE FULL verification of revision N whose counts name the
// relocation's run id ("relocation"); the FULL verification that recorded
// revision N+1 keeps the relocation's run id, and its counts name N
// ("relocated_from_revision", written since relocation exists).
struct RelocationPart {
    std::string relocation;                 // the relocation's run id
    std::uint32_t from_revision = 0;        // N, the revision it verified the new carrier against
    bool interrupted_attempt = false;       // an interrupted attempt; otherwise the run that recorded N+1
};

// Nothing for a run that is no relocation's, such as every verification.
std::optional<RelocationPart> relocation_part(const VerifyRunRecord& run);

} // namespace va
