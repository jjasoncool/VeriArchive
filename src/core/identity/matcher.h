#pragma once
// Identity matching: "which registered archive is this media?" (HANDOFF §B10,
// amended by decisions 0001, 0003, 0004, 0018 and 0021). Pure logic, no I/O.
#include "common/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace va {

enum class MatchResult {
    Match,           // filesystem and device both match
    MatchWeak,       // filesystem matches, no device serial on one side: result capped at warnings
    Migrated,        // filesystem matches, device differs (new enclosure, clone): operator decides
    Mismatch,        // device matches but filesystem differs (reformatted), or a copied identity file
    Unknown,         // not a registered archive
    Ambiguous,       // several archives match (cloned media): operator selects
    PreviousCarrier, // matches only an earlier revision's carrier (decision 0004)
};

// "MATCH", "MATCH_WEAK", ... as stored in media_evidence.match_result.
std::string_view to_string(MatchResult result);

// A carrier of an earlier revision, with the root it had there (decision 0004).
struct PreviousCarrier {
    VolumeInfo identity;
    std::string root_subpath;
};

// One registered archive as the matcher sees it.
struct ArchiveCandidate {
    Uuid archive_uuid;
    std::string root_subpath;               // ArchivePath form, "" = volume root
    bool retired = false;                   // decision 0003: still a candidate
    VolumeInfo identity;                    // carrier of the latest revision
    std::vector<PreviousCarrier> previous_carriers; // carriers of earlier revisions
};

struct MatchOutcome {
    MatchResult result = MatchResult::Unknown;
    std::vector<Uuid> archives;             // selected archive(s); several only for Ambiguous
    std::vector<std::string> details;       // e.g. "volume-label-differs", "device-replaced"
};

// `root_subpath` is the verify target relative to the volume root. Candidates
// are first narrowed to the same root_subpath, compared case-insensitively
// (decision 0001), then the §B10 table is applied.
MatchOutcome match_identity(
    const MediaEvidence& evidence,
    std::string_view root_subpath,
    std::span<const ArchiveCandidate> candidates);

// Exposed for tests and for the platform layer: serial numbers compared after
// removing spaces/underscores and ignoring case; a byte-pair-swapped ATA serial
// (as some Windows drivers report) also counts as equal.
bool serials_equal(std::string_view left, std::string_view right);

} // namespace va
