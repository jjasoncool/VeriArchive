#pragma once
// `repo rebuild-index` (R-80): manifests are the truth, so the index tables
// (archive_revisions, files) can be rebuilt from manifests/ alone. Archive rows
// keep what only the database knows (label changes, RETIRED status, notes);
// history tables are never touched — they are protected by the hash chain.
// Only what the manifests define is rebuilt: the saved progress of an
// unfinished registration, its rows of files under the revision it creates,
// stays while its plan exists and no manifest defines that revision (decision
// 0026); that of verifications and relocations is in verify_progress, which is
// never touched either. Each archive is at the latest revision that a
// manifest in its folder, read or not, or a record of a revision whose
// manifest it cannot read names, and never below the latest revision that a
// verification of it which read its media names (decision 0028); that
// revision, where its manifest cannot be read (missing, or skipped), keeps
// its record and its index as they were, as no manifest can rebuild them.
// The record of an earlier revision whose manifest it cannot read goes with
// its index, as every record that no manifest rebuilds.
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <optional>
#include <string>
#include <vector>

namespace va {

// A revision whose manifest repo rebuild-index could not read (decision 0028).
struct UnreadRevision {
    Uuid archive_uuid;
    std::string label;       // the archive's label
    std::uint32_t revision = 0;
    std::string manifest;    // its place in the repository ('/'-separated, as manifest_relpath)
    // Why the file at the revision's place (R-63) could not be read, as its
    // "skipped: ..." line ends; nothing where the file is missing.
    std::optional<std::string> cause;
};

struct RebuildReport {
    std::size_t archives = 0;
    std::size_t revisions = 0;
    std::size_t entries = 0;
    // Unfinished registrations whose saved progress stayed.
    std::size_t saved_progress_kept = 0;
    // Of the revisions whose manifest could not be read (decision 0028): the
    // archive's revision, whose record and index stayed as they were; an
    // earlier revision, whose record and index went; and the archive's
    // revision of which no record is left (a file names it but cannot be
    // read, or only its verification history names it), which the index then
    // has no record of.
    std::vector<UnreadRevision> revisions_kept;
    std::vector<UnreadRevision> revisions_left_out;
    std::vector<UnreadRevision> revisions_not_indexed;
    // Manifests that failed validation, with the reason: "cannot read manifest
    // <uuid>/r0001.vam: <cause>" or "cannot list the manifests of archive <uuid>: <cause>".
    std::vector<std::string> skipped;
};

std::expected<RebuildReport, IoError> rebuild_index(
    ArchiveStore& store, IFileSystemAccess& repository_files, const RepositoryLayout& layout);

} // namespace va
