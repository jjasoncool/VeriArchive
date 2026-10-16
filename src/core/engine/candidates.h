#pragma once
// Builds the identity-matching candidates (§B10) from the repository: each
// archive with the carrier of its latest revision and of earlier revisions
// (decision 0004), and whether it is retired (decision 0003).
#include "core/identity/matcher.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace va {

// Each archive with a recorded revision and the carrier of its latest
// revision, or only the archive `only` when the operator names one (by its
// uuid, or by the run of it to resume): a verification of a named archive
// reads no other archive's manifest. The carriers of earlier revisions are
// left out until matching looks at them (match_recorded).
//
// The carriers come from the manifests' headers (load_recorded_header), never
// from the index's copies of them (R-14). Fails, naming the archive and the
// revision, when the manifest of a latest revision read cannot be read, is
// not the one recorded, or differs from the index: matching against what is
// left would be matching without that archive.
std::expected<std::vector<ArchiveCandidate>, IoError> load_candidates(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout,
    const std::optional<Uuid>& only = std::nullopt);

// match_identity against `candidates` from load_candidates. The matcher looks
// at previous carriers only when no latest carrier at `root_subpath` matched
// or mismatched (previous_carriers_consulted): only then are they read, from
// the manifests of the earlier revisions of every archive in `candidates`, and
// kept there, and the evidence matched again. So an earlier revision's
// manifest is read only where it decides the outcome (PREVIOUS_CARRIER or
// UNKNOWN), and there it fails as load_candidates does: without it, the media
// may be the earlier carrier that it names.
std::expected<MatchOutcome, IoError> match_recorded(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const MediaEvidence& evidence, std::string_view root_subpath,
    std::span<ArchiveCandidate> candidates);

// Path of the archive root relative to its volume's mount point, as an
// ArchivePath string ("" for the volume root).
std::expected<std::string, IoError> root_subpath_of(
    const std::filesystem::path& root, const VolumeInfo& volume);

// `root` without the separators it ends with, except a root's own: "D:\Photos\"
// and "/mnt/photos/" are the folders "D:\Photos" and "/mnt/photos", while "D:\"
// and "/" stay as they are. Shells complete a folder name with a separator, so
// the user interfaces make every root they give the engines this way: one
// folder then has one root_hint and one root in plans and runs. Plans are
// found by their root compared this way too.
std::filesystem::path without_trailing_separators(std::filesystem::path root);

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
