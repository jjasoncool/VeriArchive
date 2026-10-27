#include "test_support.h"

#include "core/repository/repository.h"
#include "common/utf8.h"
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

// config.repository exactly as it is stored in the file ("" when absent).
std::string stored_pin(const std::filesystem::path& config_file)
{
    std::ifstream input(config_file, std::ios::binary);
    const auto document = nlohmann::json::parse(input, nullptr, false);
    if (!document.is_object() || !document.contains("repository") ||
        !document["repository"].is_string()) {
        return {};
    }
    return document["repository"].get<std::string>();
}

void write_config_pin(const std::filesystem::path& config_file, const std::filesystem::path& pin)
{
    va::Config config;
    config.repository = pin;
    std::filesystem::create_directories(config_file.parent_path());
    CHECK(va::write_file_atomically(
        *va::platform::make_file_writer(), config_file, va::config_to_json(config)));
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
    // Outside the folder of the config file (<exe>/config.json): stored absolute.
    CHECK(stored_pin(context->config_file) ==
          va::path_to_utf8((temporary.path() / "new-repository").lexically_normal()));

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

// Decision 0011: an init that stopped between repository.json and the pin
// left a complete repository that no config file pins. Running `repo init`
// again opens it and pins it, with its uuid as the pin's record, in the
// portable folder (first use) and at a place named with --repository; the
// next command then finds it pinned.
void init_run_again_pins_a_complete_repository()
{
    TemporaryDirectory temporary("init-again");
    const auto writer = va::platform::make_file_writer();
    for (const bool named : {false, true}) {
        auto options = make_options(temporary.path() / (named ? "named" : "portable"));
        if (named) {
            options.explicit_repository = temporary.path() / "named" / "repository-SAMPLE";
        }
        auto first = va::resolve_repository(options, always_writable_local);
        CHECK(first && first->pins_repository && va::initialise_repository(*first, *writer));
        if (!first) {
            continue;
        }
        // The crash: the pin was never written.
        std::filesystem::remove(first->config_file);

        auto again = va::resolve_repository(options, always_writable_local);
        CHECK(again && again->pins_repository && va::initialise_repository(*again, *writer));
        if (!again) {
            continue;
        }
        CHECK(again->repository_uuid == first->repository_uuid);
        const auto saved = va::read_config(again->config_file);
        CHECK(saved && *saved && (*saved)->repository && (*saved)->last_repository_uuid == first->repository_uuid);

        options.initialise = false;
        options.explicit_repository.reset();
        auto pinned = va::resolve_repository(options, always_writable_local);
        CHECK(pinned && pinned->repo_selected_by == "pinned" && va::open_repository(*pinned));
        CHECK(pinned && pinned->repository_uuid == first->repository_uuid);
    }
}

// The whole content of `file`, byte for byte.
std::string file_bytes(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Decision 0037: R-83 step 5 belongs to the pin. A repository created and
// opened with --repository beside the pinned one, or named by
// VERIARCHIVE_REPOSITORY, is never compared with the pin's record of the uuid
// nor written into it: no warning, the config file unchanged byte for byte,
// and the pinned repository opened plainly afterwards gives no warning either.
void explicit_repository_leaves_the_pins_record()
{
    TemporaryDirectory temporary("explicit-record");
    auto options = make_options(temporary.path());
    const auto writer = va::platform::make_file_writer();
    auto pinned = va::resolve_repository(options, always_writable_local);
    CHECK(pinned && pinned->pins_repository && va::initialise_repository(*pinned, *writer));
    if (!pinned) {
        return;
    }
    const auto config_before = file_bytes(pinned->config_file);
    CHECK(config_before.find(pinned->repository_uuid) != std::string::npos);

    // `repo init --repository <other>` while a repository is pinned: created,
    // not pinned, and the config file left as it was.
    auto explicit_options = options;
    explicit_options.explicit_repository = temporary.path() / "other-SAMPLE";
    auto created = va::resolve_repository(explicit_options, always_writable_local);
    CHECK(created && created->repo_selected_by == "explicit" && !created->pins_repository);
    if (!created) {
        return;
    }
    CHECK(va::initialise_repository(*created, *writer));
    CHECK(va::record_repository_uuid(*created, *writer));
    CHECK(created->previous_repository_uuid.empty());
    CHECK(created->repository_uuid != pinned->repository_uuid);
    CHECK(file_bytes(pinned->config_file) == config_before);

    // Opened again for a command, by --repository and by VERIARCHIVE_REPOSITORY.
    explicit_options.initialise = false;
    auto environment_options = options;
    environment_options.initialise = false;
    environment_options.environment_repository = explicit_options.explicit_repository;
    for (const auto* chosen : {&explicit_options, &environment_options}) {
        auto opened = va::resolve_repository(*chosen, always_writable_local);
        CHECK(opened && opened->repo_selected_by == "explicit" && va::open_repository(*opened));
        if (!opened) {
            continue;
        }
        CHECK(va::record_repository_uuid(*opened, *writer));
        CHECK(opened->previous_repository_uuid.empty());
        CHECK(file_bytes(pinned->config_file) == config_before);
    }

    // The next plain command opens the pinned repository without a warning.
    options.initialise = false;
    auto plain = va::resolve_repository(options, always_writable_local);
    CHECK(plain && plain->repo_selected_by == "pinned" && va::open_repository(*plain));
    if (!plain) {
        return;
    }
    CHECK(va::record_repository_uuid(*plain, *writer));
    CHECK(plain->previous_repository_uuid.empty());
    CHECK(file_bytes(pinned->config_file) == config_before);
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
    // The home folder is not inside the user config folder: the pin is absolute.
    CHECK(context->config_file == options.user_config_dir / "config.json");
    CHECK(context->config.repository == expected.lexically_normal());
}

// Decision 0019: the first run of a portable install pins "data", relative to
// <exe>/config.json, and later writes of the config keep it relative.
void portable_first_run_pins_a_relative_path()
{
    TemporaryDirectory temporary("portable-pin");
    auto options = make_options(temporary.path());
    const auto writer = va::platform::make_file_writer();
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *writer));
    if (!context) {
        return;
    }
    CHECK(context->config_file == options.executable_dir / "config.json");
    CHECK(stored_pin(context->config_file) == "data");

    options.initialise = false;
    auto reopened = va::resolve_repository(options, always_writable_local);
    CHECK(reopened && va::open_repository(*reopened));
    if (!reopened) {
        return;
    }
    CHECK(reopened->repo_selected_by == "pinned");
    // Settings and repo info show the resolved RepositoryRoot, not "data".
    CHECK(reopened->repo.root == (options.executable_dir / "data").lexically_normal());
    reopened->config.last_repository_uuid.clear(); // makes record_repository_uuid write
    CHECK(va::record_repository_uuid(*reopened, *writer));
    CHECK(stored_pin(reopened->config_file) == "data");
}

// Decision 0019: the whole program folder (program, config.json and data)
// copied elsewhere uses the copied repository, even with the original gone.
void copied_program_folder_uses_its_own_repository()
{
    TemporaryDirectory temporary("portable-copy");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer()));
    if (!context) {
        return;
    }
    const auto copied = temporary.path() / "usb" / "VeriArchive";
    std::filesystem::create_directories(copied.parent_path());
    std::filesystem::copy(options.executable_dir, copied, std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(options.executable_dir); // as after a move

    options.initialise = false;
    options.executable_dir = copied;
    auto moved = va::resolve_repository(options, always_writable_local);
    CHECK(moved && va::open_repository(*moved));
    if (!moved) {
        return;
    }
    CHECK(moved->config_file == copied / "config.json");
    CHECK(moved->repo.root == (copied / "data").lexically_normal());
    CHECK(moved->repo_selected_by == "pinned");
    CHECK(moved->repository_uuid == context->repository_uuid);
}

// Decision 0019: a relative pin in the user config file is taken from that
// file's folder, not from the program folder and not from the current
// directory (R-84). Both of those hold a decoy repository.
void relative_pin_follows_its_config_file()
{
    TemporaryDirectory temporary("user-config-pin");
    auto options = make_options(temporary.path());
    options.initialise = false;
    write_config_pin(options.user_config_dir / "config.json", "repo");
    write_repository_metadata(options.user_config_dir / "repo");
    write_repository_metadata(options.executable_dir / "repo");
    write_repository_metadata(temporary.path() / "elsewhere" / "repo");

    const auto original_directory = std::filesystem::current_path();
    std::filesystem::current_path(temporary.path() / "elsewhere");
    auto context = va::resolve_repository(options, always_writable_local);
    std::filesystem::current_path(original_directory);

    CHECK(context.has_value());
    if (!context) {
        return;
    }
    CHECK(context->config_file == options.user_config_dir / "config.json");
    CHECK(context->repo.root == (options.user_config_dir / "repo").lexically_normal());
    CHECK(context->repo_selected_by == "pinned");
    CHECK(context->config.repository == std::filesystem::path("repo"));
}

// An absolute pin, written before decision 0019 or pointing outside the
// config file's folder, works as before and is never rewritten.
void absolute_pin_still_works()
{
    TemporaryDirectory temporary("absolute-pin");
    auto options = make_options(temporary.path());
    options.initialise = false;
    const auto repository = (temporary.path() / "repositories" / "main").lexically_normal();
    write_repository_metadata(repository);
    // A portable config.json pinning a repository outside the program folder.
    write_config_pin(options.executable_dir / "config.json", repository);

    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context.has_value());
    if (!context) {
        return;
    }
    CHECK(context->repo.root == repository);
    CHECK(context->repo_selected_by == "pinned");
    // The uuid open_repository would read; differing from the config's (none),
    // it makes record_repository_uuid write the config again.
    context->repository_uuid = "3f2b8c1e-0d4a-4c6e-9b7a-1e2d3c4b5a69";
    CHECK(va::record_repository_uuid(*context, *va::platform::make_file_writer()));
    CHECK(stored_pin(context->config_file) == va::path_to_utf8(repository));
}

// `repo init --repository P` with P inside the --config file's folder pins P
// relative to it, written with forward slashes on every system. The folder
// itself does not count as inside: that pin stays absolute.
void explicit_init_inside_config_folder_pins_a_relative_path()
{
    TemporaryDirectory temporary("explicit-relative");
    auto options = make_options(temporary.path());
    const auto settings = temporary.path() / "settings";
    std::filesystem::create_directories(settings);
    std::ofstream(settings / "config.json") << "{}\n";
    options.explicit_config = settings / "config.json";
    options.explicit_repository = settings / "repositories" / "main";

    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer()));
    if (!context) {
        return;
    }
    CHECK(stored_pin(settings / "config.json") == "repositories/main");

    options.explicit_repository.reset();
    options.initialise = false;
    const auto pinned = va::resolve_repository(options, always_writable_local);
    CHECK(pinned && pinned->repo.root == (settings / "repositories" / "main").lexically_normal());

    std::ofstream(settings / "config.json") << "{}\n";
    options.initialise = true;
    options.explicit_repository = settings;
    const auto at_folder = va::resolve_repository(options, always_writable_local);
    CHECK(at_folder && at_folder->config.repository == settings.lexically_normal());
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
    init_run_again_pins_a_complete_repository();
    explicit_repository_leaves_the_pins_record();
    relative_paths_ignore_working_directory();
    network_repository_is_rejected_by_default();
    unwritable_executable_directory_falls_back_to_home();
    portable_first_run_pins_a_relative_path();
    copied_program_folder_uses_its_own_repository();
    relative_pin_follows_its_config_file();
    absolute_pin_still_works();
    explicit_init_inside_config_folder_pins_a_relative_path();
}