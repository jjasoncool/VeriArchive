#pragma once
#include <filesystem>
#include <map>
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
    int any_verify_max_age_days = 180;
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
    std::string language; // "" = follow the system, "en" or "zh-TW" (decision 0017)
    // R-111, §B7.3 `io.rotational_override`: by device serial, whether the
    // device is rotational (an HDD: true) or not (an SSD: false), for a USB
    // bridge that reports it wrongly or not at all. Written in the file as
    // "hdd" or "ssd" (or true or false). Read only: config_to_json keeps the
    // file's own "io" as it is.
    std::map<std::string, bool> rotational_override;
};

// Missing file is distinct from malformed or inaccessible configuration.
std::expected<std::optional<Config>, IoError> read_config(const std::filesystem::path& path);
// Serialises known fields over the preserved document. The caller writes the
// result durably (core/write_file.h); common never touches the disk for writes.
std::string config_to_json(const Config& config);

} // namespace va