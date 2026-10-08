#pragma once
// `repo rebuild-index` (R-80): manifests are the truth, so the index tables
// (archive_revisions, files) can be rebuilt from manifests/ alone. Archive rows
// keep what only the database knows (label changes, RETIRED status, notes);
// history tables are never touched — they are protected by the hash chain.
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <string>
#include <vector>

namespace va {

struct RebuildReport {
    std::size_t archives = 0;
    std::size_t revisions = 0;
    std::size_t entries = 0;
    // Manifests that failed validation, with the reason: "cannot read manifest
    // <uuid>/r0001.vam: <cause>" or "cannot list the manifests of archive <uuid>: <cause>".
    std::vector<std::string> skipped;
};

std::expected<RebuildReport, IoError> rebuild_index(
    ArchiveStore& store, IFileSystemAccess& repository_files, const RepositoryLayout& layout);

} // namespace va
