#pragma once
// The devices attached now, the volumes on each, and the health that each
// device reports of itself (decision 0044), asked once for a device that
// holds several volumes: what `media list` prints and what the window's
// Devices page shows (decision 0046), so that neither asks it its own way.
// Nothing of an archive is read. The health is asked here and when an
// operation that reads a volume starts, never by the enumeration itself
// (IMediaEnumerator::device_health); on Linux each ask is a udisks call that
// may take seconds, so the window asks it off its thread.
#include "common/error.h"
#include "common/types.h"
#include "core/interfaces.h"

#include <cstddef>
#include <expected>
#include <vector>

namespace va::app {

// A device behind one or more of the attached volumes.
struct AttachedDevice {
    PhysicalDeviceInfo device;         // as the first volume on it names it, with its health where it was asked
    std::vector<std::size_t> volumes;  // the volumes on it: their indexes in the volumes given, in that order
};

struct AttachedDevices {
    // Every volume as IMediaEnumerator::volumes() lists it, each of its
    // devices with the health it reported.
    std::vector<VolumeInfo> volumes;
    // Each device once (devices_of).
    std::vector<AttachedDevice> devices;
};

// The devices of `volumes`, in the order the volumes first name them: a
// device known by its system name (device_ref) once, whatever number of
// volumes it holds; a device without one as each volume names it, since
// nothing tells that two of them are one. Pure, no I/O.
std::vector<AttachedDevice> devices_of(const std::vector<VolumeInfo>& volumes);

// Lists the attached volumes, then asks each device for its health, once
// for a device known by its system name; a device that cannot be asked is
// Unknown ("cannot be read"). Fails only where the volumes cannot be listed.
std::expected<AttachedDevices, IoError> attached_devices(IMediaEnumerator& media);

} // namespace va::app
