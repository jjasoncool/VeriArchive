#include "core/engine/attached.h"

#include "common/archive_path.h"
#include "core/engine/candidates.h"

#include <optional>

namespace va {
namespace {

// The root of an archive at `root_subpath` on a volume mounted at
// `mount_point`, as verify_engine's locate makes it: the mount point of a
// volume mounted in a folder ends with a separator on Windows ("C:\mnt\vol\"),
// which a root at that volume's root drops, while "X:\", "/" and a GUID path
// keep theirs.
std::optional<std::filesystem::path> root_on(const std::filesystem::path& mount_point, const std::string& root_subpath)
{
    if (root_subpath.empty()) {
        return without_trailing_separators(mount_point);
    }
    const auto relative = archive_path_to_native(ArchivePath{root_subpath, "", ""});
    if (!relative) {
        return std::nullopt;
    }
    return without_trailing_separators(mount_point / *relative);
}

bool holds(MatchResult result)
{
    return result == MatchResult::Match || result == MatchResult::MatchWeak;
}

} // namespace

Attachments where_attached(std::span<const VolumeInfo> volumes, std::span<const ArchiveCandidate> candidates)
{
    Attachments found;
    found.volumes.resize(volumes.size());
    found.archives.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        ArchiveAttachment attachment;
        attachment.archive_uuid = candidate.archive_uuid;
        std::vector<AttachedRoot> held;      // MATCH and MATCH_WEAK
        std::vector<AttachedRoot> migrated;  // MIGRATED
        for (std::size_t index = 0; index < volumes.size(); ++index) {
            const auto& volume = volumes[index];
            if (volume.mount_point.empty()) {
                continue;
            }
            // The candidate alone, at its own root, as a registration and a
            // verification by uuid match it: two archives at one root (twins
            // registered on purpose, R-97) are each found where they are.
            const MediaEvidence evidence{{}, volume, std::nullopt};
            auto match = match_identity(evidence, candidate.root_subpath, std::span(&candidate, 1));
            if (!holds(match.result) && match.result != MatchResult::Migrated) {
                continue;
            }
            const auto root = root_on(volume.mount_point, candidate.root_subpath);
            if (!root) {
                continue;
            }
            AttachedRoot place{candidate.archive_uuid, index, candidate.root_subpath, *root, match.result,
                std::move(match.details)};
            found.volumes[index].push_back(place);
            (holds(place.match) ? held : migrated).push_back(std::move(place));
        }
        if (held.size() == 1) {
            attachment.result = held.front().match;
            attachment.places = std::move(held);
        } else if (held.size() > 1) {
            attachment.result = MatchResult::Ambiguous;
            attachment.places = std::move(held);
        } else if (!migrated.empty()) {
            attachment.result = MatchResult::Migrated;
            attachment.places = std::move(migrated);
        }
        found.archives.push_back(std::move(attachment));
    }
    return found;
}

} // namespace va
