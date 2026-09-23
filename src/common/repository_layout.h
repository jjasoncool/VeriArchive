#pragma once
#include <filesystem>

namespace va {

struct RepositoryLayout {
    std::filesystem::path root;

    std::filesystem::path database() const {
        return root / "veriarchive.db";
    }
    std::filesystem::path manifests() const {
        return root / "manifests";
    }
    std::filesystem::path keys() const {
        return root / "keys";
    }
    std::filesystem::path logs() const {
        return root / "logs";
    }
    std::filesystem::path cache() const {
        return root / "cache";
    }
};

} // namespace va