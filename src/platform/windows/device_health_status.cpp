#include "platform/windows/device_health_status.h"

#include <cstdint>
#include <cstring>
#include <optional>

namespace va::platform::windows {
namespace {

// STORAGE_DEVICE_MANAGEMENT_STATUS (winioctl.h): DWORD Version, DWORD Size,
// STORAGE_DISK_HEALTH_STATUS Health, DWORD NumberOfOperationalStatus, DWORD
// NumberOfAdditionalReasons, STORAGE_DISK_OPERATIONAL_STATUS
// OperationalStatus[16], then the additional reasons, which are not read.
// Every field is 4 bytes, in the computer's byte order (little-endian).
constexpr std::size_t version_offset = 0;
constexpr std::size_t health_offset = 8;
constexpr std::size_t count_offset = 12;
constexpr std::size_t statuses_offset = 20;
constexpr std::uint32_t most_statuses = 16;
// The version is the structure's size, 100 bytes with its one additional
// reason; a later one would add fields after these.
constexpr std::uint32_t known_version = 100;

std::optional<std::uint32_t> field(std::span<const std::byte> answer, std::size_t offset)
{
    if (answer.size() < offset + 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, answer.data() + offset, 4);
    return value;
}

// STORAGE_DISK_OPERATIONAL_STATUS: DiskOpStatusNone 0, Unknown 1, Ok 2,
// PredictingFailure 3, InService 4, HardwareError 5, NotUsable 6,
// TransientError 7, Missing 8.
const char* condition_of(std::uint32_t operational_status)
{
    switch (operational_status) {
    case 3:
        return "predicting-failure";
    case 4:
        return "in-service";
    case 5:
        return "hardware-error";
    case 6:
        return "not-usable";
    case 7:
        return "transient-error";
    case 8:
        return "missing";
    default:
        return nullptr;
    }
}

} // namespace

DeviceHealth health_from_management_status(std::span<const std::byte> answer)
{
    DeviceHealth health;
    const auto version = field(answer, version_offset);
    const auto status = field(answer, health_offset);
    if (!version || *version < known_version || !status) {
        return health;
    }
    // STORAGE_DISK_HEALTH_STATUS: DiskHealthUnknown 0, Unhealthy 1, Warning
    // 2, Healthy 3.
    switch (*status) {
    case 1:
        health.status = HealthStatus::Unhealthy;
        break;
    case 2:
        health.status = HealthStatus::Warning;
        break;
    case 3:
        health.status = HealthStatus::Healthy;
        break;
    default:
        break;
    }
    const auto count = field(answer, count_offset).value_or(0);
    for (std::uint32_t index = 0; index < count && index < most_statuses; ++index) {
        const auto operational = field(answer, statuses_offset + 4 * index);
        if (!operational) {
            break;
        }
        if (const auto* condition = condition_of(*operational)) {
            health.conditions.emplace_back(condition);
        }
    }
    return health;
}

} // namespace va::platform::windows
