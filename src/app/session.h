#pragma once
// Composition-root helper shared by the CLI and the GUI (R-05): resolves and
// opens the repository (HANDOFF §B7.4) and creates the platform services.
#include "app/app_context.h"
#include "app/text.h"
#include "core/engine/candidates.h"
#include "core/interfaces.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va::app {

struct SessionOptions {
    bool initialise = false;                          // repo init
    std::optional<std::filesystem::path> repository;  // --repository
    std::optional<std::filesystem::path> config;      // --config
};

struct Session {
    AppContext context;
    std::unique_ptr<IFileWriter> writer;
    std::vector<std::string> warnings;  // repository uuid changed, running elevated, ...
};

std::expected<Session, std::string> open_session(const SessionOptions& options);

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
