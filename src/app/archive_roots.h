#pragma once
// The roots of the archives that hold a folder, on the volume of that folder
// as it is attached now: a file that VeriArchive writes there (an export of a
// manifest, a verification report) would be an entry that the next FULL
// verification of that archive reports as added, failing it. The window's
// export and Save report..., and `veriarchive-cli report`, ask it of the
// folder they write into, and refuse that folder (decision 0046, 0048).
#include "common/types.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <filesystem>
#include <string>
#include <vector>

namespace va::app {

// An archive root on an attached volume, and the archive's label.
struct AttachedArchiveRoot {
    std::filesystem::path root;
    std::string label;
};

// The roots of the archives on the volume of `folder` (media.volume_for_path)
// that are `folder` or hold it, as identity matching finds them there now
// (where_attached: MATCH, MATCH_WEAK, MIGRATED and AMBIGUOUS alike, each an
// archive's folder there) from the archives' latest manifests
// (load_candidate_pool), each named by its record's label, or the start of
// its uuid where it has none. A root holds `folder` as their paths are written
// (path_within) or as the file system names the two (platform::final_path,
// decision 0029, point 6): `folder` written through a symbolic link, a
// junction, a drive letter made with subst or a short (8.3) name is the folder
// it names, and its volume is looked up so too (a drive letter made with subst
// as the folder it stands for, decision 0027). Each archive once, its root as
// the volume of `folder` as written names it where that volume holds it.
// Nothing where the volume or the archives cannot be read: the caller then
// writes no more than a file of its own.
std::vector<AttachedArchiveRoot> roots_holding(ArchiveStore& store, IFileSystemAccess& repository_files,
    IMediaEnumerator& media, const RepositoryLayout& layout, const std::filesystem::path& folder);

} // namespace va::app
