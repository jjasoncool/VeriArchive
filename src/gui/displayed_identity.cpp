#include "gui/displayed_identity.h"

#include "common/time.h"

#include <cstdint>
#include <expected>
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
    constexpr std::uint64_t gib = std::uint64_t{1} << 30;
    const auto device = [](std::string number, std::uint64_t capacity, std::string bus, DeviceHealth health) {
        PhysicalDeviceInfo device;
        device.device_ref = "SAMPLE-DEVICE-REF-" + number;
        device.model = "SAMPLE-DEVICE-" + number;
        device.serial = "SAMPLE-SERIAL-" + number;
        device.capacity_bytes = capacity;
        device.bus = std::move(bus);
        device.health = std::move(health);
        return device;
    };
    const auto volume = [](const PhysicalDeviceInfo& on, std::string mount_point, std::string label,
                            std::string number, std::uint64_t capacity, std::uint64_t used, bool read_only = false) {
        VolumeInfo volume;
        volume.mount_point = std::filesystem::path(mount_point);
        volume.filesystem_type = "sample";
        volume.filesystem_uuid = "c0ffee00-0000-4000-8000-0000000005" + number;
        volume.volume_label = std::move(label);
        volume.capacity_bytes = capacity;
        volume.read_only = read_only;
        volume.devices.push_back(on);
        return SampleVolume{std::move(volume), used};
    };
    const auto first = device("01", 2 * sample_capacity, "sample-usb", {HealthStatus::Healthy, {}});
    const auto second = device("02", sample_capacity, "sample-sata", {HealthStatus::Warning, {"predicting-failure"}});
    // Behind a sample enclosure that hides the device's own serial, and
    // passes nothing of its health on: its serial is the enclosure's
    // (decision 0047).
    auto third = device("03", 64 * gib, "sample-usb", {});
    third.enclosure_hides_identity = true;
    return {
        volume(first, "/media/SAMPLE-VOLUME-1", "SAMPLE-VOLUME-1", "01", 2 * sample_capacity, 1'370 * gib),
        volume(second, "/media/SAMPLE-VOLUME-2", "SAMPLE-VOLUME-2", "02", 600 * gib, 412 * gib),
        volume(second, "/media/SAMPLE-VOLUME-3", "SAMPLE-VOLUME-3", "03", 424 * gib, 96 * gib, true),
        volume(third, "/media/SAMPLE-VOLUME-4", "", "04", 64 * gib, 18 * gib),
    };
}

namespace {

// SAMPLE DATA: a device identity reader that reads nothing.
class SampleDeviceIdentityHelper final : public IDeviceIdentityHelper {
public:
    bool available() const override { return true; }

    std::expected<DeviceIdentityRead, IoError> read_device_identity(
        const std::string& device_ref, CancelToken& cancel) override
    {
        if (cancel.cancelled()) {
            return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "cancelled"});
        }
        DeviceIdentityRead read;
        read.device = DeviceIdentity{"SAMPLE-OWN-MODEL", sample_own_serial, "SAMPLE-OWN-WWN-01"};
        read.how = "sample";
        read.read_at = current_timestamp();
        read.device_ref = device_ref;
        return read;
    }
};

} // namespace

std::unique_ptr<IDeviceIdentityHelper> sample_device_identity_helper()
{
    return std::make_unique<SampleDeviceIdentityHelper>();
}

std::string displayed_host(const std::string& recorded, IdentityShown shown)
{
    if (shown == IdentityShown::Recorded || recorded.empty()) {
        return recorded;
    }
    return "SAMPLE-HOST";
}

app::ShownIdentity shown_identity(IdentityShown shown)
{
    if (shown == IdentityShown::Recorded) {
        return {};
    }
    return {[](const VolumeInfo& recorded) { return displayed_identity(recorded, IdentityShown::Sample); },
        [](const std::vector<PhysicalDeviceInfo>& read) { return displayed_devices(read, IdentityShown::Sample); },
        [](const std::string& recorded) { return displayed_host(recorded, IdentityShown::Sample); }};
}

} // namespace va::gui
