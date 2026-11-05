#pragma once
// The media identity the views show. Normal use shows what was recorded: the
// volume and its devices as a manifest header or media evidence holds them.
// The user guide's pictures (screenshot mode, tools/screenshots) are public,
// and the sample archives in them are folders on a real volume of the
// computer that takes them, so what was recorded is that volume's own label,
// its device's model and serial, its capacity. While the screenshot mode runs,
// every view therefore shows a fixed sample identity instead.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/shown_identity.h"
#include "common/types.h"
#include "core/interfaces.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace va::gui {

enum class IdentityShown {
    Recorded,  // what was recorded: normal use
    Sample,    // the fixed sample identity: the screenshot mode
};

// The volume identity, with its devices, that a view shows for `recorded`.
// Every view that shows media identity takes it from here, and only shows it:
// matching and every other decision use what was recorded.
//
// Sample: the same values whatever was recorded, plainly sample and never
// translated, so alike in every interface language: volume label
// SAMPLE-VOLUME, one device, model SAMPLE-DEVICE, serial SAMPLE0001, a round
// capacity of 1 TiB, sample values in every other identity field (filesystem
// type and id, volume GUID, WWN, bus) and no container; whether the device
// rotates is unknown, so neither HDD nor SSD is shown. Kept from `recorded`
// are only the mount point and read_only: where and how this computer has the
// volume attached now. They are runtime state, never part of a recorded
// identity (§B4.3), and the paths the views show hold the mount point anyway.
VolumeInfo displayed_identity(const VolumeInfo& recorded, IdentityShown shown);

// The devices that an operation read, with the health each reported
// (decision 0044), as a report or a device alert of a view names them:
// Sample, each as the sample identity's device, keeping only its health.
std::vector<PhysicalDeviceInfo> displayed_devices(const std::vector<PhysicalDeviceInfo>& read, IdentityShown shown);

// The attached volumes that a view lists in the screenshot mode, in place of
// this computer's, which are then never listed: SAMPLE DATA, a fixed list
// made up for the pictures, the same in every interface language, as Register
// Archive and Devices show them. Each volume is plainly sample
// (SAMPLE-VOLUME-*, SAMPLE-DEVICE-*, SAMPLE-SERIAL-*, SAMPLE-DEVICE-REF-*,
// c0ffee00-... ids), mounted under /media/SAMPLE-*, never at a drive letter,
// with the space in use on it; one has no label, one is read-only, and two
// are on one device. Each device has the health it "reported" (one healthy,
// one warning that it predicts its failure, one that cannot be read), so
// Devices asks no device for its own. The one whose health cannot be read is
// behind a sample enclosure that hides the device's own serial (decision
// 0047), so Devices offers to read it, which the sample device identity
// reader answers. No registered archive matches any of them.
struct SampleVolume {
    VolumeInfo volume;
    std::uint64_t used_bytes = 0;
};
std::vector<SampleVolume> sample_volumes();

// The device identity reader of the screenshot mode (decision 0047): SAMPLE
// DATA, never a read of a device. It is available, and answers every read at
// once, with no question of Windows for administrator permission, with the
// fixed sample identity (model SAMPLE-OWN-MODEL, serial SAMPLE-OWN-SERIAL-01,
// WWN SAMPLE-OWN-WWN-01, read "sample"), for the device named.
inline constexpr const char* sample_own_serial = "SAMPLE-OWN-SERIAL-01";
std::unique_ptr<IDeviceIdentityHelper> sample_device_identity_helper();

// The computer that recorded a run (VerifyRunRecord::host), as a view shows
// it: Sample, SAMPLE-HOST, since the name of the computer that takes the
// published pictures is no more sample data than its volumes are; nothing
// where none was recorded.
std::string displayed_host(const std::string& recorded, IdentityShown shown);

// The above as the app's pages of a run take them (the result page, the
// verification report, decision 0048): Recorded shows what was recorded;
// Sample the sample identity, devices and computer (displayed_identity,
// displayed_devices, displayed_host), also for the computer that makes a
// report.
app::ShownIdentity shown_identity(IdentityShown shown);

} // namespace va::gui
