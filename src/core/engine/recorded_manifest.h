#pragma once
// Loads a revision's manifest and accepts it only when its bytes are the ones
// the repository recorded (R-14): header and body BLAKE3 equal the database
// record, the body matches its own header (R-62), and it is not older than the
// highest known revision (R-15). Line 1 is compared with the BLAKE3 recorded
// for it before it is parsed (decision 0028): a manifest whose line 1 is not
// the one recorded fails with ManifestHashMismatch, whatever that line holds.
//
// The index also holds a copy of each revision's header (header_json). The
// validated manifest bytes are the truth and the database is an index
// (CLAUDE.md rule 3): wherever a recorded revision is loaded, that copy must
// be the manifest's line 1, byte for byte, and neither is ever taken in place
// of the other. Where only the index differs from a manifest that is the one
// recorded (its line 1 has the BLAKE3 recorded for it), as a copy that is not
// that line or a record that names another archive, revision or body than it,
// the index is wrong: `repo rebuild-index` rebuilds it from the manifests
// (R-80).
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <optional>
#include <string>
#include <vector>

namespace va {

struct RecordedManifest {
    ManifestHeader header;
    std::vector<Entry> entries;
};

// The manifest of a recorded revision that cannot be used (decision 0028),
// and why.
struct UnusableManifest {
    enum class Kind {
        Missing,      // the file is gone
        Unreadable,   // it cannot be read: `cause`
        Changed,      // its line 1 or its body is not the one recorded: `cause` says which
        IndexDiffers, // it is the one recorded (its line 1 has the BLAKE3 recorded for it), but the
                      // index says otherwise: its copy of the header is not that line, or the record
                      // names another archive, revision or body than that line does
    };
    Kind kind = Kind::Unreadable;
    Uuid archive_uuid;
    std::uint32_t revision = 0;
    std::string manifest;  // where it is in the repository, '/'-separated (manifest_relpath)
    std::string cause;
};

// The run-level failure class (§A6) of a verification whose manifest cannot
// be used: ManifestHashMismatch for a manifest that has changed,
// RepositoryError for one that is missing or cannot be read, or whose header
// the index holds otherwise.
std::string failure_class_of(const UnusableManifest& unusable);

// What is wrong, naming the manifest: "the manifest of revision 2
// (manifests/<archive_uuid>/r0002.vam) is missing".
std::string what_is_wrong_with(const UnusableManifest& unusable);

// The plain message: what is wrong, then how to fix it, which is to restore
// the manifest from a backup of the repository (R-86) or, where only the
// index's copy of its header differs, to rebuild the index (R-80). Rebuilding
// the index from a manifest that is missing or has changed would take what
// the file holds now for the truth.
std::string plain_message(const UnusableManifest& unusable);

struct ManifestLoadFailure {
    std::string failure_class;  // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError
    std::string message;
    // Set when the manifest cannot be used; `message` is then its plain
    // message, `failure_class` its class. Not set for a revision that is not
    // recorded or is older than the highest known one (R-15).
    std::optional<UnusableManifest> unusable;
};

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision);

// As load_recorded_manifest, without reading the entries: whether the
// manifest can be used (its line 1, its body's hash, the index's copy of its
// header, R-15), and its header when it can. For a verification that checks
// the manifest of a known archive before it looks for the media (decision
// 0028); the entries are read again once the media is found.
std::expected<ManifestHeader, ManifestLoadFailure> check_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision);

// The header of a recorded revision as its manifest holds it, reading only
// the manifest's first line: for what uses a recorded revision without its
// entries (identity matching, a new revision's header, the archive lists).
// Line 1 must be the one the revision was recorded with (its BLAKE3), the
// record must name the archive, revision and body that line names, and the
// index's copy must be the same bytes. Otherwise the manifest cannot be used
// (decision 0028): it is missing, cannot be read, has changed (its line 1),
// or the index says otherwise of it (the record, or its copy of the header).
std::expected<ManifestHeader, UnusableManifest> load_recorded_header(
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const RevisionRecord& revision);

} // namespace va
