#include "cli/cli_support.h"

#include "common/utf8.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace va::cli {
namespace {

const std::set<std::string> value_options{
    "--config", "--repository", "--label", "--io", "--resume", "--out", "--copy-of", "--seed",
};

} // namespace

std::optional<std::string> Arguments::value(const std::string& option) const
{
    const auto found = values.find(option);
    if (found == values.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<Arguments> parse_arguments(const std::vector<std::string>& raw, std::string& error)
{
    Arguments arguments;
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto& token = raw[index];
        if (!token.starts_with("--")) {
            arguments.words.push_back(token);
            continue;
        }
        if (token == "--json") {
            arguments.json = true;
            continue;
        }
        if (!value_options.contains(token)) {
            arguments.flags.insert(token);
            continue;
        }
        if (index + 1 >= raw.size()) {
            // `register ... --resume` is a flag; `verify ... --resume RUN_ID` takes a value (§B16).
            if (token == "--resume") {
                arguments.flags.insert(token);
                continue;
            }
            error = "option " + token + " needs a value";
            return std::nullopt;
        }
        const auto& value = raw[++index];
        if (token == "--config") {
            arguments.config = path_from_utf8(value);
        } else if (token == "--repository") {
            arguments.repository = path_from_utf8(value);
        } else {
            arguments.values[token] = value;
        }
    }
    return arguments;
}

std::expected<Session, Failure> open_session(const Arguments& arguments, bool initialise)
{
    const auto executable = platform::executable_path();
    const auto config_directory = platform::user_config_dir();
    const auto home = platform::user_home();
    if (!executable || !config_directory || !home) {
        const auto& error = !executable ? executable.error()
                            : !config_directory ? config_directory.error()
                                                : home.error();
        return std::unexpected(Failure{exit_code::repository, error.message});
    }

    RepositoryOptions options;
    options.initialise = initialise;
    options.executable_dir = executable->parent_path();
    options.user_config_dir = *config_directory;
    options.user_home = *home;
    options.explicit_repository = arguments.repository;
    options.explicit_config = arguments.config;
    if (const char* environment_repository = std::getenv("VERIARCHIVE_REPOSITORY")) {
        options.environment_repository = path_from_utf8(environment_repository);
    }

    const RepositoryProbes probes{platform::writable_directory, platform::network_filesystem};
    auto context = resolve_repository(options, probes);
    if (!context) {
        return std::unexpected(Failure{exit_code::repository, context.error().message});
    }

    Session session{std::move(*context), platform::make_file_writer()};
    auto opened = initialise ? initialise_repository(session.context, *session.writer)
                             : open_repository(session.context);
    if (!opened) {
        return std::unexpected(Failure{exit_code::repository, opened.error().message});
    }
    if (auto recorded = record_repository_uuid(session.context, *session.writer); !recorded) {
        return std::unexpected(Failure{exit_code::repository, recorded.error().message});
    }
    if (!session.context.previous_repository_uuid.empty()) {
        print_warning("repository_uuid changed from " + session.context.previous_repository_uuid +
                      " to " + session.context.repository_uuid + "; recorded the new uuid");
    }
    if (platform::process_is_elevated()) {
        print_warning("VeriArchive is running elevated; repository and home resolution may differ");
    }
    return session;
}

void print_json(const nlohmann::json& document)
{
    std::cout << document.dump(2) << '\n';
}

void print_warning(const std::string& message)
{
    std::cerr << "warning: " << message << '\n';
}

int report_failure(const Arguments& arguments, const Failure& failure)
{
    if (arguments.json) {
        print_json({{"error", failure.message}, {"exit_code", failure.code}});
    } else {
        std::cerr << "error: " << failure.message << '\n';
    }
    return failure.code;
}

} // namespace va::cli

namespace va::cli {

std::optional<Failure> check_options(const Arguments& arguments,
    std::initializer_list<std::string_view> flags, std::initializer_list<std::string_view> values)
{
    for (const auto& flag : arguments.flags) {
        if (std::find(flags.begin(), flags.end(), flag) == flags.end()) {
            return Failure{exit_code::usage, "unknown option " + flag};
        }
    }
    for (const auto& [option, value] : arguments.values) {
        if (std::find(values.begin(), values.end(), option) == values.end()) {
            return Failure{exit_code::usage, "option " + option + " is not used by this command"};
        }
    }
    return std::nullopt;
}

std::filesystem::path user_path(const std::string& word)
{
    std::error_code error;
    auto path = std::filesystem::absolute(path_from_utf8(word), error);
    return error ? path_from_utf8(word) : path.lexically_normal();
}

HostInfo host_info()
{
    return {platform::os_name(), platform::host_name(), VERIARCHIVE_VERSION};
}

void CliProgress::on_progress(const Progress& progress)
{
    if (quiet_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (shown_ && now - last_ < std::chrono::milliseconds(200)) {
        return;
    }
    last_ = now;
    shown_ = true;
    if (progress.items_total == 0) {
        std::cerr << "\rscanning: " << progress.items_done << " entries" << std::string(20, ' ');
    } else {
        std::cerr << "\rhashing: " << progress.items_done << '/' << progress.items_total << " files, "
                  << progress.bytes_done / (1024 * 1024) << " MiB" << std::string(10, ' ');
    }
    std::cerr.flush();
}

void CliProgress::finish()
{
    if (shown_ && !quiet_) {
        std::cerr << '\r' << std::string(70, ' ') << '\r';
        std::cerr.flush();
    }
    shown_ = false;
}

} // namespace va::cli
