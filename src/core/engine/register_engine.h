#pragma once
// Registration: scan an archive root, hash every file, write the first (or a
// new) revision's manifest, and index it in the repository.
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <optional>
#include <string>
#include <vector>

namespace va {

struct RegisterRequest {
    std::filesystem::path root;
    std::optional<std::string> label;       // default: next repository label
    bool new_revision = false;              // R-97: re-baseline an existing archive
    bool force_new_archive = false;         // R-97: register again as a separate archive
    std::optional<Uuid> copy_of;            // decision 0005
    std::optional<bool> rotational;         // --io hdd|ssd overrides detection (R-111)
    bool resume = false;                    // R-104: continue an interrupted registration
    bool write_media_copy = false;          // R-89: also write .veriarchive/ on the media (off by default)
    // A registration over a lost manifest (decision 0028, point 8): the next
    // revision of this archive, whose latest manifest is missing or cannot be
    // used and which nothing puts back, from what `root` holds now. Matching
    // cannot find the archive at a root (only that manifest records its
    // carrier), so the operator names it. Without `confirmed` it only checks
    // and says what it would register (Kind::Unconfirmed).
    std::optional<Uuid> lost_manifest_of;
    bool confirmed = false;                 // --confirm
};

struct RegisterServices {
    IFileSystemAccess& archive_files;       // reads the archive media
    IFileSystemAccess& repository_files;    // reads back repository files
    IFileWriter& writer;                    // writes repository files
    IFileWriter& media_writer;              // writes the archive media, only with write_media_copy (R-03)
    IMediaEnumerator& media;
    ArchiveStore& store;
    RepositoryLayout layout;
    HostInfo host;
    IProcessTable& processes;               // which process runs an attempt, and whether it still runs (decision 0024)
};

struct RegisterOutcome {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    ManifestCounts counts;
    std::vector<Entry> unreadable;          // decision 0010: listed, exit code 2
    std::vector<ArchivePath> skipped_mount_points;
    std::vector<std::string> warnings;
    std::filesystem::path manifest;
    std::filesystem::path media_copy;       // set when the media copy was written (R-89)
    // A registration over a lost manifest (decision 0028, point 8): the
    // revision whose manifest was lost, and whether its record left the
    // index with this record, as its manifest still could not be used.
    std::optional<std::uint32_t> lost_revision;
    bool lost_revision_left_out = false;
};

// An archive whose root the root of a registration over a lost manifest
// overlaps, on a volume that the root's volume matches only as MIGRATED (the
// filesystem id that archive's latest manifest records, on another device)
// or MATCH_WEAK (no device, or no filesystem id, to compare): the root's
// volume may be a sector-by-sector clone of that archive's volume, or that
// volume itself (decision 0028, point 8). Named before the operator confirms.
struct PossibleClone {
    Uuid archive_uuid;
    std::string label;
    MatchResult match = MatchResult::MatchWeak;  // MIGRATED or MATCH_WEAK
};

// What a registration over a lost manifest registers (decision 0028, point
// 8), as it says before the operator confirms it.
struct LostManifestRegistration {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t lost_revision = 0;        // the archive's revision, whose manifest is lost
    std::uint32_t revision = 0;             // the revision it registers: the next one
    std::string root;                       // UTF-8, as the root is given
    UnusableManifest manifest;              // what is wrong with the lost revision's manifest
    std::vector<PossibleClone> possible_clones;  // in the order of the archives' labels
};

struct RegisterFailure {
    enum class Kind {
        Invalid,     // a value of the request is malformed, e.g. the label (exit 10)
        Refused,     // needs an operator decision or flag (exit 12)
        Media,       // the root or its volume cannot be read (exit 11)
        Repository,  // manifest or database could not be written (exit 11)
        Incomplete,  // cancelled or the device disappeared (exit 4)
        Unconfirmed, // a registration over a lost manifest not confirmed: nothing written (exit 12)
    };
    Kind kind;
    std::string message;
    // Unconfirmed: what the registration would register once confirmed.
    std::optional<LostManifestRegistration> lost_manifest = std::nullopt;
};

std::expected<RegisterOutcome, RegisterFailure> register_archive(
    RegisterServices& services,
    const RegisterRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
