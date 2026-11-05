#include "app/archive_roots.h"

#include "core/engine/attached.h"
#include "core/engine/candidates.h"
#include "platform/platform_services.h"

#include <optional>
#include <set>
#include <utility>

namespace va::app {

std::vector<AttachedArchiveRoot> roots_holding(ArchiveStore& store, IFileSystemAccess& repository_files,
    IMediaEnumerator& media, const RepositoryLayout& layout, const std::filesystem::path& folder)
{
    std::vector<AttachedArchiveRoot> holding;
    // The folder as the file system names it (decision 0029, point 6):
    // written through a symbolic link, a junction, a drive letter made with
    // subst or a short (8.3) name, it is the folder that it names, which an
    // archive's root holds although the root's path does not hold the folder
    // as written.
    const auto named = platform::final_path(folder);
    // Its volume as written, a drive letter made with subst as the folder it
    // stands for, which Windows reports no volume for (decision 0027); and,
    // unless that volume holds the folder as named, the volume of the folder
    // as named, which may be another one (a link to a folder there).
    std::vector<VolumeInfo> volumes;
    if (auto volume = media.volume_for_path(platform::resolve_subst_drive(folder))) {
        volumes.push_back(std::move(*volume));
    }
    if (named && (volumes.empty() || !path_within(volumes.front().mount_point, *named))) {
        if (auto volume = media.volume_for_path(*named)) {
            volumes.push_back(std::move(*volume));
        }
    }
    if (volumes.empty()) {
        return holding;
    }
    const auto pool = load_candidate_pool(store, repository_files, layout);
    if (!pool) {
        return holding;
    }
    // As written, or both as the file system names them; by their text alone
    // where it cannot tell either.
    const auto holds = [&](const std::filesystem::path& root) {
        if (path_within(root, folder)) {
            return true;
        }
        const auto root_named = named ? platform::final_path(root) : std::nullopt;
        return root_named && path_within(*root_named, *named);
    };
    // An archive found on both volumes (one volume under two names) once, as
    // the folder's volume as written names its root.
    std::set<Uuid> listed;
    for (const auto& on_volume : where_attached(volumes, pool->candidates).volumes) {
        for (const auto& root : on_volume) {
            if (!holds(root.root) || !listed.insert(root.archive_uuid).second) {
                continue;
            }
            std::string label = root.archive_uuid.substr(0, 8);
            if (const auto record = store.find_archive(root.archive_uuid);
                record && *record && !(*record)->label.empty()) {
                label = (*record)->label;
            }
            holding.push_back({root.root, label});
        }
    }
    return holding;
}

} // namespace va::app
