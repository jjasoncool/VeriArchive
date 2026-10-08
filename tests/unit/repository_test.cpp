#include "test_support.h"

#include "core/repository/repository.h"

#include <chrono>
#include <fstream>
#include <sqlite3.h>

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view test_name)
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("va-r2-" + std::string(test_name) + '-' + std::to_string(unique));
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

va::RepositoryOptions make_options(const std::filesystem::path& root)
{
    va::RepositoryOptions options;
    options.initialise = true;
    options.executable_dir = root / "exe";
    options.user_config_dir = root / "config";
    options.user_home = root / "home";
    return options;
}

const va::RepositoryProbes always_writable_local{
    [](const std::filesystem::path&) { return true; },
    [](const std::filesystem::path&) { return false; },
};

void write_repository_metadata(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    std::ofstream output(root / "repository.json");
    output << "{\"format\":\"veriarchive-repo\",\"version\":1,"
              "\"repository_uuid\":\"test-repository\",\"created\":\"now\"}\n";
}

void first_run_uses_executable_directory()
{
    TemporaryDirectory temporary("first-run-exe");
    auto options = make_options(temporary.path());

    auto context = va::resolve_repository(options, always_writable_local);

    CHECK(context.has_value());
    if (!context) { return; }
    CHECK(context->repo.root == options.executable_dir / "data");
    CHECK(context->repo_selected_by == "first-run:exe-dir");
}

void first_run_initialises_metadata_and_schema()
{
    TemporaryDirectory temporary("initialise");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context.has_value());
    if (!context) {
        return;
    }

    const auto result = va::initialise_repository(*context);

    CHECK(result.has_value());
    CHECK(std::filesystem::exists(context->repo.root / "repository.json"));
    CHECK(va::validate_schema(context->repo).has_value());

    const auto config_time = std::filesystem::last_write_time(context->config_file);
    CHECK(va::open_repository(*context).has_value());
    CHECK(std::filesystem::last_write_time(context->config_file) == config_time);

    sqlite3* database = nullptr;
    const auto database_name = context->repo.database().u8string();
    const int opened = sqlite3_open(
        reinterpret_cast<const char*>(database_name.c_str()), &database);
    CHECK(opened == SQLITE_OK);
    if (opened == SQLITE_OK) {
        CHECK(sqlite3_exec(database, "DROP TABLE files", nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    sqlite3_close(database);
    CHECK(!va::open_repository(*context));
}

void pinned_unavailable_repository_does_not_fall_back()
{
    TemporaryDirectory temporary("pinned-missing");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context.has_value());
    if (!context) {
        return;
    }
    const auto initialized = va::initialise_repository(*context);
    CHECK(initialized.has_value());
    if (!initialized) { return; }

    std::filesystem::remove(context->repo.root / "repository.json");
    options.executable_dir = temporary.path() / "different-executable";
    options.explicit_config = context->config_file;

    CHECK(!va::resolve_repository(options, always_writable_local).has_value());
}

void explicit_repository_must_already_be_initialised()
{
    TemporaryDirectory temporary("explicit");
    auto options = make_options(temporary.path());
    options.explicit_repository = temporary.path() / "missing";

    CHECK(!va::resolve_repository(options, always_writable_local).has_value());

    write_repository_metadata(temporary.path() / "existing");
    options.explicit_repository = temporary.path() / "existing";
    const auto context = va::resolve_repository(options, always_writable_local);

    CHECK(context.has_value());
    if (!context) { return; }
    CHECK(context->repo_selected_by == "explicit");
}

void network_repository_is_rejected_by_default()
{
    TemporaryDirectory temporary("network");
    auto options = make_options(temporary.path());
    const va::RepositoryProbes network{
        [](const std::filesystem::path&) { return true; },
        [](const std::filesystem::path&) { return true; },
    };

    CHECK(!va::resolve_repository(options, network).has_value());
}

void unwritable_executable_directory_falls_back_to_home()
{
    TemporaryDirectory temporary("home-fallback");
    auto options = make_options(temporary.path());
    const auto expected = options.user_home / "VeriArchive";
    const va::RepositoryProbes only_home_writable{
        [&expected](const std::filesystem::path& path) { return path == expected; },
        [](const std::filesystem::path&) { return false; },
    };

    const auto context = va::resolve_repository(options, only_home_writable);

    CHECK(context.has_value());
    if (!context) { return; }
    CHECK(context->repo.root == expected);
    CHECK(context->repo_selected_by == "first-run:home");
}

} // namespace

void repository_tests()
{
    first_run_uses_executable_directory();
    first_run_initialises_metadata_and_schema();
    pinned_unavailable_repository_does_not_fall_back();
    explicit_repository_must_already_be_initialised();
    network_repository_is_rejected_by_default();
    unwritable_executable_directory_falls_back_to_home();
}