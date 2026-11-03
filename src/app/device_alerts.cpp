#include "app/device_alerts.h"

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

} // namespace va::app
