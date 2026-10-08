#include "app/session.h"

#include "common/utf8.h"
#include "core/repository/repository.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <cstdlib>

namespace va::app {

std::expected<Session, std::string> open_session(const SessionOptions& session_options)
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

    Session session{std::move(*context), platform::make_file_writer(), {}};
    auto opened = session_options.initialise ? initialise_repository(session.context, *session.writer)
                                             : open_repository(session.context);
    if (!opened) {
        return std::unexpected(opened.error().message);
    }
    if (auto recorded = record_repository_uuid(session.context, *session.writer); !recorded) {
        return std::unexpected(recorded.error().message);
    }
    if (!session.context.previous_repository_uuid.empty()) {
        session.warnings.push_back("repository_uuid changed from " +
                                   session.context.previous_repository_uuid + " to " +
                                   session.context.repository_uuid + "; recorded the new uuid");
    }
    if (platform::process_is_elevated()) {
        session.warnings.push_back(
            "VeriArchive is running elevated; repository and home resolution may differ");
    }
    return session;
}

HostInfo host_info(std::string app_version)
{
    return {platform::os_name(), platform::host_name(), std::move(app_version)};
}

} // namespace va::app
