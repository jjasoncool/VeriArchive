#include "core/engine/candidates.h"

#include "common/archive_path.h"
#include "common/unicode.h"
#include "core/engine/recorded_manifest.h"

#include <algorithm>

namespace va {

std::expected<std::vector<ArchiveCandidate>, IoError> load_candidates(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const std::optional<Uuid>& only)
{
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(archives.error());
    }
    std::vector<ArchiveCandidate> candidates;
    for (const auto& archive : *archives) {
        if (only && archive.archive_uuid != *only) {
            continue;
        }
        const auto revisions = store.revisions(archive.archive_uuid);
        if (!revisions) {
            return std::unexpected(revisions.error());
        }
        if (revisions->empty()) {
            continue; // registration never finished
        }
        ArchiveCandidate candidate;
        candidate.archive_uuid = archive.archive_uuid;
        candidate.retired = archive.status == "RETIRED";
        const auto latest = std::ranges::find(*revisions, archive.current_revision, &RevisionRecord::revision);
        if (latest != revisions->end()) {
            const auto header = load_recorded_header(repository_files, layout, *latest);
            if (!header) {
                return std::unexpected(header.error());
            }
            candidate.identity = header->identity;
            candidate.root_subpath = header->root_subpath;
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::expected<MatchOutcome, IoError> match_recorded(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const MediaEvidence& evidence, std::string_view root_subpath,
    std::span<ArchiveCandidate> candidates)
{
    const auto outcome = match_identity(evidence, root_subpath, candidates);
    if (!outcome.previous_carriers_consulted) {
        return outcome;
    }
    // Decision 0004: the carriers of every revision but the latest, each as
    // its manifest names it.
    for (auto& candidate : candidates) {
        const auto archive = store.find_archive(candidate.archive_uuid);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        const auto revisions = store.revisions(candidate.archive_uuid);
        if (!revisions) {
            return std::unexpected(revisions.error());
        }
        candidate.previous_carriers.clear();
        for (const auto& revision : *revisions) {
            if (*archive && revision.revision == (*archive)->current_revision) {
                continue;
            }
            const auto header = load_recorded_header(repository_files, layout, revision);
            if (!header) {
                return std::unexpected(header.error());
            }
            candidate.previous_carriers.push_back({header->identity, header->root_subpath});
        }
    }
    return match_identity(evidence, root_subpath, candidates);
}

std::expected<std::string, IoError> root_subpath_of(
    const std::filesystem::path& root, const VolumeInfo& volume)
{
    const auto relative = root.lexically_normal().lexically_relative(volume.mount_point.lexically_normal());
    if (relative.empty() || relative.native().starts_with(std::filesystem::path("..").native())) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, "the root is not on the reported volume"});
    }
    if (relative == ".") {
        return std::string{};
    }
    const auto archive_path = archive_path_from_native(relative);
    if (!archive_path) {
        return std::unexpected(archive_path.error());
    }
    return archive_path->p;
}

std::filesystem::path without_trailing_separators(std::filesystem::path root)
{
    // A path that ends with a separator has an empty file name; one without a
    // relative part is a root ("D:\", "/"). Each parent_path() drops one element.
    while (!root.has_filename() && root.has_relative_path()) {
        root = root.parent_path();
    }
    return root;
}

bool roots_overlap(std::string_view left, std::string_view right)
{
    const auto a = unicode_fold(left);
    const auto b = unicode_fold(right);
    return a.empty() || b.empty() || a == b || a.starts_with(b + "/") || b.starts_with(a + "/");
}

} // namespace va
