#include "gui/displayed_identity.h"

#include <cstdint>
#include <utility>

namespace va::gui {
namespace {

// A round size, plainly made up (a disk sold as 1 TB holds about 0.9 TiB):
// "1.0 TiB" in Archives.
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

std::vector<PhysicalDeviceInfo> displayed_devices(const std::vector<PhysicalDeviceInfo>& read, IdentityShown shown)
{
    if (shown == IdentityShown::Recorded) {
        return read;
    }
    std::vector<PhysicalDeviceInfo> devices;
    for (const auto& device : read) {
        auto sample = sample_identity().devices.front();
        sample.health = device.health;
        devices.push_back(std::move(sample));
    }
    return devices;
}

std::vector<SampleVolume> sample_volumes()
{
    // SAMPLE DATA: made up for the screenshot mode's pictures.
    const auto sample = [](std::string mount_point, std::string label, std::string number, std::uint64_t capacity,
                            std::uint64_t used, std::string bus) {
        PhysicalDeviceInfo device;
        device.model = "SAMPLE-DEVICE-" + number;
        device.serial = "SAMPLE-SERIAL-" + number;
        device.capacity_bytes = capacity;
        device.bus = std::move(bus);
        VolumeInfo volume;
        volume.mount_point = std::filesystem::path(mount_point);
        volume.filesystem_type = "sample";
        volume.filesystem_uuid = "c0ffee00-0000-4000-8000-0000000005" + number;
        volume.volume_label = std::move(label);
        volume.capacity_bytes = capacity;
        volume.devices.push_back(std::move(device));
        return SampleVolume{std::move(volume), used};
    };
    constexpr std::uint64_t gib = std::uint64_t{1} << 30;
    return {
        sample("/media/SAMPLE-VOLUME-1", "SAMPLE-VOLUME-1", "01", 2 * sample_capacity, 1'370 * gib, "sample-usb"),
        sample("/media/SAMPLE-VOLUME-2", "SAMPLE-VOLUME-2", "02", sample_capacity, 412 * gib, "sample-sata"),
        sample("/media/SAMPLE-UNLABELLED", "", "03", 64 * gib, 18 * gib, "sample-usb"),
    };
}

std::string displayed_host(const std::string& recorded, IdentityShown shown)
{
    if (shown == IdentityShown::Recorded || recorded.empty()) {
        return recorded;
    }
    return "SAMPLE-HOST";
}

} // namespace va::gui
