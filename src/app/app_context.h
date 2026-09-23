#pragma once
#include "common/config.h"
#include "common/repository_layout.h"

#include <string>

namespace va {

struct AppContext {
    std::filesystem::path executable_dir;
    std::filesystem::path user_config_dir;
    std::filesystem::path config_file;
    Config config;
    RepositoryLayout repo;
    std::string repo_selected_by;
    std::string repository_uuid;
};

} // namespace va