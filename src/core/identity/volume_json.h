#pragma once
// VolumeInfo <-> JSON, used for `media list --json` and for the raw evidence
// stored in media_evidence.volume_json (R-96). Not canonical: the manifest
// header has its own fixed-order writer.
#include "common/types.h"

#include <nlohmann/json.hpp>

namespace va {

nlohmann::json volume_to_json(const VolumeInfo& volume);
VolumeInfo volume_from_json(const nlohmann::json& json);

} // namespace va
