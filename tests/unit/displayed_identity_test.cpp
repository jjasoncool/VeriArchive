// The media identity the GUI shows (src/gui/displayed_identity.*): as
// recorded in normal use; in the screenshot mode, whose pictures are public, a
// fixed sample identity that holds nothing of what was recorded.
#include "test_support.h"

#include "core/identity/volume_json.h"
#include "gui/displayed_identity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using va::gui::displayed_identity;
using va::gui::IdentityShown;

constexpr std::uint64_t one_tib = std::uint64_t{1} << 40;

// A recorded identity, made up for the test: a volume on two devices (as on
// Storage Spaces), every field set, each name marked RECORDED.
va::VolumeInfo recorded_volume()
{
    va::VolumeInfo volume;
    volume.mount_point = std::filesystem::path("Z:/");
    volume.filesystem_type = "ntfs";
    volume.filesystem_uuid = "c0ffee00c0ffee01";
    volume.filesystem_uuid_short = "c0ffee01";
    volume.volume_label = "RECORDED-LABEL";
    volume.volume_guid = "{c0ffee00-0000-4000-8000-000000000001}";
    volume.capacity_bytes = 8001563222016;
    volume.read_only = true;
    volume.container = "storage-spaces";
    va::PhysicalDeviceInfo first;
    first.device_ref = "\\\\.\\PhysicalDrive7";
    first.model = "RECORDED-MODEL-1";
    first.serial = "RECORDED-SERIAL-1";
    first.wwn = "eui.c0ffee0000000001";
    first.capacity_bytes = 4000787030016;
    first.rotational = true;
    first.bus = "usb";
    va::PhysicalDeviceInfo second;
    second.device_ref = "\\\\.\\PhysicalDrive8";
    second.model = "RECORDED-MODEL-2";
    second.serial = "RECORDED-SERIAL-2";
    second.wwn = "eui.c0ffee0000000002";
    second.capacity_bytes = 4000787030017;
    second.rotational = false;
    second.bus = "sata";
    volume.devices = {first, second};
    return volume;
}

// Another recorded identity, unlike the first in every field.
va::VolumeInfo other_volume()
{
    va::VolumeInfo volume;
    volume.mount_point = std::filesystem::path("Y:/");
    volume.filesystem_type = "exfat";
    volume.filesystem_uuid = "c0ffee02";
    volume.filesystem_uuid_short = "c0ffee02";
    volume.capacity_bytes = 128043712512;
    va::PhysicalDeviceInfo device;
    device.model = "RECORDED-MODEL-3";
    device.capacity_bytes = 128043712512;
    device.bus = "nvme";
    volume.devices = {device};
    return volume;
}

// The identity alone, as JSON text: without where and how the volume is
// attached now.
std::string identity_text(va::VolumeInfo volume)
{
    volume.mount_point.clear();
    volume.read_only = false;
    return va::volume_to_json_text(volume);
}

void recorded_identity_is_shown_as_recorded()
{
    // volume_to_json holds every field of the volume and its devices.
    for (const auto& recorded : {recorded_volume(), other_volume(), va::VolumeInfo{}}) {
        CHECK(va::volume_to_json_text(displayed_identity(recorded, IdentityShown::Recorded)) ==
              va::volume_to_json_text(recorded));
    }
}

void sample_identity_is_plainly_sample()
{
    const auto shown = displayed_identity(recorded_volume(), IdentityShown::Sample);
    CHECK(shown.volume_label == "SAMPLE-VOLUME");
    CHECK(shown.filesystem_type == "sample");
    CHECK(shown.filesystem_uuid == "SAMPLE-FILESYSTEM-ID");
    CHECK(shown.filesystem_uuid_short == "SAMPLE01");
    CHECK(shown.volume_guid == "SAMPLE-VOLUME-GUID");
    CHECK(shown.capacity_bytes == one_tib);  // "1.0 TiB" in the Archive List
    CHECK(shown.container.empty());
    CHECK(shown.devices.size() == 1);
    if (shown.devices.size() == 1) {
        const auto& device = shown.devices.front();
        CHECK(device.model == "SAMPLE-DEVICE");
        CHECK(device.serial == "SAMPLE0001");
        CHECK(device.wwn == "SAMPLE-WWN");
        CHECK(device.bus == "sample");
        CHECK(device.capacity_bytes == one_tib);
        CHECK(!device.rotational);  // neither HDD nor SSD
        CHECK(device.device_ref.empty());
    }
}

// Nothing of the recorded identity reaches the sample one.
void sample_holds_nothing_recorded()
{
    const auto recorded = recorded_volume();
    const auto shown = displayed_identity(recorded, IdentityShown::Sample);
    std::vector<std::string> values{recorded.filesystem_type, recorded.filesystem_uuid,
        recorded.filesystem_uuid_short, recorded.volume_label, recorded.volume_guid, recorded.container,
        std::to_string(recorded.capacity_bytes)};
    for (const auto& device : recorded.devices) {
        values.insert(values.end(), {device.device_ref, device.model, device.serial, device.wwn, device.bus,
                                        std::to_string(device.capacity_bytes)});
    }
    const auto shown_text = identity_text(shown);
    for (const auto& value : values) {
        CHECK(!value.empty());
        CHECK(shown_text.find(value) == std::string::npos);
    }
    CHECK(shown.devices.size() == 1);  // two were recorded
}

// The same sample whatever was recorded, even nothing at all.
void sample_identity_is_fixed()
{
    const auto first = identity_text(displayed_identity(recorded_volume(), IdentityShown::Sample));
    CHECK(identity_text(displayed_identity(other_volume(), IdentityShown::Sample)) == first);
    CHECK(identity_text(displayed_identity(va::VolumeInfo{}, IdentityShown::Sample)) == first);
}

// Where and how this computer has the volume attached now is no identity:
// the views' paths show the mount point anyway.
void sample_keeps_where_the_volume_is_attached()
{
    for (const auto& recorded : {recorded_volume(), other_volume()}) {
        const auto shown = displayed_identity(recorded, IdentityShown::Sample);
        CHECK(shown.mount_point == recorded.mount_point);
        CHECK(shown.read_only == recorded.read_only);
    }
}

// The devices that an operation read, as its report and its device alert
// name them (decision 0044): as read in normal use; in the screenshot mode
// each as the sample device, nothing read but its health.
void devices_read_are_shown_as_sample()
{
    auto read = recorded_volume().devices;
    read[0].health = va::DeviceHealth{va::HealthStatus::Warning, {"predicting-failure"}};
    CHECK(va::gui::displayed_devices(read, IdentityShown::Recorded).front().serial == "RECORDED-SERIAL-1");
    const auto shown = va::gui::displayed_devices(read, IdentityShown::Sample);
    CHECK(shown.size() == 2);
    for (const auto& device : shown) {
        CHECK(device.model == "SAMPLE-DEVICE" && device.serial == "SAMPLE0001" && device.wwn == "SAMPLE-WWN" &&
              device.device_ref.empty() && device.bus == "sample");
    }
    CHECK(shown[0].health && shown[0].health->status == va::HealthStatus::Warning &&
          shown[0].health->conditions == std::vector<std::string>{"predicting-failure"});
    CHECK(!shown[1].health);
    CHECK(va::gui::displayed_devices({}, IdentityShown::Sample).empty());
}

} // namespace

void displayed_identity_tests()
{
    recorded_identity_is_shown_as_recorded();
    sample_identity_is_plainly_sample();
    sample_holds_nothing_recorded();
    sample_identity_is_fixed();
    sample_keeps_where_the_volume_is_attached();
    devices_read_are_shown_as_sample();
}
