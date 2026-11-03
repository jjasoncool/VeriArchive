#include "app/device_alerts.h"

#include "core/engine/device_alerts.h"

#include <array>
#include <utility>

namespace va::app {

std::string health_condition_words(std::string_view condition)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 13> words{{
        {"predicting-failure", "predicting failure"},
        {"hardware-error", "hardware error"},
        {"not-usable", "not usable"},
        {"transient-error", "transient error"},
        {"in-service", "being repaired or rebuilt"},
        {"missing", "missing"},
        {"bad-sectors", "bad sectors"},
        {"attributes-failing", "SMART attributes past their threshold"},
        {"spare-low", "spare capacity low"},
        {"temperature", "temperature too high"},
        {"degraded", "reliability degraded"},
        {"read-only", "turned read-only"},
        {"volatile-memory-failed", "volatile memory backup failed"},
    }};
    for (const auto& [code, plain] : words) {
        if (condition == code) {
            return std::string(plain);
        }
    }
    return std::string(condition);
}

std::string health_words(const Text& text, const DeviceHealth& health)
{
    std::string words;
    switch (health.status) {
    case HealthStatus::Healthy:
        words = text("healthy");
        break;
    case HealthStatus::Warning:
        words = text("warning");
        break;
    case HealthStatus::Unhealthy:
        words = text("unhealthy");
        break;
    case HealthStatus::Unknown:
        words = text("cannot be read");
        break;
    }
    if (health.conditions.empty()) {
        return words;
    }
    const std::string_view separator = text.language() == Language::TraditionalChinese ? "、" : ", ";
    std::string conditions;
    for (const auto& condition : health.conditions) {
        conditions += (conditions.empty() ? "" : std::string(separator)) + text(health_condition_words(condition));
    }
    return words + (text.language() == Language::TraditionalChinese ? "" : " ") + text.parenthesized(conditions);
}

std::string device_name(const Text& text, const PhysicalDeviceInfo& device)
{
    const auto model = device.model.empty() ? std::string("?") : device.model;
    if (!device.serial.empty()) {
        return text.format("{} (serial {})", model, device.serial);
    }
    if (!device.wwn.empty()) {
        return text.format("{} (WWN {})", model, device.wwn);
    }
    return model;
}

std::string device_health_words(const Text& text, const PhysicalDeviceInfo& device)
{
    return text.format("{}: {}", device_name(text, device), health_words(text, device.health.value_or(DeviceHealth{})));
}

std::vector<std::string> health_alert_lines(const Text& text, const std::vector<PhysicalDeviceInfo>& devices)
{
    std::vector<std::string> lines;
    for (const auto& device : devices) {
        if (device.health && health_alerts(*device.health)) {
            lines.push_back(std::string(alert_mark) +
                            text.format("Device alert: {} reports its health as {}. Copy the archives on it to "
                                        "another device soon, and verify the copies.",
                                device_name(text, device), health_words(text, *device.health)));
        }
    }
    return lines;
}

} // namespace va::app
