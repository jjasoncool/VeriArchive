#pragma once
#include "common/types.h"

#include <string>

namespace va::platform::linux_health {

// What udisks2 says of the drive that holds `device_ref` ("/dev/sdb",
// "/dev/nvme0n1"), as IMediaEnumerator::device_health gives it (decision
// 0044): Unknown without a system bus or udisks2, for a device that udisks2
// knows no drive of (a loop or virtual device), and for a drive whose SMART
// it cannot read (many USB enclosures).
DeviceHealth udisks_device_health(const std::string& device_ref);

} // namespace va::platform::linux_health
