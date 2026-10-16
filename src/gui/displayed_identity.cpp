#include "gui/displayed_identity.h"

#include <cstdint>
#include <utility>

namespace va::gui {
namespace {

// A round size, plainly made up (a disk sold as 1 TB holds about 0.9 TiB):
// "1.0 TiB" in the Archive List.
constexpr std::uint64_t sample_capacity = std::uint64_t{1} << 40;

// The sample identity: one volume on one device, every value plainly made up.
VolumeInfo sample_identity()
{
    PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = "SAMPLE0001";
    device.wwn = "SAMPLE-WWN";
    device.capacity_bytes = sample_capacity;
    device.bus = "sample";
    // device_ref (the operating system's name for the device) stays empty,
    // and rotational unknown.

    VolumeInfo volume;
    volume.filesystem_type = "sample";
    volume.filesystem_uuid = "SAMPLE-FILESYSTEM-ID";
    volume.filesystem_uuid_short = "SAMPLE01";
    volume.volume_label = "SAMPLE-VOLUME";
    volume.volume_guid = "SAMPLE-VOLUME-GUID";
    volume.capacity_bytes = sample_capacity;
    volume.devices.push_back(std::move(device));
    // container stays empty: no BitLocker, LUKS or other container.
    return volume;
}

} // namespace

VolumeInfo displayed_identity(const VolumeInfo& recorded, IdentityShown shown)
{
    if (shown == IdentityShown::Recorded) {
        return recorded;
    }
    VolumeInfo sample = sample_identity();
    sample.mount_point = recorded.mount_point;
    sample.read_only = recorded.read_only;
    return sample;
}

} // namespace va::gui
