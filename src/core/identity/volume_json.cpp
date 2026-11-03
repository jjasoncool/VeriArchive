#include "core/identity/volume_json.h"

#include "common/utf8.h"

namespace va {

nlohmann::json volume_to_json(const VolumeInfo& volume)
{
    auto devices = nlohmann::json::array();
    for (const auto& device : volume.devices) {
        nlohmann::json entry{
            {"device_ref", device.device_ref},
            {"model", device.model},
            {"serial", device.serial},
            {"wwn", device.wwn},
            {"capacity_bytes", device.capacity_bytes},
            {"bus", device.bus},
        };
        entry["rotational"] = device.rotational ? nlohmann::json(*device.rotational) : nullptr;
        // Decision 0044: kept where the operation asked for it.
        if (device.health) {
            entry["health"] = health_to_json(*device.health);
        }
        devices.push_back(std::move(entry));
    }
    return {
        {"mount_point", path_to_utf8(volume.mount_point)},
        {"filesystem_type", volume.filesystem_type},
        {"filesystem_uuid", volume.filesystem_uuid},
        {"filesystem_uuid_short", volume.filesystem_uuid_short},
        {"volume_label", volume.volume_label},
        {"volume_guid", volume.volume_guid},
        {"capacity_bytes", volume.capacity_bytes},
        {"read_only", volume.read_only},
        {"container", volume.container},
        {"devices", std::move(devices)},
    };
}

VolumeInfo volume_from_json(const nlohmann::json& json)
{
    VolumeInfo volume;
    volume.mount_point = path_from_utf8(json.value("mount_point", std::string{}));
    volume.filesystem_type = json.value("filesystem_type", std::string{});
    volume.filesystem_uuid = json.value("filesystem_uuid", std::string{});
    volume.filesystem_uuid_short = json.value("filesystem_uuid_short", std::string{});
    volume.volume_label = json.value("volume_label", std::string{});
    volume.volume_guid = json.value("volume_guid", std::string{});
    volume.capacity_bytes = json.value("capacity_bytes", std::uint64_t{0});
    volume.read_only = json.value("read_only", false);
    volume.container = json.value("container", std::string{});
    if (json.contains("devices") && json["devices"].is_array()) {
        for (const auto& entry : json["devices"]) {
            PhysicalDeviceInfo device;
            device.device_ref = entry.value("device_ref", std::string{});
            device.model = entry.value("model", std::string{});
            device.serial = entry.value("serial", std::string{});
            device.wwn = entry.value("wwn", std::string{});
            device.capacity_bytes = entry.value("capacity_bytes", std::uint64_t{0});
            device.bus = entry.value("bus", std::string{});
            if (entry.contains("rotational") && entry["rotational"].is_boolean()) {
                device.rotational = entry["rotational"].get<bool>();
            }
            if (entry.contains("health") && entry["health"].is_object()) {
                device.health = health_from_json(entry["health"]);
            }
            volume.devices.push_back(std::move(device));
        }
    }
    return volume;
}

std::string volume_to_json_text(const VolumeInfo& volume)
{
    return volume_to_json(volume).dump();
}

std::string_view health_status_code(HealthStatus status)
{
    switch (status) {
    case HealthStatus::Healthy:
        return "healthy";
    case HealthStatus::Warning:
        return "warning";
    case HealthStatus::Unhealthy:
        return "unhealthy";
    case HealthStatus::Unknown:
        break;
    }
    return "unknown";
}

nlohmann::json health_to_json(const DeviceHealth& health)
{
    return {{"status", health_status_code(health.status)}, {"conditions", health.conditions}};
}

DeviceHealth health_from_json(const nlohmann::json& json)
{
    DeviceHealth health;
    const auto status = json.value("status", std::string{});
    for (const auto known : {HealthStatus::Healthy, HealthStatus::Warning, HealthStatus::Unhealthy}) {
        if (status == health_status_code(known)) {
            health.status = known;
        }
    }
    if (const auto conditions = json.find("conditions"); conditions != json.end() && conditions->is_array()) {
        for (const auto& condition : *conditions) {
            if (condition.is_string()) {
                health.conditions.push_back(condition.get<std::string>());
            }
        }
    }
    return health;
}

} // namespace va
