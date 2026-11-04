// The devices attached now with their health, asked once a device
// (src/app/attached_devices.*, decisions 0044 and 0046), as `media list` and
// the window's Devices page get them. Every volume and device below is sample
// data made up for the test (SAMPLE-* labels, models, serials and system
// names, c0ffee00-... ids, mount points under /media/SAMPLE-*); none is of a
// real volume or device.
#include "test_support.h"

#include "app/attached_devices.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

va::PhysicalDeviceInfo device(std::string device_ref, std::string serial)
{
    va::PhysicalDeviceInfo info;
    info.device_ref = std::move(device_ref);
    info.model = "SAMPLE-DEVICE";
    info.serial = std::move(serial);
    return info;
}

va::VolumeInfo volume(std::string mount_point, std::string filesystem_uuid, std::vector<va::PhysicalDeviceInfo> devices)
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_uuid = std::move(filesystem_uuid);
    info.volume_label = "SAMPLE-VOLUME";
    info.devices = std::move(devices);
    return info;
}

// Lists the volumes given; tells each device's health by its serial, and
// counts how often each serial was asked.
class CountingMedia final : public va::IMediaEnumerator {
public:
    std::vector<va::VolumeInfo> listed;
    std::map<std::string, va::DeviceHealth> health;
    std::map<std::string, int> asked;
    std::optional<va::IoError> error;
    std::optional<va::IoError> health_error;  // for every device

    std::expected<std::vector<va::VolumeInfo>, va::IoError> volumes() override
    {
        if (error) {
            return std::unexpected(*error);
        }
        return listed;
    }
    std::expected<va::VolumeInfo, va::IoError> volume_for_path(const std::filesystem::path&) override
    {
        return std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "not used by the test"});
    }
    std::expected<va::DeviceHealth, va::IoError> device_health(const va::PhysicalDeviceInfo& asked_for) override
    {
        ++asked[asked_for.serial];
        if (health_error) {
            return std::unexpected(*health_error);
        }
        const auto found = health.find(asked_for.serial);
        return found == health.end() ? va::DeviceHealth{} : found->second;
    }
};

void health_is_asked_once_a_device()
{
    CountingMedia media;
    // Two volumes on one device, one on another, and two devices the system
    // names nothing (no device_ref), each its own.
    media.listed = {
        volume("/media/SAMPLE-A", "c0ffee00-0000-4000-8000-0000000000a1", {device("SAMPLE-REF-1", "SAMPLE-SERIAL-1")}),
        volume("/media/SAMPLE-B", "c0ffee00-0000-4000-8000-0000000000a2", {device("SAMPLE-REF-2", "SAMPLE-SERIAL-2")}),
        volume("/media/SAMPLE-C", "c0ffee00-0000-4000-8000-0000000000a3", {device("SAMPLE-REF-1", "SAMPLE-SERIAL-1")}),
        volume("/media/SAMPLE-D", "c0ffee00-0000-4000-8000-0000000000a4", {device("", "SAMPLE-SERIAL-3")}),
        volume("/media/SAMPLE-E", "c0ffee00-0000-4000-8000-0000000000a5", {device("", "SAMPLE-SERIAL-4")}),
    };
    media.health["SAMPLE-SERIAL-1"] = {va::HealthStatus::Warning, {"predicting-failure"}};
    media.health["SAMPLE-SERIAL-2"] = {va::HealthStatus::Healthy, {}};
    const auto attached = va::app::attached_devices(media);
    CHECK(attached.has_value());
    if (!attached) {
        return;
    }
    CHECK(media.asked["SAMPLE-SERIAL-1"] == 1);
    CHECK(media.asked["SAMPLE-SERIAL-2"] == 1);
    CHECK(media.asked["SAMPLE-SERIAL-3"] == 1);
    CHECK(media.asked["SAMPLE-SERIAL-4"] == 1);
    // Every volume keeps its order, each device with the health it reported.
    CHECK(attached->volumes.size() == 5);
    CHECK(attached->volumes[0].devices.front().health &&
          attached->volumes[0].devices.front().health->status == va::HealthStatus::Warning);
    CHECK(attached->volumes[2].devices.front().health &&
          attached->volumes[2].devices.front().health->conditions == std::vector<std::string>{"predicting-failure"});
    CHECK(attached->volumes[1].devices.front().health &&
          attached->volumes[1].devices.front().health->status == va::HealthStatus::Healthy);
    CHECK(attached->volumes[3].devices.front().health &&
          attached->volumes[3].devices.front().health->status == va::HealthStatus::Unknown);
    // The devices: SAMPLE-REF-1 once with both its volumes, then as the
    // volumes first name them.
    CHECK(attached->devices.size() == 4);
    if (attached->devices.size() == 4) {
        CHECK(attached->devices[0].device.device_ref == "SAMPLE-REF-1");
        CHECK((attached->devices[0].volumes == std::vector<std::size_t>{0, 2}));
        CHECK(attached->devices[0].device.health && attached->devices[0].device.health->status == va::HealthStatus::Warning);
        CHECK(attached->devices[1].device.device_ref == "SAMPLE-REF-2");
        CHECK((attached->devices[1].volumes == std::vector<std::size_t>{1}));
        CHECK(attached->devices[2].device.serial == "SAMPLE-SERIAL-3");
        CHECK((attached->devices[2].volumes == std::vector<std::size_t>{3}));
        CHECK(attached->devices[3].device.serial == "SAMPLE-SERIAL-4");
    }
}

void a_device_that_cannot_be_asked_is_unknown()
{
    CountingMedia media;
    media.listed = {
        volume("/media/SAMPLE-A", "c0ffee00-0000-4000-8000-0000000000b1", {device("SAMPLE-REF-1", "SAMPLE-SERIAL-1")})};
    media.health_error = va::IoError{va::IoErrorKind::Other, 0, "SAMPLE: the device cannot be asked"};
    const auto attached = va::app::attached_devices(media);
    CHECK(attached && attached->volumes.front().devices.front().health &&
          attached->volumes.front().devices.front().health->status == va::HealthStatus::Unknown);

    // Volumes that cannot be listed: the failure, and no device is asked.
    CountingMedia failing;
    failing.listed = media.listed;
    failing.error = va::IoError{va::IoErrorKind::Other, 0, "SAMPLE: the volumes cannot be listed"};
    const auto none = va::app::attached_devices(failing);
    CHECK(!none && none.error().message == "SAMPLE: the volumes cannot be listed");
    CHECK(failing.asked.empty());
}

void devices_of_groups_without_asking()
{
    // A volume on two devices (spanned), one of them also behind another
    // volume; a volume without a device.
    const std::vector volumes{
        volume("/media/SAMPLE-A", "c0ffee00-0000-4000-8000-0000000000c1",
            {device("SAMPLE-REF-1", "SAMPLE-SERIAL-1"), device("SAMPLE-REF-2", "SAMPLE-SERIAL-2")}),
        volume("/media/SAMPLE-B", "c0ffee00-0000-4000-8000-0000000000c2", {}),
        volume("/media/SAMPLE-C", "c0ffee00-0000-4000-8000-0000000000c3", {device("SAMPLE-REF-2", "SAMPLE-SERIAL-2")}),
    };
    const auto devices = va::app::devices_of(volumes);
    CHECK(devices.size() == 2);
    if (devices.size() == 2) {
        CHECK((devices[0].volumes == std::vector<std::size_t>{0}));
        CHECK((devices[1].volumes == std::vector<std::size_t>{0, 2}));
        CHECK(!devices[0].device.health);
    }
    CHECK(va::app::devices_of({}).empty());
}

} // namespace

void attached_devices_tests()
{
    health_is_asked_once_a_device();
    a_device_that_cannot_be_asked_is_unknown();
    devices_of_groups_without_asking();
}
