#pragma once
// VolumeInfo <-> JSON, used for `media list --json` and for the raw evidence
// stored in media_evidence.volume_json (R-96). Not canonical: the manifest
// header has its own fixed-order writer.
#include "common/types.h"

#include <nlohmann/json.hpp>

#include <string_view>

namespace va {

nlohmann::json volume_to_json(const VolumeInfo& volume);
VolumeInfo volume_from_json(const nlohmann::json& json);
std::string volume_to_json_text(const VolumeInfo& volume);

// A device's health (decision 0044) as its media evidence keeps it, and as
// `media list --json` and a run's counts give it: {"status": "healthy",
// "conditions": [...]}, its status one of "unknown", "healthy", "warning",
// "unhealthy". Read back, an unknown status code is Unknown.
std::string_view health_status_code(HealthStatus status);
nlohmann::json health_to_json(const DeviceHealth& health);
DeviceHealth health_from_json(const nlohmann::json& json);

} // namespace va
