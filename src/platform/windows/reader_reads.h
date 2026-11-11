#pragma once
// What the window takes of one run of the device identity reader (decisions
// 0047 and 0052): for each device it was given, its own identity, decoded
// here, unprivileged, from the identification the device returned, and the
// SMART read the repository keeps, with why nothing was read where nothing
// was. Plain C++ without the Windows headers, so that it is tested on every
// system.
#include "core/interfaces.h"
#include "platform/windows/device_identity_reader.h"

#include <vector>

namespace va::platform::windows {

// The reads of `devices` (reader_devices, in the order the reader was given
// them), each device as the system described it before the operator was
// asked (`seen`, the same order, each with its device_ref), at `read_at`. A
// device whose identification does not decode is a read that failed (Other).
// Where the identification was read and SMART was not, the read's problem
// says why: "smart-unsupported" or "smart-off" where the identification says
// so, "no-answer" where SMART was not answered in time, "smart-not-passed"
// otherwise; where nothing was read, "not-passed", "no-answer" or
// "unreadable", by the error's kind (reader_error). A device no longer
// attached gets no SMART read.
std::vector<DeviceRead> device_reads(const std::vector<ReaderDevice>& devices,
    const std::vector<PhysicalDeviceInfo>& seen, const Timestamp& read_at);

} // namespace va::platform::windows
