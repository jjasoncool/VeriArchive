#pragma once
// Shared pieces of the command-line interface: argument parsing, exit codes
// (HANDOFF §B16), repository opening, and text/JSON output.
#include "app/app_context.h"
#include "core/interfaces.h"
#include "core/engine/candidates.h"
#include "core/repository/repository.h"

#include <chrono>
#include <initializer_list>

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace va::cli {

namespace exit_code {
inline constexpr int passed = 0;
inline constexpr int passed_with_warnings = 2;
inline constexpr int failed = 3;
inline constexpr int incomplete = 4;
inline constexpr int usage = 10;
inline constexpr int repository = 11;
inline constexpr int identity_decision = 12;
} // namespace exit_code

// Command line split into global options, the command words, and the
// command's own options. Every command accepts --json (§B16).
struct Arguments {
    bool json = false;
    std::optional<std::filesystem::path> config;
    std::optional<std::filesystem::path> repository;
    std::vector<std::string> words;               // e.g. {"repo", "info"} or {"verify", "<uuid>"}
    std::map<std::string, std::string> values;    // e.g. {"--label", "ARCH-0001"}
    std::set<std::string> flags;                  // e.g. {"--full"}

    bool has(const std::string& flag) const { return flags.contains(flag); }
    std::optional<std::string> value(const std::string& option) const;
};

// Splits the command line. Known value options (--config, --label, --out, ...)
// take the next word as their value; any other "--name" is a flag.
std::optional<Arguments> parse_arguments(const std::vector<std::string>& raw, std::string& error);

// A command's failure: the exit code to return and a message for the user.
struct Failure {
    int code;
    std::string message;
};

// Everything a command needs: the resolved repository context and the
// platform services, created once here (the composition root, R-05).
struct Session {
    AppContext context;
    std::unique_ptr<IFileWriter> writer;
};

// Resolves and opens (or initialises) the repository per HANDOFF §B7.4.
std::expected<Session, Failure> open_session(const Arguments& arguments, bool initialise);

// A usage failure when the command received an option it does not accept.
std::optional<Failure> check_options(const Arguments& arguments,
    std::initializer_list<std::string_view> flags, std::initializer_list<std::string_view> values);

// A path typed by the user; relative paths are taken relative to the shell's
// working directory, as users expect.
std::filesystem::path user_path(const std::string& word);

// Operating system, host name and version for manifests and history.
HostInfo host_info();

// Progress on stderr, at most five updates a second; silent in --json mode.
class CliProgress final : public IProgressSink {
public:
    explicit CliProgress(bool quiet) : quiet_(quiet) {}
    ~CliProgress() override { finish(); }
    void on_progress(const Progress& progress) override;
    void finish();

private:
    bool quiet_;
    bool shown_ = false;
    std::chrono::steady_clock::time_point last_{};
};

// Output helpers: text goes to stdout line by line; JSON is printed once.
void print_json(const nlohmann::json& document);
void print_warning(const std::string& message);
int report_failure(const Arguments& arguments, const Failure& failure);

} // namespace va::cli
