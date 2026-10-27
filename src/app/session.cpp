#include "app/session.h"

#include "common/utf8.h"
#include "core/repository/repository.h"
#include "core/write_file.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <cstdlib>

namespace va::app {
namespace {

constexpr const char* running_elevated = "VeriArchive is running elevated; repository and home resolution may differ";

} // namespace

std::optional<std::string> elevated_warning()
{
    if (platform::process_is_elevated()) {
        return std::string(running_elevated);
    }
    return std::nullopt;
}

std::expected<Session, SessionFailure> open_session(const SessionOptions& session_options)
{
    // R-12 first, also where no repository can be opened: running elevated
    // may be the reason, as the home folder and the config file then resolve
    // otherwise (decision 0037).
    std::vector<std::string> warnings;
    if (session_options.elevated ? session_options.elevated() : platform::process_is_elevated()) {
        warnings.emplace_back(running_elevated);
    }
    const auto failed = [&](std::string message) {
        return std::unexpected(SessionFailure{std::move(message), warnings});
    };

    const auto executable = platform::executable_path();
    const auto config_directory = platform::user_config_dir();
    const auto home = platform::user_home();
    if (!executable || !config_directory || !home) {
        const auto& error = !executable ? executable.error()
                            : !config_directory ? config_directory.error()
                                                : home.error();
        return failed(error.message);
    }

    RepositoryOptions options;
    options.initialise = session_options.initialise;
    options.executable_dir = executable->parent_path();
    options.user_config_dir = *config_directory;
    options.user_home = *home;
    options.explicit_repository = session_options.repository;
    options.explicit_config = session_options.config;
    if (const char* environment_repository = std::getenv("VERIARCHIVE_REPOSITORY")) {
        options.environment_repository = path_from_utf8(environment_repository);
    }

    const RepositoryProbes probes{platform::writable_directory, platform::network_filesystem};
    auto context = resolve_repository(options, probes);
    if (!context) {
        return failed(context.error().message);
    }

    Session session{std::move(*context), platform::make_file_writer(), warnings, std::nullopt};
    if (session_options.rebuild) {
        auto made = open_repository_to_rebuild(session.context);
        if (!made) {
            return failed(made.error().message);
        }
        session.replaced_database = std::move(*made);
    } else {
        auto opened = session_options.initialise ? initialise_repository(session.context, *session.writer)
                                                 : open_repository(session.context);
        if (!opened) {
            return failed(opened.error().message);
        }
    }
    if (auto recorded = record_repository_uuid(session.context, *session.writer); !recorded) {
        return failed(recorded.error().message);
    }
    if (!session.context.previous_repository_uuid.empty()) {
        session.warnings.push_back("repository_uuid changed from " +
                                   session.context.previous_repository_uuid + " to " +
                                   session.context.repository_uuid + "; recorded the new uuid");
    }
    return session;
}

HostInfo host_info(std::string app_version)
{
    return {platform::os_name(), platform::host_name(), std::move(app_version)};
}

namespace {

// The config file that open_session() would read, resolved the same way:
// a relative path is taken from the executable's folder, never from the current
// directory.
std::optional<std::filesystem::path> config_path(std::optional<std::filesystem::path> explicit_config)
{
    if (explicit_config && !explicit_config->is_relative()) {
        return explicit_config; // needs neither the executable nor the user config directory
    }
    const auto executable = platform::executable_path();
    if (!executable) {
        return std::nullopt;
    }
    RepositoryOptions options;
    options.explicit_config = std::move(explicit_config);
    options.executable_dir = executable->parent_path();
    if (!options.explicit_config) {
        // Only the default location needs the user config directory, so an
        // explicit --config still works when APPDATA / HOME are unset.
        const auto config_directory = platform::user_config_dir();
        if (!config_directory) {
            return std::nullopt;
        }
        options.user_config_dir = *config_directory;
    }
    auto path = config_file_path(options);
    if (path.is_relative()) {
        path = options.executable_dir / path;
    }
    return path;
}

} // namespace

Language interface_language(std::optional<std::string> explicit_code,
    std::optional<std::filesystem::path> explicit_config)
{
    if (!explicit_code) {
        if (const char* environment = std::getenv("VERIARCHIVE_LANG")) {
            explicit_code = environment;
        }
    }
    std::optional<std::string> configured;
    if (const auto path = config_path(std::move(explicit_config))) {
        if (const auto config = read_config(*path); config && *config) {
            configured = (*config)->language;
        }
    }
    return resolve_language(std::move(explicit_code), std::move(configured), platform::ui_language());
}

std::expected<void, std::string> save_language(Language language,
    std::optional<std::filesystem::path> explicit_config)
{
    const auto path = config_path(std::move(explicit_config));
    if (!path) {
        return std::unexpected(std::string("cannot locate the config file"));
    }
    Config config;
    if (const auto existing = read_config(*path); existing && *existing) {
        config = **existing;
    } else if (!existing) {
        return std::unexpected(existing.error().message);
    }
    config.language = std::string(language_code(language));
    std::error_code error;
    std::filesystem::create_directories(path->parent_path(), error);
    const auto writer = platform::make_file_writer();
    if (auto written = write_file_atomically(*writer, *path, config_to_json(config)); !written) {
        return std::unexpected(written.error().message);
    }
    return {};
}

} // namespace va::app
