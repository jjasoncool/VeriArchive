#pragma once
// Which registered archive each attached volume holds, and where (decision
// 0046): what the window's Register Archive and Devices pages show of the
// volumes attached now. Told by identity matching alone (§B10, match_identity),
// from the volumes that IMediaEnumerator::volumes() lists and the candidates
// of the archives' latest revisions (load_candidate_pool), exactly as a
// verification by an archive's uuid looks for its carrier and as a
// registration finds the archives at its root: each archive is matched on its
// own, at its own root subpath. Pure logic, no I/O: no folder is looked at,
// so an archive's folder that was moved or renamed on its volume still counts
// as attached there.
#include "core/identity/matcher.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace va {

// An archive's root on an attached volume, as matching finds it there: the
// volume's identity matches the carrier that the archive's latest revision
// records (MATCH, or MATCH_WEAK where a device serial is missing on one side,
// as behind an enclosure that hides it), or only its filesystem does, on
// another device (MIGRATED: a new enclosure, or a sector-by-sector clone).
struct AttachedRoot {
    Uuid archive_uuid;
    std::size_t volume = 0;                  // the volume's index in the volumes given
    std::string root_subpath;                // as the archive records it ("" = the volume root)
    std::filesystem::path root;              // the volume's mount point (or GUID path) with the root subpath,
                                             // made as the user interfaces make a root (without_trailing_separators)
    MatchResult match = MatchResult::Match;  // Match, MatchWeak or Migrated
    std::vector<std::string> details;        // as match_identity gives them ("volume-label-differs", "retired", ...)
};

// Where one archive is attached now:
//  - Match or MatchWeak: on one volume, at places.front();
//  - Ambiguous: on more than one volume, each a MATCH or MATCH_WEAK (a device
//    and a sector-by-sector clone of it that show no serial), all in
//    `places`; a verification by the archive's uuid stops there, naming them;
//  - Migrated: on no volume, but its filesystem is on another device than
//    the one recorded (`places`, each MIGRATED);
//  - Unknown: on no attached volume.
struct ArchiveAttachment {
    Uuid archive_uuid;
    MatchResult result = MatchResult::Unknown;
    std::vector<AttachedRoot> places;
};

struct Attachments {
    std::vector<ArchiveAttachment> archives;         // one for each candidate, in their order
    std::vector<std::vector<AttachedRoot>> volumes;  // one for each volume, in their order: the roots of
                                                     // archives on it, MIGRATED ones included, in the
                                                     // candidates' order
};

// A volume without a mount point holds nothing (it cannot be read), and an
// archive whose root subpath is no path of this system is on no volume. The
// candidates' earlier carriers are not looked at: an earlier carrier is no
// archive's root now (decision 0004).
Attachments where_attached(std::span<const VolumeInfo> volumes, std::span<const ArchiveCandidate> candidates);

} // namespace va
