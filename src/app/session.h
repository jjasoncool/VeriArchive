#pragma once
// Composition-root helper shared by the CLI and the GUI (R-05): resolves and
// opens the repository (HANDOFF §B7.4) and creates the platform services.
#include "app/app_context.h"
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

} // namespace va::app
