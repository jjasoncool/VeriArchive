#pragma once
// Composition-root helper shared by the CLI and the GUI (R-05): resolves and
// opens the repository (HANDOFF §B7.4) and creates the platform services.
#include "app/app_context.h"
#include "app/text.h"
#include "core/engine/candidates.h"
#include "core/interfaces.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va::app {

struct SessionOptions {
    bool initialise = false;                          // repo init
    std::optional<std::filesystem::path> repository;  // --repository
    std::optional<std::filesystem::path> config;      // --config
    // Whether this process runs elevated (R-12): the platform's answer when
    // empty, as in the user interfaces; tests give their own.
    std::function<bool()> elevated;
};

struct Session {
    AppContext context;
    std::unique_ptr<IFileWriter> writer;
    std::vector<std::string> warnings;  // running elevated, repository uuid changed, ...
};

// Why no repository could be opened, with the warnings that hold all the
// same: running elevated (R-12), which may be why, as the home folder and the
// config file then resolve otherwise (decision 0037).
struct SessionFailure {
    std::string message;
    std::vector<std::string> warnings;
};

std::expected<Session, SessionFailure> open_session(const SessionOptions& options);

// R-12: the warning that this process runs elevated, where it does; for a
// command that opens no repository (`media list`).
std::optional<std::string> elevated_warning();

// Operating system, host name and version for manifests and history.
HostInfo host_info(std::string app_version);

// The interface language (decision 0017): `explicit_code` (--lang), then the
// VERIARCHIVE_LANG environment variable, then the config file's "language",
// then the operating system's language, then English.
Language interface_language(std::optional<std::string> explicit_code,
    std::optional<std::filesystem::path> explicit_config);

// Remembers the language in the config file that interface_language() reads.
std::expected<void, std::string> save_language(Language language,
    std::optional<std::filesystem::path> explicit_config);

} // namespace va::app
