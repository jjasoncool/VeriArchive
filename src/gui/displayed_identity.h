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
#include "common/types.h"

#include <cstdint>
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
// this computer's, which IMediaEnumerator::volumes() is then never asked:
// SAMPLE DATA, a fixed list made up for the pictures, the same in every
// interface language. Each volume is plainly sample (SAMPLE-VOLUME-*,
// SAMPLE-DEVICE-*, SAMPLE-SERIAL-*, c0ffee00-... ids), mounted under
// /media/SAMPLE-*, never at a drive letter, with the space in use on it; one
// has no label. No registered archive matches any of them.
struct SampleVolume {
    VolumeInfo volume;
    std::uint64_t used_bytes = 0;
};
std::vector<SampleVolume> sample_volumes();

// The computer that recorded a run (VerifyRunRecord::host), as a view shows
// it: Sample, SAMPLE-HOST, since the name of the computer that takes the
// published pictures is no more sample data than its volumes are; nothing
// where none was recorded.
std::string displayed_host(const std::string& recorded, IdentityShown shown);

} // namespace va::gui
