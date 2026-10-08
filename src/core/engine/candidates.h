#pragma once
// Builds the identity-matching candidates (§B10) from the repository: each
// archive with the carrier of its latest revision and of earlier revisions
// (decision 0004), and whether it is retired (decision 0003).
#include "core/identity/matcher.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <filesystem>
#include <string>

namespace va {

std::expected<std::vector<ArchiveCandidate>, IoError> load_candidates(ArchiveStore& store);

// The header recorded for a revision (archive_revisions.header_json).
std::expected<ManifestHeader, IoError> recorded_header(const RevisionRecord& revision);

// Path of the archive root relative to its volume's mount point, as an
// ArchivePath string ("" for the volume root).
std::expected<std::string, IoError> root_subpath_of(
    const std::filesystem::path& root, const VolumeInfo& volume);

// Decision 0001: roots on one volume overlap when one equals or contains the
// other, compared case-insensitively.
bool roots_overlap(std::string_view left, std::string_view right);

// Where the process runs, recorded in manifests and history (§B4.3 created_by).
struct HostInfo {
    std::string os;           // "windows" or "linux"
    std::string host;
    std::string app_version;
};

} // namespace va
