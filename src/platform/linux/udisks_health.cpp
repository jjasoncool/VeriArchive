#include "platform/linux/udisks_health.h"

#include <algorithm>

namespace va::platform::linux_health {

DeviceHealth health_from_ata(const AtaSmart& smart)
{
    DeviceHealth health;
    if (!smart.supported || !smart.enabled || smart.updated == 0) {
        return health;
    }
    if (smart.failing) {
        health.status = HealthStatus::Unhealthy;
        health.conditions.emplace_back("predicting-failure");
    }
    if (smart.attributes_failing > 0) {
        health.conditions.emplace_back("attributes-failing");
    }
    if (smart.bad_sectors > 0) {
        health.conditions.emplace_back("bad-sectors");
    }
    if (health.status == HealthStatus::Unknown) {
        health.status = health.conditions.empty() ? HealthStatus::Healthy : HealthStatus::Warning;
    }
    return health;
}

DeviceHealth health_from_nvme(std::uint64_t updated, const std::vector<std::string>& critical_warnings)
{
    DeviceHealth health;
    if (updated == 0) {
        return health;
    }
    bool unhealthy = false;
    const auto add = [&](const char* condition) {
        if (std::ranges::find(health.conditions, condition) == health.conditions.end()) {
            health.conditions.emplace_back(condition);
        }
    };
    for (const auto& warning : critical_warnings) {
        if (warning == "spare") {
            add("spare-low");
        } else if (warning == "temperature") {
            add("temperature");
        } else if (warning == "degraded") {
            add("degraded");
            unhealthy = true;
        } else if (warning == "readonly" || warning == "pmr_readonly") {
            add("read-only");
            unhealthy = true;
        } else if (warning == "volatile_mem") {
            add("volatile-memory-failed");
        } else if (!warning.empty()) {
            add(warning.c_str()); // a later udisks2's, as it names it
        }
    }
    health.status = unhealthy                   ? HealthStatus::Unhealthy
                    : health.conditions.empty() ? HealthStatus::Healthy
                                                : HealthStatus::Warning;
    return health;
}

} // namespace va::platform::linux_health
