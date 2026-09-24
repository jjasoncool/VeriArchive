#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <expected>
#include <nlohmann/json.hpp>
#include "common/error.h"

namespace va {

struct TrustedKey {
    std::string key_id;
    std::string pubkey_hex;
    std::string note;
};

struct Policy {
    int full_verify_max_age_days = 365;
    int any_verify_max_age_days = 90;
    bool strict_metadata = false;
};

struct Config {
    // Preserve extension fields and nested settings when updating known fields.
    nlohmann::json document = nlohmann::json::object();
    std::optional<std::filesystem::path> repository;
    std::string last_repository_uuid;
    std::vector<TrustedKey> trusted_keys;
    Policy policy;
    bool allow_network_repository = false;
};

// Missing file is distinct from malformed or inaccessible configuration.
std::expected<std::optional<Config>, IoError> read_config(const std::filesystem::path& path);
bool write_config(const std::filesystem::path& path, const Config& config);

} // namespace va