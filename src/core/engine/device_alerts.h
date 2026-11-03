#pragma once
// Device alerts (decision 0044): signs that a device may be failing, told
// beside a run result and never changing it. This part: the health that a
// device reports of itself, asked when an operation that reads its volume
// starts.
#include "common/types.h"
#include "core/interfaces.h"

namespace va {

// Asks every device of `volume` for its health, keeping what each reports
// in its `health`; a device that cannot be asked is kept Unknown.
void observe_device_health(IMediaEnumerator& media, VolumeInfo& volume);

// Whether a device's health is a device alert: it reports itself Warning or
// Unhealthy, or a condition that tells of a failure, which every condition
// does but "in-service" (a repair or a rebuild under way) and "missing".
// Healthy and Unknown with no such condition are none.
bool health_alerts(const DeviceHealth& health);

} // namespace va
