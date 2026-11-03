#include "core/engine/device_alerts.h"

#include <algorithm>

namespace va {

void observe_device_health(IMediaEnumerator& media, VolumeInfo& volume)
{
    for (auto& device : volume.devices) {
        const auto health = media.device_health(device);
        device.health = health ? *health : DeviceHealth{};
    }
}

bool health_alerts(const DeviceHealth& health)
{
    if (health.status == HealthStatus::Warning || health.status == HealthStatus::Unhealthy) {
        return true;
    }
    return std::ranges::any_of(health.conditions,
        [](const std::string& condition) { return condition != "in-service" && condition != "missing"; });
}

} // namespace va
