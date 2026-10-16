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
#include <string_view>
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
        NotIndexed,   // the manifest of the archive's revision, as the archive's record says it, of
                      // which the index has no record (as where `repo rebuild-index` could not read
                      // it and no record of it was left): `file` says what the file at its place is
    };
    // NotIndexed: the file at the manifest's place (R-63), read as `repo
    // rebuild-index` reads a manifest, as no record vouches for it: one that
    // reads as the manifest of this revision of this archive (Readable), one
    // that is gone (Missing), or one that cannot be read (Unreadable: `cause`).
    enum class File { Readable, Missing, Unreadable };
    Kind kind = Kind::Unreadable;
    Uuid archive_uuid;
    std::uint32_t revision = 0;
    std::string manifest;  // where it is in the repository, '/'-separated (manifest_relpath)
    std::string cause;
    File file = File::Readable;  // NotIndexed only
};

// The run-level failure class (§A6) of a verification whose manifest cannot
// be used: ManifestHashMismatch for a manifest that has changed,
// RepositoryError for one that is missing or cannot be read, or of which the
// index says otherwise (IndexDiffers) or has no record (NotIndexed).
std::string failure_class_of(const UnusableManifest& unusable);

// The kind in English for --json (`manifest_problem.kind`): "missing",
// "unreadable", "changed", "index_differs" or "not_indexed".
std::string_view kind_code(UnusableManifest::Kind kind);

// What is wrong, naming the manifest: "the manifest of revision 2
// (manifests/<archive_uuid>/r0002.vam) is missing", or "the repository's
// index has no record of the manifest of revision 2 (...), which is missing".
std::string what_is_wrong_with(const UnusableManifest& unusable);

// The plain message: what is wrong, then how to fix it, which is to restore
// the manifest from a backup of the repository (R-86) or, where only the
// index differs from a manifest that is the one recorded (IndexDiffers), to
// rebuild the index (R-80). Rebuilding the index from a manifest that is
// missing or has changed would take what the file holds now for the truth.
// Where the index has no record of the manifest (NotIndexed), only
// rebuilding the index records it again: after the manifest is restored,
// where it is missing or cannot be read.
std::string plain_message(const UnusableManifest& unusable);

// The place of the manifest of `revision` of the archive in the repository
// (R-63), '/'-separated, as a revision's record names it (manifest_relpath):
// "manifests/<archive_uuid>/r0002.vam".
std::string manifest_relpath_of(const Uuid& archive_uuid, std::uint32_t revision);

// The manifest of `revision` of the archive, of which the index has no
// record, though the archive's record says that revision (decision 0028):
// it cannot be used (NotIndexed), and the file at its place is read as `repo
// rebuild-index` reads a manifest, to say how to fix it. `whole`: its body
// against its hash and every entry, as the rebuild reads it, as before a
// verification; otherwise line 1 alone, as for the archive lists and
// identity matching.
UnusableManifest manifest_not_indexed(IFileSystemAccess& repository_files, const RepositoryLayout& layout,
    const Uuid& archive_uuid, std::uint32_t revision, bool whole);

struct ManifestLoadFailure {
    std::string failure_class;  // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError
    std::string message;
    // Set when the manifest cannot be used; `message` is then its plain
    // message, `failure_class` its class. Not set for a revision that is
    // older than the highest known one (R-15), nor for one that is not
    // recorded and is not the archive's revision either; the archive's
    // revision that is not recorded cannot be used (NotIndexed).
    std::optional<UnusableManifest> unusable;
};

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision);

// As load_recorded_manifest, without reading the entries: whether the
// manifest can be used (its line 1, its body's hash, the index's record and
// copy of its header, R-15), and its header when it can. For a verification
// that checks the manifest of a known archive before it looks for the media
// (decision 0028); the entries are read again once the media is found.
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
