#include "cli/cli_support.h"

#include "app/session.h"
#include "common/utf8.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace va::cli {
namespace {

const std::set<std::string> value_options{
    "--config", "--repository", "--lang", "--label", "--io", "--resume", "--out", "--copy-of", "--seed",
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

Arguments parse_arguments(const std::vector<std::string>& raw, std::string& error)
{
    // A usage error does not throw the options away: run() still needs --lang and
    // --config to show the error. (Searching `raw` for "--lang" instead would go
    // wrong in `--label --lang`, where "--lang" is the label.)
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
            if (error.empty()) {
                error = "option " + token + " needs a value";
            }
            break; // the last word: every option before it has been read
        }
        const auto& value = raw[++index];
        if (token == "--config") {
            arguments.config = path_from_utf8(value);
        } else if (token == "--repository") {
            arguments.repository = path_from_utf8(value);
        } else if (token == "--lang") {
            if (!app::parse_language(value)) {
                if (error.empty()) {
                    error = "--lang takes en or zh-TW";
                }
                continue; // keep reading, so a later --config still chooses the language
            }
            arguments.language = value;
        } else {
            arguments.values[token] = value;
        }
    }
    return arguments;
}

std::expected<Session, Failure> open_session(const Arguments& arguments, bool initialise)
{
    auto session = app::open_session({initialise, arguments.repository, arguments.config});
    if (!session) {
        return std::unexpected(Failure{exit_code::repository, session.error()});
    }
    for (const auto& warning : session->warnings) {
        print_warning(arguments.text, warning);
    }
    return Session{std::move(session->context), std::move(session->writer)};
}

void print_json(const nlohmann::json& document)
{
    std::cout << document.dump(2) << '\n';
}

void print_warning(const app::Text& text, const std::string& message)
{
    std::cerr << text.format("warning: {}", text(message)) << '\n';
}

int report_failure(const Arguments& arguments, const Failure& failure)
{
    if (arguments.json) {
        print_json({{"error", failure.message}, {"exit_code", failure.code}});
    } else {
        std::cerr << arguments.text.format("error: {}", arguments.text(failure.message)) << '\n';
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
    return app::host_info(VERIARCHIVE_VERSION);
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
        std::cerr << '\r' << text_.format("scanning: {} entries", progress.items_done)
                  << std::string(20, ' ');
    } else {
        std::cerr << '\r'
                  << text_.format("hashing: {}/{} files, {} MiB", progress.items_done,
                         progress.items_total, progress.bytes_done / (1024 * 1024))
                  << std::string(10, ' ');
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
