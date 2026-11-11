#pragma once
// Device alerts (decision 0044): signs that a device may be failing, told
// beside a run result and never changing it. This part: the health that a
// device reports of itself, asked when an operation that reads its volume
// starts (a registration, a verification, a relocation), kept with the
// operation's media evidence and, for a run in the history, in its counts.
#include "common/types.h"
#include "core/interfaces.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string_view>
#include <vector>

namespace va {

// Asks every device of `volume` for its health, keeping what each reports
// in its `health`; a device that cannot be asked is kept Unknown.
void observe_device_health(IMediaEnumerator& media, VolumeInfo& volume);

// Whether a device's health is a device alert: it reports itself Warning or
// Unhealthy, or a condition that tells of a failure, which every condition
// does but "in-service" (a repair or a rebuild under way) and "missing".
// Healthy and Unknown with no such condition are none.
bool health_alerts(const DeviceHealth& health);

// Whether any of `devices` was asked and its health alerts.
bool any_health_alerts(const std::vector<PhysicalDeviceInfo>& devices);

// Told once, on the operation's thread, of the devices of the volume that an
// operation reads and of the health each reported, as soon as they were
// asked, before anything of the archive is read: the window shows a device
// alert at once, the command line prints it.
using DevicesObserved = std::function<void(const std::vector<PhysicalDeviceInfo>& devices)>;

// The devices that were asked, as a run's counts keep them ("device_health"):
// [{"model": ..., "serial": ..., "wwn": ..., "health": {"status": ...,
// "conditions": [...]}}]. Recorded with the run in the hash chain (R-90).
nlohmann::json device_health_json(const std::vector<PhysicalDeviceInfo>& devices);

// The devices that a run's counts (`counts_json`) record with their health;
// none for a run recorded before decision 0044, or that read no media.
std::vector<PhysicalDeviceInfo> recorded_device_health(std::string_view counts_json);

// The device alerts that a recorded run gives, from its counts: a device
// whose health alerts, and a read markedly slower than the one it was
// compared with (read_speed.h).
struct RunAlerts {
    bool health = false;
    bool slower = false;
    // The latest SMART read of a device of its carrier alerts (decision
    // 0052): only of an archive (app::archive_alerts), never of a run.
    bool smart = false;
    bool any() const { return health || slower || smart; }
};
RunAlerts run_alerts(std::string_view counts_json);

// As --json gives them: ["health", "slower"], each that is there.
nlohmann::json run_alerts_json(const RunAlerts& alerts);

} // namespace va
