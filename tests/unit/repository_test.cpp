#include "test_support.h"

#include "core/repository/repository.h"
#include "common/uuid.h"
#include "core/write_file.h"
#include "platform/file_writer.h"

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
              "\"repository_uuid\":\"3f2b8c1e-0d4a-4c6e-9b7a-1e2d3c4b5a69\",\"created\":\"now\"}\n";
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

    const auto result = va::initialise_repository(*context, *va::platform::make_file_writer());

    CHECK(result.has_value());
    CHECK(std::filesystem::exists(context->repo.root / "repository.json"));
    CHECK(va::is_canonical_uuid(context->repository_uuid));
    CHECK(!va::is_canonical_uuid("3F2B8C1E-0D4A-4C6E-9B7A-1E2D3C4B5A69"));
    CHECK(!va::is_canonical_uuid("3f2b8c1e0d4a4c6e9b7a1e2d3c4b5a69"));
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

// A locked or damaged database is reported with SQLite's own reason; only a
// database without VeriArchive's tables is an unsupported schema.
void schema_check_reports_the_sqlite_reason()
{
    TemporaryDirectory temporary("schema-reason");
    const va::RepositoryLayout layout{temporary.path()};
    std::filesystem::create_directories(layout.root);
    const auto database_name = layout.database().u8string();
    const auto run = [&](sqlite3* database, const char* sql) {
        CHECK(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    };

    std::ofstream(layout.database(), std::ios::binary) << std::string(4096, 'x');
    const auto damaged = va::validate_schema(layout);
    CHECK(!damaged && damaged.error().message == "cannot prepare SQL: file is not a database");
    std::filesystem::remove(layout.database());

    sqlite3* database = nullptr;
    CHECK(sqlite3_open(reinterpret_cast<const char*>(database_name.c_str()), &database) == SQLITE_OK);
    run(database, "CREATE TABLE other(x)");
    const auto foreign = va::validate_schema(layout);
    CHECK(!foreign && foreign.error().message == "unsupported repository schema");

    // Another connection holds the database while this one checks it.
    run(database, "CREATE TABLE schema_version(version INTEGER); INSERT INTO schema_version VALUES (1)");
    run(database, "BEGIN EXCLUSIVE");
    const auto locked = va::validate_schema(layout);
    CHECK(!locked && locked.error().message == "cannot prepare SQL: database is locked");
    run(database, "ROLLBACK");
    sqlite3_close(database);
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
    const auto initialized = va::initialise_repository(*context, *va::platform::make_file_writer());
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
    options.initialise = false;
    options.explicit_repository = temporary.path() / "missing";

    CHECK(!va::resolve_repository(options, always_writable_local).has_value());

    write_repository_metadata(temporary.path() / "existing");
    options.explicit_repository = temporary.path() / "existing";
    const auto context = va::resolve_repository(options, always_writable_local);

    CHECK(context.has_value());
    if (!context) { return; }
    CHECK(context->repo_selected_by == "explicit");
}

// §B16 `repo init --repository P` creates the repository at P and pins it.
void explicit_init_creates_and_pins_repository()
{
    TemporaryDirectory temporary("explicit-init");
    auto options = make_options(temporary.path());
    options.explicit_repository = temporary.path() / "new-repository";

    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context.has_value());
    if (!context) {
        return;
    }
    CHECK(va::initialise_repository(*context, *va::platform::make_file_writer()));
    CHECK(std::filesystem::exists(temporary.path() / "new-repository" / "repository.json"));

    options.explicit_repository.reset();
    options.initialise = false;
    const auto pinned = va::resolve_repository(options, always_writable_local);
    CHECK(pinned && pinned->repo_selected_by == "pinned");
}

void malformed_repository_uuid_is_rejected()
{
    TemporaryDirectory temporary("bad-uuid");
    const auto root = temporary.path() / "repository";
    std::filesystem::create_directories(root);
    std::ofstream(root / "repository.json")
        << "{\"format\":\"veriarchive-repo\",\"version\":1,"
           "\"repository_uuid\":\"NOT-A-UUID\",\"created\":\"now\"}\n";
    auto options = make_options(temporary.path());
    options.explicit_repository = root;

    CHECK(!va::resolve_repository(options, always_writable_local).has_value());
}

// R-83 step 5: a repository whose uuid differs from the recorded one is opened
// with a warning, and the new uuid is recorded.
void changed_repository_uuid_is_reported_and_recorded()
{
    TemporaryDirectory temporary("uuid-changed");
    auto options = make_options(temporary.path());
    const auto writer = va::platform::make_file_writer();
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *writer));
    if (!context) {
        return;
    }
    const auto actual_uuid = context->repository_uuid;
    const std::string recorded_uuid = "00000000-0000-4000-8000-000000000000";
    context->config.last_repository_uuid = recorded_uuid;
    CHECK(va::write_file_atomically(*writer, context->config_file, va::config_to_json(context->config)));

    options.initialise = false;
    auto reopened = va::resolve_repository(options, always_writable_local);
    CHECK(reopened && va::open_repository(*reopened));
    if (!reopened) {
        return;
    }
    CHECK(va::record_repository_uuid(*reopened, *writer));
    CHECK(reopened->previous_repository_uuid == recorded_uuid);
    const auto saved = va::read_config(reopened->config_file);
    CHECK(saved && *saved && (*saved)->last_repository_uuid == actual_uuid);
}

// R-84: relative locations resolve against the executable directory, never the
// process working directory. The test moves the working directory elsewhere.
void relative_paths_ignore_working_directory()
{
    TemporaryDirectory temporary("cwd");
    auto options = make_options(temporary.path());
    write_repository_metadata(options.executable_dir / "repo");
    std::filesystem::create_directories(temporary.path() / "elsewhere");
    write_repository_metadata(temporary.path() / "elsewhere" / "repo");

    const auto original_directory = std::filesystem::current_path();
    std::filesystem::current_path(temporary.path() / "elsewhere");
    options.explicit_repository = std::filesystem::path("repo");
    const auto context = va::resolve_repository(options, always_writable_local);
    std::filesystem::current_path(original_directory);

    CHECK(context.has_value());
    if (context) {
        CHECK(context->repo.root == (options.executable_dir / "repo").lexically_normal());
    }
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
    schema_check_reports_the_sqlite_reason();
    pinned_unavailable_repository_does_not_fall_back();
    explicit_repository_must_already_be_initialised();
    explicit_init_creates_and_pins_repository();
    malformed_repository_uuid_is_rejected();
    changed_repository_uuid_is_reported_and_recorded();
    relative_paths_ignore_working_directory();
    network_repository_is_rejected_by_default();
    unwritable_executable_directory_falls_back_to_home();
}