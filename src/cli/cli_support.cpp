#include "cli/cli_support.h"

#include "app/session.h"
#include "common/utf8.h"
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

std::filesystem::path root_path(const std::string& word)
{
    return platform::resolve_subst_drive(without_trailing_separators(user_path(word)));
}

bool names_archive(const Uuid& uuid, const std::string& word)
{
    return uuid == word || (word.size() >= 8 && uuid.starts_with(word));
}

std::expected<Uuid, Failure> find_archive_uuid(ArchiveStore& store, const std::string& word)
{
    const auto archives = store.archives();
    if (!archives) {
        return std::unexpected(Failure{exit_code::repository, archives.error().message});
    }
    std::optional<Uuid> found;
    for (const auto& archive : *archives) {
        if (names_archive(archive.archive_uuid, word)) {
            if (found) {
                return std::unexpected(Failure{exit_code::usage, "several archives start with " + word});
            }
            found = archive.archive_uuid;
        }
    }
    if (!found) {
        return std::unexpected(Failure{exit_code::usage, "no archive " + word});
    }
    return *found;
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
    // Every report feeds the estimate, also those not shown.
    estimate_.update(progress, now);
    // The report that completes the count is always shown, so the line does
    // not keep the last time left while the results are written.
    const bool complete = progress.items_total > 0 && progress.items_done == progress.items_total;
    if (line_.shown() && !complete && now - last_ < std::chrono::milliseconds(200)) {
        return;
    }
    last_ = now;
    if (unit_ == ProgressUnit::entries) {
        write(text_.format("comparing: {}/{} entries", progress.items_done, progress.items_total));
    } else if (progress.items_total == 0) {
        write(text_.format("scanning: {} entries", progress.items_done));
    } else {
        write(text_.format("hashing: {}", app::describe_progress(text_, estimate_)));
    }
}

void CliProgress::write(const std::string& line)
{
    std::cerr << line_.replace(line);
    std::cerr.flush();
}

void CliProgress::show(const std::string& message)
{
    if (quiet_) {
        return;
    }
    write(message);
    last_ = {}; // long ago: the next report is shown at once, not held back by the rate limit
}

void CliProgress::finish()
{
    if (line_.shown()) {
        std::cerr << line_.clear();
        std::cerr.flush();
    }
}

} // namespace va::cli
