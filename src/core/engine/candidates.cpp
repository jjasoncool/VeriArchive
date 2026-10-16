#include "core/engine/candidates.h"

#include "common/archive_path.h"
#include "common/json_error.h"
#include "common/unicode.h"

namespace va {

std::expected<ManifestHeader, IoError> recorded_header(const RevisionRecord& revision)
{
    try {
        return header_from_json(nlohmann::json::parse(revision.header_json));
    } catch (const std::exception& error) {
        return std::unexpected(IoError{IoErrorKind::Other, 0,
            "invalid recorded manifest header for " + revision.archive_uuid + ": " + json_error_text(error)});
    }
}

std::expected<std::vector<ArchiveCandidate>, IoError> load_candidates(ArchiveStore& store)
{
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(archives.error());
    }
    std::vector<ArchiveCandidate> candidates;
    for (const auto& archive : *archives) {
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
        for (const auto& revision : *revisions) {
            const auto header = recorded_header(revision);
            if (!header) {
                return std::unexpected(header.error());
            }
            if (revision.revision == archive.current_revision) {
                candidate.identity = header->identity;
                candidate.root_subpath = header->root_subpath;
            } else {
                candidate.previous_carriers.push_back({header->identity, header->root_subpath});
            }
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
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
