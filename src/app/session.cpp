#include "app/session.h"

#include "app/local_time.h"
#include "common/utf8.h"
#include "core/repository/repository.h"
#include "core/write_file.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <chrono>
#include <cstdlib>
#include <string_view>

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

namespace {

// The repository that a session with these options opens (HANDOFF §B7.4),
// resolved from the program's folder, the user config folder and home, the
// options and VERIARCHIVE_REPOSITORY; or why it cannot be resolved.
std::expected<AppContext, std::string> resolved_repository(const SessionOptions& session_options)
{
    const auto executable = platform::executable_path();
    const auto config_directory = platform::user_config_dir();
    const auto home = platform::user_home();
    if (!executable || !config_directory || !home) {
        const auto& error = !executable ? executable.error()
                            : !config_directory ? config_directory.error()
                                                : home.error();
        return std::unexpected(error.message);
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
        return std::unexpected(context.error().message);
    }
    return std::move(*context);
}

} // namespace

std::expected<Session, SessionFailure> open_session(const SessionOptions& session_options)
{
    // R-12 first, also where no repository can be opened: running elevated
    // may be the reason, as the home folder and the config file then resolve
    // otherwise (decision 0037).
    std::vector<std::string> warnings;
    if (session_options.elevated ? session_options.elevated() : platform::process_is_elevated()) {
        warnings.emplace_back(running_elevated);
    }
    const auto failed = [&](std::string message, std::vector<std::string> notes = {},
                            std::optional<RepositoryLayout> repository = std::nullopt) {
        return std::unexpected(SessionFailure{std::move(message), warnings, std::move(notes), std::move(repository)});
    };

    auto context = resolved_repository(session_options);
    if (!context) {
        // No RepositoryRoot resolved: no folder in which to name copies.
        return failed(std::move(context.error()));
    }

    Session session{std::move(*context), platform::make_file_writer(), warnings, {}, std::nullopt};
    // The copy of a database kept before its upgrade is named for the date
    // that the clock here shows, as people read it (decision 0017).
    const auto today = local_date(
        session_options.now.value_or(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())));
    std::expected<void, IoError> opened;
    if (session_options.rebuild) {
        auto made = open_repository_to_rebuild(session.context, today);
        if (made) {
            session.replaced_database = std::move(*made);
        } else {
            opened = std::unexpected(made.error());
        }
    } else {
        opened = session_options.initialise ? initialise_repository(session.context, *session.writer, today)
                                            : open_repository(session.context, today);
    }
    // Said once, also when the open fails after the upgrade, as on a config
    // file it cannot write: the database stays upgraded, and no later open
    // says where its copy is (decision 0032).
    if (session.context.schema_upgrade) {
        session.notes.push_back(schema_upgrade_note(*session.context.schema_upgrade));
    }
    // With the RepositoryRoot resolved, where the copies of its database kept
    // before upgrades are named also when it cannot be opened (decision 0032).
    if (!opened) {
        return failed(opened.error().message, session.notes, session.context.repo);
    }
    if (auto recorded = record_repository_uuid(session.context, *session.writer); !recorded) {
        return failed(recorded.error().message, session.notes, session.context.repo);
    }
    if (!session.context.previous_repository_uuid.empty()) {
        session.warnings.push_back("repository_uuid changed from " +
                                   session.context.previous_repository_uuid + " to " +
                                   session.context.repository_uuid + "; recorded the new uuid");
    }
    return session;
}

std::optional<int> schema_upgrade_ahead(const SessionOptions& options)
{
    const auto context = resolved_repository(options);
    if (!context) {
        return std::nullopt;
    }
    return schema_version_to_upgrade(context->repo);
}

std::string schema_upgrade_note(const SchemaUpgrade& upgrade)
{
    return "the repository database was upgraded from schema version " + std::to_string(upgrade.from) + " to " +
           std::to_string(upgrade.to) +
           ", which earlier versions of VeriArchive cannot open; a copy of it from before the upgrade is kept at " +
           path_to_utf8(upgrade.copy);
}

std::vector<std::string> copy_before_upgrade_lines(const Text& text, const RepositoryLayout& layout)
{
    std::vector<std::string> lines;
    for (const auto& copy : copies_before_upgrade(layout)) {
        lines.push_back(text.format("Copy of the database from before an upgrade: {} (an earlier version of "
                                    "VeriArchive can open only this copy; you can delete it once you no longer "
                                    "need that version)",
            path_to_utf8(copy)));
    }
    return lines;
}

std::string session_failure_in_window(const Text& text, const SessionFailure& failure)
{
    auto shown = text(failure.message);
    for (const auto& warning : failure.warnings) {
        shown += "\n\n" + text.format("Warning: {}", text(warning));
    }
    for (const auto& note : failure.notes) {
        shown += "\n\n" + text.format("Note: {}", text(note));
    }
    if (failure.repository) {
        // One paragraph, a line per copy.
        std::string_view separator = "\n\n";
        for (const auto& line : copy_before_upgrade_lines(text, *failure.repository)) {
            shown += separator;
            shown += line;
            separator = "\n";
        }
    }
    return shown;
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
