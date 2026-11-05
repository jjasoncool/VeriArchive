#pragma once
// Whether a disk's enclosure hides the device's own identity (decision 0047),
// from the device identification page (SCSI VPD page 0x83) as Windows gives
// it, without elevation, to a handle opened without access rights (R-09):
// IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceIdProperty, whose answer is
// the structure STORAGE_DEVICE_ID_DESCRIPTOR of winioctl.h. Plain C++, so
// that it is tested on every system.
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace va::platform::windows {

// Whether the answer (`returned` bytes of STORAGE_DEVICE_ID_DESCRIPTOR: DWORD
// Version, DWORD Size, DWORD NumberOfIdentifiers, then the identifiers, each
// a STORAGE_IDENTIFIER: CodeSet, Type and Association as 4-byte values,
// IdentifierSize and NextOffset as 2-byte ones, then its bytes, all in the
// computer's byte order) names the device itself (association 0, the logical
// unit) by an identifier unique in the world: an NAA identifier (type 3)
// whose NAA is 2, 5 or 6 (IEEE extended, registered, registered extended),
// or an EUI-64 (type 2), not all zeros. A locally assigned NAA (NAA 3), which
// a USB bridge makes up for the disk it presents, names nothing beyond it, nor
// does a vendor's identifier (type 1, the bridge's vendor, product and
// serial). Identifiers that do not fit in the answer are not read.
bool names_device_worldwide(std::span<const std::byte> answer);

// Whether a disk on `bus` (as the media enumerator names it) is behind an
// enclosure that hides the device's own identity: on bus "usb", for a disk
// whose medium is not removable (`removable_media`, as the system reports
// it), where its identification page (`answer`; nothing where the system
// gives none) names no device worldwide (above), so that the serial the
// system reports is the enclosure's. Never on any other bus, and never for
// removable media: a USB flash memory or card reader has no enclosure between
// it and the computer, its USB serial is its own, and no ATA device is behind
// it (udev, likewise, asks only a USB disk that is not removable for its ATA
// identity). False also where the page names the device worldwide although
// the enclosure still reports its own serial: the system's serial may then be
// the enclosure's while this says false.
bool enclosure_hides_identity(
    std::string_view bus, bool removable_media, std::optional<std::span<const std::byte>> answer);

} // namespace va::platform::windows
