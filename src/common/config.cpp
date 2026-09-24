#include "common/config.h"
#include "common/utf8.h"

#include <fstream>

namespace va {

std::expected<std::optional<Config>, IoError> read_config(const std::filesystem::path& path)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(IoError{IoErrorKind::Other, error.value(), error.message()});
    }
    if (!exists) {
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(IoError{IoErrorKind::PermissionDenied, 0, "Cannot read config"});
    }

    try {
        Config config;
        input >> config.document;
        const auto& document = config.document;
        if (!document.is_object()) {
            return std::unexpected(IoError{IoErrorKind::Other, 0, "Config must be an object"});
        }
        if (document.contains("repository")) {
            config.repository = path_from_utf8(document.at("repository").get<std::string>());
            if (config.repository->empty()) {
                return std::unexpected(IoError{IoErrorKind::Other, 0, "Repository path is empty"});
            }
        }
        config.last_repository_uuid = document.value("last_repository_uuid", std::string{});
        config.allow_network_repository = document.value("allow_network_repository", false);
        if (document.contains("policy")) {
            const auto& policy = document.at("policy");
            if (!policy.is_object()) {
                return std::unexpected(IoError{IoErrorKind::Other, 0, "Policy must be an object"});
            }
            config.policy.full_verify_max_age_days = policy.value("full_verify_max_age_days", 365);
            config.policy.any_verify_max_age_days = policy.value("any_verify_max_age_days", 90);
            config.policy.strict_metadata = policy.value("strict_metadata", false);
        }
        if (document.contains("trusted_keys")) {
            if (!document.at("trusted_keys").is_array()) {
                return std::unexpected(IoError{IoErrorKind::Other, 0, "Trusted keys must be an array"});
            }
            for (const auto& key : document.at("trusted_keys")) {
                config.trusted_keys.push_back({key.at("key_id").get<std::string>(),
                    key.at("pubkey_hex").get<std::string>(), key.value("note", std::string{})});
            }
        }
        return config;
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(IoError{IoErrorKind::Other, 0,
            std::string("Invalid config: ") + error.what()});
    }
}

bool write_config(const std::filesystem::path& path, const Config& config)
{
    auto document = config.document;
    if (config.repository) {
        document["repository"] = path_to_utf8(*config.repository);
    } else {
        document.erase("repository");
    }
    document["last_repository_uuid"] = config.last_repository_uuid;
    document["allow_network_repository"] = config.allow_network_repository;
    auto& policy = document["policy"];
    policy["full_verify_max_age_days"] = config.policy.full_verify_max_age_days;
    policy["any_verify_max_age_days"] = config.policy.any_verify_max_age_days;
    policy["strict_metadata"] = config.policy.strict_metadata;
    auto keys = nlohmann::json::array();
    for (const auto& key : config.trusted_keys) {
        keys.push_back({{"key_id", key.key_id}, {"pubkey_hex", key.pubkey_hex}, {"note", key.note}});
    }
    document["trusted_keys"] = std::move(keys);

    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << document.dump(2) << '\n';
    output.close();
    return !output.fail();
}

} // namespace va