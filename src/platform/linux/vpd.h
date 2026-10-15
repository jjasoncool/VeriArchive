#pragma once
// Parsing of SCSI Vital Product Data pages read from sysfs (decision 0021).
// Pure C++ so it is tested on every OS, like mountinfo.
#include <string>
#include <string_view>

namespace va::platform::linux_vpd {

// The unit serial number from the bytes of VPD page 0x80
// (/sys/block/<disk>/device/vpd_pg80), without the spaces and NULs that pad it;
// empty when the page is not valid or the serial is not printable ASCII.
std::string unit_serial(std::string_view page);

// True when the SCSI vendor that sysfs shows for a device
// (/sys/block/<disk>/device/vendor, e.g. "ATA     \n") is "ATA": the device is
// a SATA device behind the Linux ATA layer (libata). Only for these is the
// page 0x80 serial the one udev reports (decision 0021).
bool is_ata_vendor(std::string_view vendor);

} // namespace va::platform::linux_vpd
