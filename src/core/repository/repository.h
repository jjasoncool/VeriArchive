#pragma once
#include "app/app_context.h"
#include "common/error.h"
#include "core/interfaces.h"
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

// The config file used (R-82): --config, else <executable_dir>/config.json when
// it exists (portable install), else the user config directory's config.json.
std::filesystem::path config_file_path(const RepositoryOptions& options);

std::expected<AppContext, IoError> resolve_repository(
    const RepositoryOptions& options,
    const RepositoryProbes& probes);
// Crash-safe order: directories, then the database schema in one transaction,
// then repository.json (its presence marks a complete repository), then the
// config pin. Re-running after a crash at any step completes the repository.
std::expected<void, IoError> initialise_repository(AppContext& context, IFileWriter& writer);
std::expected<void, IoError> open_repository(AppContext& context);
// R-83 step 5: when repository.json's uuid differs from the one recorded in the
// config, sets context.previous_repository_uuid (the caller warns) and records
// the new uuid in the existing config file.
std::expected<void, IoError> record_repository_uuid(AppContext& context, IFileWriter& writer);
std::expected<void, IoError> validate_schema(const RepositoryLayout& layout);

} // namespace va