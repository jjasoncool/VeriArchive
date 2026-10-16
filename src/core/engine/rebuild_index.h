#pragma once
// `repo rebuild-index` (R-80): manifests are the truth, so the index tables
// (archive_revisions, files) can be rebuilt from manifests/ alone. Archive rows
// keep what only the database knows (label changes, RETIRED status, notes);
// history tables are never touched — they are protected by the hash chain.
// Only what the manifests define is rebuilt: the saved progress of an
// unfinished registration, its rows of files under the revision it creates,
// stays while its plan exists and no manifest defines that revision (decision
// 0026); that of verifications and relocations is in verify_progress, which is
// never touched either. And a revision whose manifest it cannot read (missing,
// or skipped) keeps its record and its index as they were, as no manifest can
// rebuild them; each archive is at the latest revision that a manifest in its
// folder, read or not, or a record kept names (decision 0028).
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <string>
#include <vector>

namespace va {

struct RebuildReport {
    std::size_t archives = 0;
    std::size_t revisions = 0;
    std::size_t entries = 0;
    // Unfinished registrations whose saved progress stayed.
    std::size_t saved_progress_kept = 0;
    // Revisions whose manifest could not be read (missing, or skipped), whose
    // record and index stayed as they were (decision 0028).
    std::size_t revisions_kept = 0;
    // Manifests that failed validation, with the reason: "cannot read manifest
    // <uuid>/r0001.vam: <cause>" or "cannot list the manifests of archive <uuid>: <cause>".
    std::vector<std::string> skipped;
};

std::expected<RebuildReport, IoError> rebuild_index(
    ArchiveStore& store, IFileSystemAccess& repository_files, const RepositoryLayout& layout);

} // namespace va
