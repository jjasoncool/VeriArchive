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

void VolumeWatch::answered()
{
    io_errors_in_a_row_ = 0;
}

ReadFailure VolumeWatch::failed(const IoError& error)
{
    if (stop_ != DeviceStop::None) {
        return ReadFailure::Stop;
    }
    if (error.kind == IoErrorKind::DeviceRemoved) {
        stop_ = DeviceStop::WentAway;
        return ReadFailure::Stop;
    }
    if (const auto now = media_.volume_for_path(root_); !now || !same_volume(volume_, *now)) {
        stop_ = DeviceStop::WentAway;
        return ReadFailure::Stop;
    }
    if (error.kind != IoErrorKind::MediaError && error.kind != IoErrorKind::Timeout) {
        return ReadFailure::Entry;
    }
    if (++io_errors_in_a_row_ >= io_errors_that_stop) {
        stop_ = DeviceStop::NotAnswering;
        return ReadFailure::Stop;
    }
    return ReadFailure::Held;
}

void VolumeWatch::watch_scan(ScanOptions& options)
{
    options.answered = [this] { answered(); };
    options.must_stop = [this](const IoError& error) { return failed(error) == ReadFailure::Stop; };
}

} // namespace va
