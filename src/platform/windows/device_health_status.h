#pragma once
// The health that Windows' storage stack gives of a disk (decision 0044), from
// the answer to IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceManagementStatus,
// which a handle opened without access rights may ask (R-09): the structure
// STORAGE_DEVICE_MANAGEMENT_STATUS of the Windows SDK, which MinGW's headers
// do not define. Plain C++, so that it is tested on every system.
#include "common/types.h"

#include <cstddef>
#include <span>

namespace va::platform::windows {

// The answer as the IOCTL wrote it, `returned` bytes. Its health (Health:
// unknown, unhealthy, warning, healthy) is the status; each operational
// status other than none, unknown and OK is a condition (predicting failure,
// in service, hardware error, not usable, transient error, missing). An
// answer too short to hold the health, or of a version older than the
// structure this reads, is Unknown.
DeviceHealth health_from_management_status(std::span<const std::byte> answer);

} // namespace va::platform::windows
