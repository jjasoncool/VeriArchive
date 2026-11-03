#pragma once
// The health that udisks2 gives of a drive (decision 0044), from the
// properties of its org.freedesktop.UDisks2.Drive.Ata interface (SMART of an
// ATA drive) or its org.freedesktop.UDisks2.NVMe.Controller interface (the
// SMART log of an NVMe controller), which udisks2's daemon reads as root and
// shows to every program. Plain C++, so that it is tested on every system.
#include "common/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace va::platform::linux_health {

// The properties of Drive.Ata that tell its health.
struct AtaSmart {
    bool supported = false;              // SmartSupported
    bool enabled = false;                // SmartEnabled
    std::uint64_t updated = 0;           // SmartUpdated: when the data was read, 0 for never
    bool failing = false;                // SmartFailing: the drive predicts its failure
    std::int32_t attributes_failing = 0; // SmartNumAttributesFailing, -1 where unknown
    std::int64_t bad_sectors = 0;        // SmartNumBadSectors (pending and reallocated), -1 where unknown
};

// Unknown where SMART is not supported, not enabled or not read yet;
// Unhealthy, predicting failure, where the drive says it is failing; Warning
// with "attributes-failing" or "bad-sectors" where attributes are past their
// threshold now or sectors were found bad; Healthy otherwise.
DeviceHealth health_from_ata(const AtaSmart& smart);

// SmartUpdated and SmartCriticalWarning of NVMe.Controller: Unknown where the
// log was not read yet; Healthy with no critical warning; otherwise Warning,
// Unhealthy where the controller says its reliability is degraded or its
// media read only, with a condition for each warning ("spare" as
// "spare-low", "temperature", "degraded", "readonly" and "pmr_readonly" as
// "read-only", "volatile_mem" as "volatile-memory-failed"; one this does not
// know as udisks2 names it).
DeviceHealth health_from_nvme(std::uint64_t updated, const std::vector<std::string>& critical_warnings);

} // namespace va::platform::linux_health
