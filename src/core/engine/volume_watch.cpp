#include "core/engine/volume_watch.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace va {

bool same_volume(const VolumeInfo& before, const VolumeInfo& now)
{
    const auto filesystem = [](const VolumeInfo& volume) {
        return std::tie(volume.filesystem_uuid, volume.filesystem_uuid_short, volume.volume_guid);
    };
    const auto device = [](const PhysicalDeviceInfo& info) { return std::tie(info.model, info.serial, info.wwn); };
    return filesystem(before) == filesystem(now) &&
           std::ranges::equal(before.devices, now.devices,
               [&](const PhysicalDeviceInfo& left, const PhysicalDeviceInfo& right) {
                   return device(left) == device(right);
               });
}

VolumeWatch::VolumeWatch(IMediaEnumerator& media, std::filesystem::path root, VolumeInfo volume)
    : media_(media), root_(std::move(root)), volume_(std::move(volume))
{
}

bool VolumeWatch::still_attached()
{
    if (!gone_) {
        const auto now = media_.volume_for_path(root_);
        gone_ = !now || !same_volume(volume_, *now);
    }
    return !gone_;
}

} // namespace va
