#pragma once
// Loads a revision's manifest and accepts it only when its bytes are the ones
// the repository recorded (R-14): header and body BLAKE3 equal the database
// record, the body matches its own header (R-62), and it is not older than the
// highest known revision (R-15).
//
// The index also holds a copy of each revision's header (header_json). The
// validated manifest bytes are the truth and the database is an index
// (CLAUDE.md rule 3): wherever a recorded revision is loaded, that copy must
// be the manifest's line 1, byte for byte, and neither is ever taken in place
// of the other. Where only the copy differs from a manifest that is the one
// recorded, the index is wrong: `repo rebuild-index` rebuilds it from the
// manifests (R-80).
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <string>
#include <vector>

namespace va {

struct RecordedManifest {
    ManifestHeader header;
    std::vector<Entry> entries;
};

struct ManifestLoadFailure {
    std::string failure_class;  // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError
    std::string message;
};

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision);

// The header of a recorded revision as its manifest holds it, reading only
// the manifest's first line: for what uses a recorded revision without its
// entries (identity matching, a new revision's header, the archive lists).
// Line 1 must be the one the revision was recorded with (its BLAKE3, and the
// archive, revision and body it names), and the index's copy the same bytes.
// Otherwise a failure that names the archive and the revision: "cannot use
// the manifest of revision N of archive U: <cause>" when the manifest cannot
// be read or is not the one recorded, and, when only the index's copy
// differs, how to rebuild the index.
std::expected<ManifestHeader, IoError> load_recorded_header(
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const RevisionRecord& revision);

} // namespace va
