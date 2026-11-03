#include "core/engine/device_alerts.h"

#include "core/engine/read_speed.h"
#include "core/identity/volume_json.h"

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

bool any_health_alerts(const std::vector<PhysicalDeviceInfo>& devices)
{
    return std::ranges::any_of(
        devices, [](const PhysicalDeviceInfo& device) { return device.health && health_alerts(*device.health); });
}

nlohmann::json device_health_json(const std::vector<PhysicalDeviceInfo>& devices)
{
    auto list = nlohmann::json::array();
    for (const auto& device : devices) {
        if (device.health) {
            list.push_back({{"model", device.model}, {"serial", device.serial}, {"wwn", device.wwn},
                {"health", health_to_json(*device.health)}});
        }
    }
    return list;
}

std::vector<PhysicalDeviceInfo> recorded_device_health(std::string_view counts_json)
{
    std::vector<PhysicalDeviceInfo> devices;
    const auto counts = nlohmann::json::parse(counts_json, nullptr, false);
    if (!counts.is_object()) {
        return devices;
    }
    const auto listed = counts.find("device_health");
    if (listed == counts.end() || !listed->is_array()) {
        return devices;
    }
    // Read as written, and nothing that is not: the counts come from the
    // database, which anyone may have changed (§A5).
    const auto text = [](const nlohmann::json& entry, const char* key) {
        const auto found = entry.find(key);
        return found != entry.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    for (const auto& entry : *listed) {
        if (!entry.is_object()) {
            continue;
        }
        PhysicalDeviceInfo device;
        device.model = text(entry, "model");
        device.serial = text(entry, "serial");
        device.wwn = text(entry, "wwn");
        const auto health = entry.find("health");
        device.health = health != entry.end() && health->is_object() ? health_from_json(*health) : DeviceHealth{};
        devices.push_back(std::move(device));
    }
    return devices;
}

RunAlerts run_alerts(std::string_view counts_json)
{
    const auto compared = recorded_speed_comparison(counts_json);
    return {any_health_alerts(recorded_device_health(counts_json)), compared && compared->alerts()};
}

nlohmann::json run_alerts_json(const RunAlerts& alerts)
{
    auto codes = nlohmann::json::array();
    if (alerts.health) {
        codes.push_back("health");
    }
    if (alerts.slower) {
        codes.push_back("slower");
    }
    return codes;
}

} // namespace va
