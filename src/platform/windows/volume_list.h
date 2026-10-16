#pragma once
// Which volumes Windows lists (IMediaEnumerator::volumes), and under which
// mount point, from what FindFirstVolumeW and GetVolumePathNamesForVolumeNameW
// give. Plain C++ without Windows headers, so it is tested on every OS, like
// the drive letters made with subst.
#include <string>
#include <utility>
#include <vector>

namespace va::platform::windows {

// A volume as Windows names it: its GUID path ("\\?\Volume{...}\") and its
// drive letters and folder paths, of which it may have none.
struct VolumeNames {
    std::wstring volume_name;
    std::vector<std::wstring> mount_points;
};

// Every volume, with the mount point under which it is listed and an archive
// on it is looked for: the first of its drive letters and folder paths, or,
// for a volume that has neither, its GUID path, its only name, which is taken
// as the root of an archive on it (decision 0029, point 3). So verification
// by archive uuid finds an archive on such a volume too.
inline std::vector<std::pair<std::wstring, std::wstring>> volumes_to_list(const std::vector<VolumeNames>& volumes)
{
    std::vector<std::pair<std::wstring, std::wstring>> listed;
    listed.reserve(volumes.size());
    for (const auto& volume : volumes) {
        listed.emplace_back(
            volume.volume_name, volume.mount_points.empty() ? volume.volume_name : volume.mount_points.front());
    }
    return listed;
}

} // namespace va::platform::windows
