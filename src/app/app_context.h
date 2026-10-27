#pragma once
#include "common/config.h"
#include "common/repository_layout.h"

#include <optional>
#include <string>

namespace va {

// What opening a repository changed in its database: its schema version was
// raised from `from` to `to` (R-92), after the database as it was, with its
// verification history, was copied beside it to `copy` (decision 0032).
struct SchemaUpgrade {
    int from = 0;
    int to = 0;
    std::filesystem::path copy;
};

struct AppContext {
    std::filesystem::path executable_dir;
    std::filesystem::path user_config_dir;
    std::filesystem::path config_file;
    Config config;
    RepositoryLayout repo;
    std::string repo_selected_by;
    std::string repository_uuid;
    // Set by resolve_repository when `repo init` pins RepositoryRoot (§B7.4
    // step 3, or `repo init --repository P` with nothing pinned): only then
    // does initialise_repository write the pin, with the repository's uuid as
    // the pin's record, into the config file (decision 0037).
    bool pins_repository = false;
    // Non-empty when the pinned repository's uuid differs from the uuid
    // recorded in the config (R-83 step 5, decision 0037); the user interface
    // shows a warning. A repository chosen with --repository or
    // VERIARCHIVE_REPOSITORY is never compared with that record.
    std::string previous_repository_uuid;
    // Set when opening the repository upgraded its database; the user
    // interface says once where the copy from before the upgrade is.
    std::optional<SchemaUpgrade> schema_upgrade;
};

} // namespace va
