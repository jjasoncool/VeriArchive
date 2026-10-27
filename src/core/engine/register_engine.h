#pragma once
// Registration: scan an archive root, hash every file, write the first (or a
// new) revision's manifest, and index it in the repository.
#include "core/engine/candidates.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <filesystem>
#include <functional>
#include <map>
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
    std::map<std::string, bool> rotational_override = {}; // io.rotational_override of the config, by serial (R-111)
    bool resume = false;                    // R-104: continue an interrupted registration
    bool write_media_copy = false;          // R-89: also write .veriarchive/ on the media (off by default)
    // A registration over a lost manifest (decision 0028, point 8): the next
    // revision of this archive, whose latest manifest is missing or cannot be
    // used and which nothing puts back, from what `root` holds now. Matching
    // cannot find the archive at a root (only that manifest records its
    // carrier), so the operator names it. Without `confirmed` it only checks
    // and says what it would register (Kind::Unconfirmed).
    std::optional<Uuid> lost_manifest_of;
    // --confirm: of a registration over a lost manifest, as above; or of a
    // resume of a registration whose plan never says when its attempt last
    // worked and names no process (an older version's, decision 0022, point
    // 7), which that version may still run: without it, such a resume changes
    // nothing and says what it would take up (Kind::Unconfirmed,
    // OlderVersionRegistration). A resume of any other plan takes it and needs
    // none.
    bool confirmed = false;
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
    // The path by which the file system names a folder (platform::final_path),
    // to tell a root that holds the repository, or lies inside it, however
    // either is spelled (decision 0029, point 6); empty, or nothing for a
    // path, where only the paths' text is compared.
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)> final_path = {};
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
// contains or lies in, on a volume that the root's volume matches only as
// MIGRATED (the filesystem id that archive's latest manifest records, on
// another device) or MATCH_WEAK (no device, or no filesystem id, to compare):
// the root's volume may be a sector-by-sector clone of that archive's
// volume, or that volume itself (decision 0028, point 8). Named before the
// operator confirms.
struct PossibleClone {
    Uuid archive_uuid;
    std::string label;
    MatchResult match = MatchResult::MatchWeak;  // MIGRATED or MATCH_WEAK
};

// An archive whose very root the root of a registration over a lost manifest
// is, at that archive's own folder (its root subpath), on a volume that is
// plainly that archive's, a MATCH of it (the filesystem id and a device that
// its latest manifest records), or that matches it only as MIGRATED or
// MATCH_WEAK, which may be that archive's volume or a sector-by-sector clone
// of it. The same folder registered on purpose as two archives (R-97,
// --force-new-archive) gives each of these twins that volume and folder, so
// the archive named may be that archive's twin, whatever serial the volume's
// device shows now (decision 0028, point 8). Named before the operator
// confirms, with the match.
struct SameRoot {
    Uuid archive_uuid;
    std::string label;
    MatchResult match = MatchResult::Match;  // MATCH, MIGRATED or MATCH_WEAK
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
    std::vector<SameRoot> same_root;             // in the order of the archives' labels
};

// An unfinished registration whose plan never says when its attempt last
// worked and names no process: a version before decision 0022 wrote it, which
// may still run it, and which names its manifest before it records it
// (decision 0023, consequences). A resume says what it would take up before
// the operator confirms that no program of that version runs it (decision
// 0022, point 7).
struct OlderVersionRegistration {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;             // the revision it creates
    std::string root;                       // UTF-8, as its plan names it
    Timestamp started;                      // when its plan was made (its "created")
};

struct RegisterFailure {
    enum class Kind {
        Invalid,     // a value of the request is malformed, e.g. the label (exit 10)
        Refused,     // needs an operator decision or flag (exit 12)
        Media,       // the root or its volume cannot be read (exit 11)
        Repository,  // manifest or database could not be written (exit 11)
        Incomplete,  // cancelled or the device disappeared (exit 4)
        Unconfirmed, // a registration over a lost manifest, or a resume of an older version's registration,
                     // not confirmed: nothing written (exit 12)
    };
    Kind kind;
    std::string message;
    // Unconfirmed: what the registration would register once confirmed.
    std::optional<LostManifestRegistration> lost_manifest = std::nullopt;
    // Unconfirmed: the older version's registration that the resume would take up once confirmed.
    std::optional<OlderVersionRegistration> older_version = std::nullopt;
};

std::expected<RegisterOutcome, RegisterFailure> register_archive(
    RegisterServices& services,
    const RegisterRequest& request,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
