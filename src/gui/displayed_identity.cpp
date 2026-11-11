#include "gui/displayed_identity.h"

#include "common/time.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

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

// SAMPLE DATA: an ATA SMART page (SMART READ DATA, or with `thresholds`
// SMART READ THRESHOLDS) of the attributes given as {id, value, threshold,
// raw}, its checksum made to hold (decision 0052).
struct SampleAttribute {
    std::uint8_t id;
    std::uint8_t value;
    std::uint8_t threshold;
    std::uint64_t raw;
};
std::vector<std::uint8_t> sample_smart_page(const std::vector<SampleAttribute>& attributes, bool thresholds)
{
    std::vector<std::uint8_t> page(512, 0);
    page[0] = 0x10;
    for (std::size_t index = 0; index < attributes.size() && index < 30; ++index) {
        const auto& attribute = attributes[index];
        const std::size_t at = 2 + 12 * index;
        page[at] = attribute.id;
        if (thresholds) {
            page[at + 1] = attribute.threshold;
            continue;
        }
        page[at + 1] = attribute.threshold != 0 ? 0x33 : 0x32;  // pre-failure where it has a threshold
        page[at + 3] = attribute.value;
        page[at + 4] = attribute.value;
        for (std::size_t byte = 0; byte < 6; ++byte) {
            page[at + 5 + byte] = static_cast<std::uint8_t>(attribute.raw >> (8 * byte));
        }
    }
    unsigned sum = 0;
    for (std::size_t index = 0; index < 511; ++index) {
        sum += page[index];
    }
    page[511] = static_cast<std::uint8_t>(-sum & 0xff);
    return page;
}

// SAMPLE DATA: a device identity reader that reads nothing. Each sample
// device of sample_volumes gets a made-up identity and a made-up SMART read
// (decision 0052): SAMPLE-DEVICE-02, which reports a warning, has 8
// reallocated and 8 pending sectors; the others report nothing amiss; any
// other device passes no command on.
class SampleDeviceIdentityHelper final : public IDeviceIdentityHelper {
public:
    bool available() const override { return true; }

    std::expected<std::vector<DeviceRead>, IoError> read_devices(
        const std::vector<std::string>& device_refs, CancelToken& cancel) override
    {
        if (cancel.cancelled()) {
            return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "cancelled"});
        }
        const auto now = current_timestamp();
        std::vector<DeviceRead> reads;
        for (const auto& device_ref : device_refs) {
            DeviceRead read;
            read.device_ref = device_ref;
            const PhysicalDeviceInfo* seen = nullptr;
            const auto volumes = sample_volumes();
            for (const auto& sample : volumes) {
                if (sample.volume.devices.front().device_ref == device_ref) {
                    seen = &sample.volume.devices.front();
                }
            }
            SmartRead smart;
            smart.read_at = now;
            if (!seen) {
                read.identity = std::unexpected(IoError{IoErrorKind::CommandNotPassed, 0,
                    "the device's enclosure passes no ATA command on to the device"});
                reads.push_back(std::move(read));
                continue;
            }
            smart.seen = *seen;
            smart.seen.health.reset();
            DeviceIdentityRead identity;
            identity.device = seen->enclosure_hides_identity
                                  ? DeviceIdentity{"SAMPLE-OWN-MODEL", sample_own_serial, "SAMPLE-OWN-WWN-01"}
                                  : DeviceIdentity{seen->model, seen->serial, ""};
            identity.how = "sample";
            identity.read_at = now;
            identity.device_ref = device_ref;
            read.identity = identity;
            const bool failing = device_ref == "SAMPLE-DEVICE-REF-02";
            const std::vector<SampleAttribute> attributes{
                {0x01, 100, 16, 0},
                {0x05, failing ? std::uint8_t{98} : std::uint8_t{100}, 5, failing ? 8u : 0u},
                {0x09, 92, 0, 8'760},
                {0x0C, 100, 0, 412},
                {0xC2, 64, 0, 36},
                {0xC5, 100, 0, failing ? 8u : 0u},
                {0xC6, 100, 0, 0},
                {0xC7, 200, 0, 0},
            };
            smart.device = identity.device;
            smart.firmware = "SAMPLE01";
            smart.how = seen->bus == "sample-usb" ? "sat" : "ata";
            smart.data = sample_smart_page(attributes, false);
            smart.thresholds = sample_smart_page(attributes, true);
            read.smart = std::move(smart);
            reads.push_back(std::move(read));
        }
        return reads;
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
