#pragma once
#include "app/app_context.h"
#include "common/error.h"
#include <expected>
#include <functional>
#include <optional>

namespace va {

struct RepositoryOptions {
    bool initialise = false;
    std::optional<std::filesystem::path> explicit_repository;
    std::optional<std::filesystem::path> explicit_config;
    std::optional<std::filesystem::path> environment_repository;
    std::filesystem::path executable_dir;
    std::filesystem::path user_config_dir;
    std::filesystem::path user_home;
};

struct RepositoryProbes {
    std::function<bool(const std::filesystem::path&)> writable;
    std::function<bool(const std::filesystem::path&)> network_filesystem;
};

std::expected<AppContext, IoError> resolve_repository(
    const RepositoryOptions& options,
    const RepositoryProbes& probes);
std::expected<void, IoError> initialise_repository(AppContext& context);
std::expected<void, IoError> open_repository(AppContext& context);
std::expected<void, IoError> validate_schema(const RepositoryLayout& layout);

} // namespace va