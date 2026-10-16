#include "test_support.h"

#include "core/repository/repository.h"
#include "app/session.h"
#include "core/repository/archive_store.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/write_file.h"
#include "platform/file_writer.h"

#include <algorithm>
#include <chrono>
#include <expected>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <semaphore>
#include <set>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <thread>

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

    const auto result = va::initialise_repository(*context, *va::platform::make_file_writer(), test::today);

    CHECK(result.has_value());
    CHECK(std::filesystem::exists(context->repo.root / "repository.json"));
    CHECK(va::is_canonical_uuid(context->repository_uuid));
    CHECK(!va::is_canonical_uuid("3F2B8C1E-0D4A-4C6E-9B7A-1E2D3C4B5A69"));
    CHECK(!va::is_canonical_uuid("3f2b8c1e0d4a4c6e9b7a1e2d3c4b5a69"));
    CHECK(va::validate_schema(context->repo).has_value());

    const auto config_time = std::filesystem::last_write_time(context->config_file);
    CHECK(va::open_repository(*context, test::today).has_value());
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
    CHECK(!va::open_repository(*context, test::today));
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
    CHECK(!damaged && damaged.error().kind == va::IoErrorKind::DatabaseDamaged &&
          damaged.error().message == "the repository's database " + va::path_to_utf8(layout.database()) +
                                         " is not a database; make it again from the manifests with `repo "
                                         "rebuild-index`");
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
    const auto initialized = va::initialise_repository(*context, *va::platform::make_file_writer(), test::today);
    CHECK(initialized.has_value());
    if (!initialized) { return; }

    std::filesystem::remove(context->repo.root / "repository.json");
    options.executable_dir = temporary.path() / "different-executable";
    options.explicit_config = context->config_file;

    // The error names the pinned repository and the config file that names
    // it (decision 0037): the operator may have to attach a disk.
    const auto unavailable = va::resolve_repository(options, always_writable_local);
    CHECK(!unavailable.has_value());
    CHECK(!unavailable &&
          unavailable.error().message == "the configured repository " + va::path_to_utf8(context->repo.root) +
                                             " is unavailable or is not a VeriArchive repository (the config file " +
                                             va::path_to_utf8(context->config_file) + " names it)");
}

void explicit_repository_must_already_be_initialised()
{
    TemporaryDirectory temporary("explicit");
    auto options = make_options(temporary.path());
    options.initialise = false;
    options.explicit_repository = temporary.path() / "missing";

    // Named with its path, and where it was named (decision 0037).
    const auto missing = va::path_to_utf8((temporary.path() / "missing").lexically_normal());
    const auto named = va::resolve_repository(options, always_writable_local);
    CHECK(!named && named.error().message ==
                        "the repository " + missing + " is unavailable or is not a VeriArchive repository");
    options.explicit_repository.reset();
    options.environment_repository = temporary.path() / "missing";
    const auto from_environment = va::resolve_repository(options, always_writable_local);
    CHECK(!from_environment && from_environment.error().message ==
                                   "VERIARCHIVE_REPOSITORY names the repository " + missing +
                                       ", which is unavailable or is not a VeriArchive repository");
    options.environment_repository.reset();

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
    CHECK(va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
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
    CHECK(context && va::initialise_repository(*context, *writer, test::today));
    if (!context) {
        return;
    }
    const auto actual_uuid = context->repository_uuid;
    const std::string recorded_uuid = "00000000-0000-4000-8000-000000000000";
    context->config.last_repository_uuid = recorded_uuid;
    CHECK(va::write_file_atomically(*writer, context->config_file, va::config_to_json(context->config)));

    options.initialise = false;
    auto reopened = va::resolve_repository(options, always_writable_local);
    CHECK(reopened && va::open_repository(*reopened, test::today));
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
        CHECK(first && first->pins_repository && va::initialise_repository(*first, *writer, test::today));
        if (!first) {
            continue;
        }
        // The crash: the pin was never written.
        std::filesystem::remove(first->config_file);

        auto again = va::resolve_repository(options, always_writable_local);
        CHECK(again && again->pins_repository && va::initialise_repository(*again, *writer, test::today));
        if (!again) {
            continue;
        }
        CHECK(again->repository_uuid == first->repository_uuid);
        const auto saved = va::read_config(again->config_file);
        CHECK(saved && *saved && (*saved)->repository && (*saved)->last_repository_uuid == first->repository_uuid);

        options.initialise = false;
        options.explicit_repository.reset();
        auto pinned = va::resolve_repository(options, always_writable_local);
        CHECK(pinned && pinned->repo_selected_by == "pinned" && va::open_repository(*pinned, test::today));
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
    CHECK(pinned && pinned->pins_repository && va::initialise_repository(*pinned, *writer, test::today));
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
    CHECK(va::initialise_repository(*created, *writer, test::today));
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
        CHECK(opened && opened->repo_selected_by == "explicit" && va::open_repository(*opened, test::today));
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
    CHECK(plain && plain->repo_selected_by == "pinned" && va::open_repository(*plain, test::today));
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
    CHECK(context && va::initialise_repository(*context, *writer, test::today));
    if (!context) {
        return;
    }
    CHECK(context->config_file == options.executable_dir / "config.json");
    CHECK(stored_pin(context->config_file) == "data");

    options.initialise = false;
    auto reopened = va::resolve_repository(options, always_writable_local);
    CHECK(reopened && va::open_repository(*reopened, test::today));
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
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
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
    CHECK(moved && va::open_repository(*moved, test::today));
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
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
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

// R-12 (decision 0037): the warning that VeriArchive runs elevated comes
// first with a session that opens, and with one that cannot be opened, where
// running elevated may be why; not where it does not run elevated.
void elevated_warning_holds_also_without_a_repository()
{
    TemporaryDirectory temporary("elevated");
    const std::vector<std::string> elevated{
        "VeriArchive is running elevated; repository and home resolution may differ"};
    va::app::SessionOptions options;
    options.config = temporary.path() / "missing-config-SAMPLE.json";
    options.elevated = [] { return true; };
    const auto failed = va::app::open_session(options);
    CHECK(!failed && failed.error().message == "Explicit config does not exist" &&
          failed.error().warnings == elevated);
    options.elevated = [] { return false; };
    const auto failed_unelevated = va::app::open_session(options);
    CHECK(!failed_unelevated && failed_unelevated.error().warnings.empty());

    // A repository named with --repository, beside a config file of its own.
    auto repository_options = make_options(temporary.path());
    repository_options.explicit_repository = temporary.path() / "repository-SAMPLE";
    std::filesystem::create_directories(temporary.path() / "settings");
    std::ofstream(temporary.path() / "settings" / "config.json") << "{}\n";
    repository_options.explicit_config = temporary.path() / "settings" / "config.json";
    auto created = va::resolve_repository(repository_options, always_writable_local);
    CHECK(created && va::initialise_repository(*created, *va::platform::make_file_writer(), test::today));
    options.repository = repository_options.explicit_repository;
    options.config = repository_options.explicit_config;
    options.elevated = [] { return true; };
    const auto opened = va::app::open_session(options);
    CHECK(opened && opened->warnings == elevated);
    options.elevated = [] { return false; };
    const auto opened_unelevated = va::app::open_session(options);
    CHECK(opened_unelevated && opened_unelevated->warnings.empty());
}

// A repository made for the database tests below (sample data): resolved
// and initialised at `root`/repository-SAMPLE with a config file of its own.
std::optional<va::AppContext> sample_repository(const std::filesystem::path& root)
{
    auto options = make_options(root);
    options.explicit_repository = root / "repository-SAMPLE";
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return std::nullopt;
    }
    return std::move(*context);
}

// Runs `sql` in a database of its own at `file` (sample data).
void run_sql(const std::filesystem::path& file, const char* sql)
{
    sqlite3* database = nullptr;
    const auto name = file.u8string();
    CHECK(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
}

// The schema version in the repository's database, read through a
// connection of its own; -1 when it cannot be read.
int stored_schema_version(const va::RepositoryLayout& layout)
{
    sqlite3* database = nullptr;
    const auto name = layout.database().u8string();
    int version = -1;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT version FROM schema_version", -1, &statement, nullptr) ==
                SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            version = sqlite3_column_int(statement, 0);
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return version;
}

// Runs `sql` in the repository's database (sample data).
void run_sql(const va::RepositoryLayout& layout, const char* sql)
{
    run_sql(layout.database(), sql);
}

// Decision 0036: what is at veriarchive.db. A database of this version's
// schema can be used. One that is missing, empty (0 bytes, or no table),
// not a database (sample bytes) or damaged (its first page's b-tree marked
// with a type that does not exist, sample damage) cannot, and every command
// names `repo rebuild-index` for it. One that holds another program's
// tables, a later version's schema, or that is locked is none of these: it
// is an error, and nothing ever replaces it.
void database_state_tells_what_cannot_be_used()
{
    TemporaryDirectory temporary("database-state");
    auto context = sample_repository(temporary.path());
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    const auto database = layout.database();
    const auto database_text = va::path_to_utf8(database);
    const auto state = [&] { return va::database_state(layout); };
    const auto opened_says = [&](const std::string& what) {
        auto reopened = *context;
        const auto opened = va::open_repository(reopened, test::today);
        CHECK(!opened && opened.error().message == "the repository's database " + database_text + " " + what +
                                                       "; make it again from the manifests with `repo rebuild-index`");
    };
    CHECK(state() == va::DatabaseState::Usable);
    const auto usable = file_bytes(database);

    std::filesystem::remove(database);
    CHECK(state() == va::DatabaseState::Missing);
    opened_says("is missing");

    std::ofstream(database, std::ios::binary).flush();
    CHECK(state() == va::DatabaseState::Empty);
    opened_says("is empty");
    std::filesystem::remove(database);
    run_sql(database, "CREATE TABLE sample(x); DROP TABLE sample;");
    CHECK(std::filesystem::file_size(database) > 0 && state() == va::DatabaseState::Empty);
    opened_says("is empty");

    std::ofstream(database, std::ios::binary | std::ios::trunc) << std::string(4096, 'x');
    CHECK(state() == va::DatabaseState::NotADatabase);
    opened_says("is not a database");

    // A database made again whose rebuild did not finish: its mark (sample).
    std::ofstream(database, std::ios::binary | std::ios::trunc) << usable;
    run_sql(database, "INSERT INTO settings VALUES ('rebuild:replaced-database', "
                      "'{\"state\":\"damaged\",\"moved_to\":null}');");
    CHECK(state() == va::DatabaseState::Unfilled);
    {
        auto reopened = *context;
        const auto opened = va::open_repository(reopened, test::today);
        CHECK(!opened && opened.error().message ==
                             "the repository's database " + database_text +
                                 " was made again, but its rebuild from the manifests did not finish; finish it with "
                                 "`repo rebuild-index`");
    }

    // Byte 100 starts the first page's b-tree, whose type is 2, 5, 10 or 13.
    auto damaged = usable;
    CHECK(damaged.size() > 100);
    damaged[100] = 7;
    std::ofstream(database, std::ios::binary | std::ios::trunc) << damaged;
    CHECK(state() == va::DatabaseState::Damaged);
    opened_says("is damaged");

    // This version's schema without one of its tables (sample damage).
    std::ofstream(database, std::ios::binary | std::ios::trunc) << usable;
    run_sql(database, "DROP TABLE chunks;");
    CHECK(state() == va::DatabaseState::Damaged);
    opened_says("is damaged");

    // Another program's database, and a later version's schema: errors.
    std::filesystem::remove(database);
    run_sql(database, "CREATE TABLE sample(x);");
    const auto other = state();
    CHECK(!other && other.error().message == "unsupported repository schema");
    std::filesystem::remove(database);
    run_sql(database, "CREATE TABLE schema_version (version INTEGER NOT NULL); INSERT INTO schema_version VALUES (4);");
    const auto later = state();
    CHECK(!later && later.error().message == "unsupported repository schema");

    // A database of this schema that another connection holds locked.
    std::ofstream(database, std::ios::binary | std::ios::trunc) << usable;
    CHECK(state() == va::DatabaseState::Usable);
    sqlite3* holder = nullptr;
    const auto name = database.u8string();
    CHECK(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &holder) == SQLITE_OK);
    CHECK(sqlite3_exec(holder, "PRAGMA journal_mode=DELETE; BEGIN EXCLUSIVE; INSERT INTO settings VALUES ('sample', "
                               "'locked');",
              nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto locked = state();
    CHECK(!locked && locked.error().message.ends_with("database is locked"));
    sqlite3_exec(holder, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(holder);
}

// Decision 0036: opened for `repo rebuild-index`, a database that cannot be
// used is put aside as veriarchive.db.damaged-<UTC date and time>, with the
// journal beside it, and a new database of this schema made in its place;
// the repository keeps its uuid. A missing one is made anew, with a journal
// left beside it put aside too. A usable database, and one that is no state
// (another program's tables), are left as they are.
void rebuild_makes_a_database_that_cannot_be_used_again()
{
    TemporaryDirectory temporary("database-again");
    auto context = sample_repository(temporary.path());
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    const auto database = layout.database();
    const auto uuid = context->repository_uuid;
    const auto rebuild = [&] {
        auto reopened = *context;
        reopened.repository_uuid.clear();
        auto made = va::open_repository_to_rebuild(reopened, test::today);
        CHECK(!made || reopened.repository_uuid == uuid);
        return made;
    };
    const auto aside_files = [&] {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(layout.root)) {
            const auto name = va::path_to_utf8(entry.path().filename());
            if (name.starts_with("veriarchive.db.damaged-")) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    };

    const auto usable = rebuild();
    CHECK(usable && !*usable && aside_files().empty());

    // Not a database, with a journal beside it (sample bytes): both put aside.
    const std::string sample_bytes(4096, 'x');
    std::ofstream(database, std::ios::binary | std::ios::trunc) << sample_bytes;
    std::ofstream(layout.root / "veriarchive.db-wal", std::ios::binary) << "sample journal";
    const auto replaced = rebuild();
    CHECK(replaced && *replaced && (*replaced)->state == va::DatabaseState::NotADatabase && (*replaced)->moved_to);
    if (!replaced || !*replaced || !(*replaced)->moved_to) {
        return;
    }
    const auto moved = *(*replaced)->moved_to;
    const auto moved_name = va::path_to_utf8(moved.filename());
    CHECK(moved.parent_path() == layout.root && moved_name.starts_with("veriarchive.db.damaged-20") &&
          moved_name.ends_with("Z") && moved_name.find(':') == std::string::npos);
    CHECK(file_bytes(moved) == sample_bytes);
    CHECK(file_bytes(layout.root / va::path_from_utf8(moved_name + "-wal")) == "sample journal");
    // Every file put aside is named: the database and the journal written
    // beside it, and any other file SQLite kept beside it, under its name.
    const auto& set_aside = (*replaced)->set_aside;
    CHECK(std::ranges::count(set_aside, layout.root / va::path_from_utf8(moved_name + "-wal")) == 1 &&
          std::ranges::count(set_aside, moved) == 1 && std::ranges::all_of(set_aside, [&](const auto& file) {
              return va::path_to_utf8(file.filename()).starts_with(moved_name) && std::filesystem::exists(file);
          }));
    CHECK(!std::filesystem::exists(layout.root / "veriarchive.db-wal"));
    CHECK(va::database_state(layout) == va::DatabaseState::Unfilled);

    // Missing, with a journal left beside it (sample bytes): made anew, the
    // journal put aside, so the new database never takes it up.
    std::filesystem::remove(database);
    std::ofstream(layout.root / "veriarchive.db-journal", std::ios::binary) << "sample journal";
    const auto missing = rebuild();
    CHECK(missing && *missing && (*missing)->state == va::DatabaseState::Missing && !(*missing)->moved_to);
    CHECK(!std::filesystem::exists(layout.root / "veriarchive.db-journal"));
    CHECK(va::database_state(layout) == va::DatabaseState::Unfilled);
    const auto names = aside_files();
    const auto journal = std::ranges::find_if(names, [](const std::string& name) { return name.ends_with("-journal"); });
    CHECK(journal != names.end() &&
          file_bytes(layout.root / va::path_from_utf8(*journal)) == "sample journal");
    // The journal put aside is named, where no database put aside names it.
    if (journal != names.end() && missing && *missing) {
        CHECK(std::ranges::count((*missing)->set_aside, layout.root / va::path_from_utf8(*journal)) == 1);
    }

    // Another program's database: an error, and the file left as it was.
    std::filesystem::remove(database);
    run_sql(database, "CREATE TABLE sample(x);");
    const auto other_bytes = file_bytes(database);
    const auto other = rebuild();
    CHECK(!other && other.error().message == "unsupported repository schema");
    CHECK(file_bytes(database) == other_bytes && aside_files() == names);
}

// Decision 0036: the new database is made beside the one it replaces, as
// veriarchive.db.new with its mark, and moved into place last. A rebuild
// that stopped between moving the database aside and moving its new one into
// place (made so here: the new database moved back beside its place, sample)
// left the database missing: the new one is taken up, naming the database
// put aside before, and nothing more is put aside. A new database left beside
// a database that cannot be used (sample bytes), as by a rebuild that stopped
// before it moved anything, goes, and the database is replaced as any.
void a_new_database_left_beside_its_place_is_taken_up()
{
    TemporaryDirectory temporary("database-taken-up");
    auto context = sample_repository(temporary.path());
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    const auto database = layout.database();
    const auto made_beside = layout.root / "veriarchive.db.new";
    const auto aside_count = [&] {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(layout.root)) {
            // The database put aside, not the files SQLite keeps beside it.
            const auto name = va::path_to_utf8(entry.path().filename());
            count += name.starts_with("veriarchive.db.damaged-") && !name.ends_with("-wal") &&
                             !name.ends_with("-shm") && !name.ends_with("-journal")
                         ? 1
                         : 0;
        }
        return count;
    };
    const std::string sample_bytes(4096, 'x');
    std::ofstream(database, std::ios::binary | std::ios::trunc) << sample_bytes;
    auto first_context = *context;
    const auto first = va::open_repository_to_rebuild(first_context, test::today);
    CHECK(first && *first && (*first)->moved_to && !std::filesystem::exists(made_beside));
    if (!first || !*first || !(*first)->moved_to) {
        return;
    }
    std::filesystem::rename(database, made_beside);
    CHECK(va::database_state(layout) == va::DatabaseState::Missing);
    auto again_context = *context;
    const auto again = va::open_repository_to_rebuild(again_context, test::today);
    CHECK(again && *again && (*again)->state == va::DatabaseState::NotADatabase &&
          (*again)->moved_to == (*first)->moved_to);
    CHECK(aside_count() == 1 && !std::filesystem::exists(made_beside) &&
          va::database_state(layout) == va::DatabaseState::Unfilled);

    std::ofstream(made_beside, std::ios::binary) << "sample leftover";
    std::ofstream(database, std::ios::binary | std::ios::trunc) << sample_bytes;
    auto third_context = *context;
    const auto third = va::open_repository_to_rebuild(third_context, test::today);
    CHECK(third && *third && (*third)->moved_to && (*third)->moved_to != (*first)->moved_to);
    CHECK(aside_count() == 2 && !std::filesystem::exists(made_beside) &&
          va::database_state(layout) == va::DatabaseState::Unfilled);
    if (third && *third && (*third)->moved_to) {
        CHECK(file_bytes(*(*third)->moved_to) == sample_bytes);
    }
}

// Whether the repository's database has the table `name`.
bool has_table(const va::RepositoryLayout& layout, const char* name)
{
    sqlite3* database = nullptr;
    const auto file = layout.database().u8string();
    bool found = false;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?", -1,
                &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
            found = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1;
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return found;
}

// The first column of the first row of `sql` in the database file `file`, as
// text, read through a connection of its own; "-" when there is none or it
// cannot be read.
std::string first_value(const std::filesystem::path& file, const char* sql)
{
    sqlite3* database = nullptr;
    const auto name = file.u8string();
    std::string value = "-";
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(statement, 0);
            value = text ? reinterpret_cast<const char*>(text) : "";
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return value;
}

// The names of the files in `folder` that start with "veriarchive-schema-":
// the copies of the database kept before upgrades, and what is left of them.
// A copy is in WAL mode, as its database is, so SQLite makes its -wal and
// -shm files beside it whenever it is opened, as these tests open it: those
// are left out.
std::set<std::string> copies_in(const std::filesystem::path& folder)
{
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const auto name = va::path_to_utf8(entry.path().filename());
        if (name.starts_with("veriarchive-schema-") && !name.ends_with("-wal") && !name.ends_with("-shm")) {
            names.insert(name);
        }
    }
    return names;
}

// The repository's database as V1 left it (version 1, with V1's tables), or
// as a program of `version` left it, with one archive (sample data).
void lower_schema(const va::RepositoryLayout& layout, int version)
{
    run_sql(layout,
        ("DROP TABLE IF EXISTS verify_progress_chunks; UPDATE schema_version SET version = " +
            std::to_string(version) +
            "; DELETE FROM archives; INSERT INTO archives VALUES('c0ffee00-0000-4000-8000-000000000032',"
            "'ARCH-0001','2026-10-16T00:00:00Z',1,'REGISTERED','',NULL)")
            .c_str());
}

// Decisions 0032 and 0033: a new repository has schema version 3. One that V1
// created (version 1, whose tables are those of version 2), and one of
// version 2, are raised to version 3 when this version opens them, keeping
// their rows: version 3 adds verify_progress_chunks (decision 0033), and a V1
// program refuses the repository from then on. Before it raises the version,
// the open keeps a copy of the database as it is beside it, named for that
// version and the day it is opened on: the copy holds the version and the
// rows as they were, whole, and the open says where it is (decision 0032).
// Opened again, the repository needs no upgrade, and no copy is made. A
// version this program does not know is refused, and left as it is.
void an_older_repository_is_raised_to_schema_version_3()
{
    TemporaryDirectory temporary("schema-v1");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    CHECK(stored_schema_version(layout) == 3 && has_table(layout, "verify_progress_chunks"));
    CHECK(!context->schema_upgrade && copies_in(layout.root).empty());

    // As V1 left it, and as a version 2 program left it (sample data).
    for (const int older : {1, 2}) {
        lower_schema(layout, older);
        const auto unmigrated = va::validate_schema(layout);
        CHECK(!unmigrated && unmigrated.error().message == "unsupported repository schema");
        CHECK(va::open_repository(*context, test::today).has_value());
        CHECK(stored_schema_version(layout) == 3 && has_table(layout, "verify_progress_chunks"));
        CHECK(va::validate_schema(layout).has_value());
        CHECK(first_value(layout.database(), "SELECT label FROM archives") == "ARCH-0001");

        const auto copy =
            layout.root / ("veriarchive-schema-" + std::to_string(older) + "-before-upgrade-2026-10-17.db");
        const auto& upgrade = context->schema_upgrade;
        CHECK(upgrade && upgrade->from == older && upgrade->to == 3 && upgrade->copy == copy);
        CHECK(first_value(copy, "SELECT version FROM schema_version") == std::to_string(older));
        CHECK(first_value(copy, "SELECT label FROM archives") == "ARCH-0001");
        CHECK(first_value(copy, "SELECT count(*) FROM sqlite_master WHERE name = 'verify_progress_chunks'") == "0");
        CHECK(first_value(copy, "PRAGMA integrity_check") == "ok");
    }
    const std::set<std::string> both{
        "veriarchive-schema-1-before-upgrade-2026-10-17.db", "veriarchive-schema-2-before-upgrade-2026-10-17.db"};
    CHECK(copies_in(layout.root) == both);

    // Opened again: nothing more to do, and nothing more to copy.
    CHECK(va::open_repository(*context, test::today).has_value());
    CHECK(stored_schema_version(layout) == 3);
    CHECK(!context->schema_upgrade && copies_in(layout.root) == both);

    // A version 3 database without the table is incomplete, not raised.
    run_sql(layout, "DROP TABLE verify_progress_chunks");
    const auto incomplete = va::validate_schema(layout);
    CHECK(!incomplete && incomplete.error().message == "unsupported or incomplete repository schema");

    // A later program's repository, neither upgraded nor copied.
    run_sql(layout, "UPDATE schema_version SET version = 4");
    const auto later = va::open_repository(*context, test::today);
    CHECK(!later && later.error().message == "unsupported repository schema");
    CHECK(stored_schema_version(layout) == 4);
    CHECK(!context->schema_upgrade && copies_in(layout.root) == both);
}

// The copy kept before an upgrade never replaces a file (decision 0032): when
// its name is taken, as by a copy that was put back and upgraded again on the
// same day, it takes the next name with -2, -3, ... before .db, and the file
// that has the name stays as it is. What a copy cut short by a crash left
// under its name with ".partial" added is replaced, and the journal left
// beside it is gone too: SQLite deletes it, as it finds the new file empty.
void a_copy_before_an_upgrade_never_replaces_a_file()
{
    TemporaryDirectory temporary("schema-copy-names");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    lower_schema(layout, 1);
    const std::string kept = "SAMPLE: a file of this name that is not to be replaced (sample data)\n";
    const auto taken = layout.root / "veriarchive-schema-1-before-upgrade-2026-10-17.db";
    std::ofstream(taken, std::ios::binary) << kept;
    const auto second = layout.root / "veriarchive-schema-1-before-upgrade-2026-10-17-2.db";
    auto partial = second;
    partial += ".partial";
    auto partial_journal = partial;
    partial_journal += "-journal";
    std::ofstream(partial, std::ios::binary) << "SAMPLE: a copy cut short (sample data)\n";
    std::ofstream(partial_journal, std::ios::binary) << "SAMPLE: its journal (sample data)\n";

    CHECK(va::open_repository(*context, test::today).has_value());
    CHECK(context->schema_upgrade && context->schema_upgrade->copy == second);
    CHECK(stored_schema_version(layout) == 3);
    std::ifstream taken_file(taken, std::ios::binary);
    const std::string taken_now{std::istreambuf_iterator<char>(taken_file), std::istreambuf_iterator<char>()};
    CHECK(taken_now == kept);
    CHECK(first_value(second, "SELECT version FROM schema_version") == "1");
    CHECK(first_value(second, "SELECT label FROM archives") == "ARCH-0001");
    CHECK(copies_in(layout.root) == (std::set<std::string>{"veriarchive-schema-1-before-upgrade-2026-10-17.db",
                                        "veriarchive-schema-1-before-upgrade-2026-10-17-2.db"}));
}

// Without its copy, a database is not upgraded (decision 0032): when the copy
// cannot be made, the open fails, saying where the copy was to be and why it
// could not be made, and the repository stays at its version with its rows,
// for a program of that version; no copy is left. Once the copy can be made,
// the next open upgrades it.
void an_upgrade_without_its_copy_is_refused()
{
    TemporaryDirectory temporary("schema-no-copy");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    // An upgrade from version 2 first, which the context then tells of.
    lower_schema(layout, 2);
    CHECK(va::open_repository(*context, test::today).has_value() && context->schema_upgrade);
    lower_schema(layout, 1);
    // A folder where the copy is written (sample data): no file can be made there.
    const auto copy = layout.root / "veriarchive-schema-1-before-upgrade-2026-10-17.db";
    auto partial = copy;
    partial += ".partial";
    std::filesystem::create_directories(partial / "SAMPLE folder in the way");

    const auto refused = va::open_repository(*context, test::today);
    const auto expected = "cannot keep a copy of the repository database at " + va::path_to_utf8(copy) +
                          " before upgrading it from schema version 1 to 3, so the repository was not upgraded: "
                          "unable to open database file";
    CHECK(!refused && refused.error().message == expected);
    if (!refused && refused.error().message != expected) {
        std::cerr << "  refused with: " << refused.error().message << '\n';
    }
    CHECK(stored_schema_version(layout) == 1 && !has_table(layout, "verify_progress_chunks"));
    CHECK(first_value(layout.database(), "SELECT label FROM archives") == "ARCH-0001");
    CHECK(!context->schema_upgrade && !std::filesystem::exists(copy));

    std::filesystem::remove_all(partial);
    CHECK(va::open_repository(*context, test::today).has_value());
    CHECK(context->schema_upgrade && context->schema_upgrade->copy == copy);
    CHECK(stored_schema_version(layout) == 3 && first_value(copy, "SELECT version FROM schema_version") == "1");
}

// An upgrade that fails after its copy was made, as a database of version 2
// that has the table version 3 adds already, leaves the database at its
// version and removes the copy, which would be of a database that was not
// upgraded (decision 0032).
void an_upgrade_that_fails_keeps_no_copy()
{
    TemporaryDirectory temporary("schema-upgrade-fails");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    run_sql(layout, "UPDATE schema_version SET version = 2");
    const auto failed = va::open_repository(*context, test::today);
    CHECK(!failed && failed.error().message == "cannot upgrade the repository database to schema version 3: cannot "
                                               "execute SQL: table verify_progress_chunks already exists");
    CHECK(stored_schema_version(layout) == 2 && copies_in(layout.root).empty() && !context->schema_upgrade);
}

// Another process's connection to the repository's database, which holds the
// write lock from a thread of its own while `work` runs (sample data): as an
// open of the same repository in another process holds it while it copies
// the database to upgrade it. `work` gets the connection, in its write
// transaction, and ends it.
class LockHolder {
public:
    LockHolder(const va::RepositoryLayout& layout, std::function<void(sqlite3*)> work)
    {
        thread_ = std::thread([this, name = layout.database().u8string(), work = std::move(work)] {
            sqlite3* connection = nullptr;
            CHECK(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &connection) == SQLITE_OK);
            CHECK(sqlite3_exec(connection, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
            held_.release();
            work(connection);
            sqlite3_close(connection);
        });
        CHECK(held_.try_acquire_for(std::chrono::seconds(60)));
    }

    ~LockHolder() { thread_.join(); }

    LockHolder(const LockHolder&) = delete;
    LockHolder& operator=(const LockHolder&) = delete;

private:
    std::binary_semaphore held_{0};
    std::thread thread_;
};

// A repository whose database V1 left (version 1), for the tests of the lock.
std::optional<va::AppContext> repository_of_version_1(const TemporaryDirectory& temporary)
{
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return std::nullopt;
    }
    lower_schema(context->repo, 1);
    return std::move(*context);
}

// Ten tries of 100 ms each a second, and a patience the tests never reach.
constexpr va::UpgradeLockWait short_tries{std::chrono::milliseconds(100), std::chrono::seconds(30)};

// The copy is made in the transaction that upgrades the database, under its
// write lock (decision 0032), so that it is the very database upgraded, which
// no other process changes meanwhile. While another connection holds that
// lock, longer than one try of a statement's wait, the open waits for it, and
// once the lock is free, copies and upgrades.
void an_upgrade_waits_for_the_write_lock()
{
    TemporaryDirectory temporary("schema-copy-lock");
    auto context = repository_of_version_1(temporary);
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    std::chrono::steady_clock::duration waited{};
    std::expected<void, va::IoError> opened;
    {
        const LockHolder holder(layout, [](sqlite3* connection) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            CHECK(sqlite3_exec(connection, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
        });
        const auto started = std::chrono::steady_clock::now();
        opened = va::open_repository(*context, test::today, short_tries);
        waited = std::chrono::steady_clock::now() - started;
    }
    CHECK(opened.has_value() && waited >= std::chrono::milliseconds(500));
    if (!opened) {
        std::cerr << "  failed: " << opened.error().message << '\n';
    }
    CHECK(context->schema_upgrade && stored_schema_version(layout) == 3);
    CHECK(copies_in(layout.root) == std::set<std::string>{"veriarchive-schema-1-before-upgrade-2026-10-17.db"});
}

// While another process upgrades the database, an open of it waits, and once
// that process has upgraded it, opens it without upgrading it again: no copy
// of its own and nothing to tell. It reads the version between its tries, so
// it goes on also while another connection takes the lock next, as the same
// one does here at once.
void an_open_waits_for_another_upgrade()
{
    TemporaryDirectory temporary("schema-other-upgrade");
    auto context = repository_of_version_1(temporary);
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    std::binary_semaphore opened_meanwhile{0};
    std::chrono::steady_clock::duration waited{};
    std::expected<void, va::IoError> opened;
    {
        const LockHolder holder(layout, [&](sqlite3* connection) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // The upgrade of version 1 to 3, as the other process makes it.
            CHECK(sqlite3_exec(connection,
                      "CREATE TABLE verify_progress_chunks (run_id TEXT NOT NULL,path TEXT NOT NULL,raw_hex TEXT NOT "
                      "NULL DEFAULT '',stream TEXT NOT NULL DEFAULT '',idx INTEGER NOT NULL,b3 TEXT NOT NULL,PRIMARY "
                      "KEY(run_id,path,raw_hex,stream,idx)) WITHOUT ROWID; UPDATE schema_version SET version = 3; "
                      "COMMIT; BEGIN IMMEDIATE",
                      nullptr, nullptr, nullptr) == SQLITE_OK);
            // The lock taken again, until the open is done, or 10 seconds.
            (void)opened_meanwhile.try_acquire_for(std::chrono::seconds(10));
            CHECK(sqlite3_exec(connection, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
        });
        const auto started = std::chrono::steady_clock::now();
        opened = va::open_repository(*context, test::today, short_tries);
        waited = std::chrono::steady_clock::now() - started;
        opened_meanwhile.release();
    }
    CHECK(opened.has_value() && waited < std::chrono::seconds(5));
    if (!opened) {
        std::cerr << "  failed: " << opened.error().message << '\n';
    }
    CHECK(!context->schema_upgrade && copies_in(layout.root).empty() && stored_schema_version(layout) == 3);
}

// When the lock stays taken longer than the open's patience, the open fails
// with SQLite's cause, without a copy and without the upgrade, and an open
// once the lock is free copies and upgrades. The open's own patience is 2
// minutes, as a record waits for the lock (decision 0023), in tries of 5
// seconds, as a statement waits.
void an_upgrade_gives_up_after_its_patience()
{
    CHECK(va::UpgradeLockWait{}.each_try == std::chrono::seconds(5) &&
          va::UpgradeLockWait{}.patience == std::chrono::minutes(2));
    TemporaryDirectory temporary("schema-lock-patience");
    auto context = repository_of_version_1(temporary);
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    {
        const LockHolder holder(layout, [](sqlite3* connection) {
            std::this_thread::sleep_for(std::chrono::seconds(4));
            CHECK(sqlite3_exec(connection, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
        });
        const auto started = std::chrono::steady_clock::now();
        const auto locked = va::open_repository(
            *context, test::today, {std::chrono::milliseconds(100), std::chrono::milliseconds(500)});
        const auto waited = std::chrono::steady_clock::now() - started;
        CHECK(!locked && locked.error().message == "cannot upgrade the repository database to schema version 3: "
                                                   "cannot execute SQL: database is locked");
        if (locked || locked.error().message.find("database is locked") == std::string::npos) {
            std::cerr << "  opened while the lock was held: " << (locked ? "upgraded" : locked.error().message) << '\n';
        }
        CHECK(waited < std::chrono::seconds(3));
        CHECK(copies_in(layout.root).empty() && stored_schema_version(layout) == 1 && !context->schema_upgrade);
    }
    CHECK(va::open_repository(*context, test::today, short_tries).has_value());
    CHECK(copies_in(layout.root) == std::set<std::string>{"veriarchive-schema-1-before-upgrade-2026-10-17.db"});
}

// Decisions 0032 and 0036: a database of V1's schema (version 1) is judged by
// the tables of its own version, and only one that can be used is upgraded. A
// V1 database without one of its tables is damaged, and one that V1's rebuild
// made again and did not fill is unfilled: opening the repository names the
// rebuild for each and leaves it at version 1, keeping no copy. Opened for
// `repo rebuild-index`, a usable V1 database is upgraded in place, its rows
// kept and nothing put aside, after the copy of it from before the upgrade is
// kept, which the open names (context.schema_upgrade); an unfilled one is
// upgraded with its mark, which still names the database put aside then, and
// its copy kept too; a damaged one is put aside and made again at this
// version, with no copy; a later version's is refused and left as it is
// (sample data).
void an_older_schema_is_upgraded_only_where_it_can_be_used()
{
    TemporaryDirectory temporary("schema-older");
    auto context = sample_repository(temporary.path());
    if (!context) {
        return;
    }
    const auto& layout = context->repo;
    const auto database = layout.database();
    const auto database_text = va::path_to_utf8(database);
    const auto current = file_bytes(database);
    // As V1 left it: without the table that version 3 adds.
    run_sql(layout,
        "DROP TABLE verify_progress_chunks; UPDATE schema_version SET version = 1; "
        "INSERT INTO archives VALUES('c0ffee00-0000-4000-8000-000000000036','ARCH-0001','2026-10-27T00:00:00Z',1,"
        "'REGISTERED','',NULL)");
    const auto version_1 = file_bytes(database);
    const auto label_kept = [&] {
        auto store = va::ArchiveStore::open_read_only(layout);
        if (!store) {
            return false;
        }
        const auto archive = store->find_archive("c0ffee00-0000-4000-8000-000000000036");
        return archive && *archive && (*archive)->label == "ARCH-0001";
    };
    // The copies of the database kept before an upgrade, by name.
    const auto copies = [&] {
        std::set<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(layout.root)) {
            const auto name = va::path_to_utf8(entry.path().filename());
            if (name.starts_with("veriarchive-schema-")) {
                names.insert(name);
            }
        }
        return names;
    };
    // The upgrade that `opened` made, from version 1, and its copy, a
    // database of version 1, which is then removed.
    const auto upgraded_with_copy = [&](const va::AppContext& opened) {
        const auto& upgrade = opened.schema_upgrade;
        CHECK(upgrade && upgrade->from == 1 && upgrade->to == 3 &&
              copies() == std::set<std::string>{"veriarchive-schema-1-before-upgrade-2026-10-17.db"});
        if (upgrade) {
            CHECK(upgrade->copy == layout.root / "veriarchive-schema-1-before-upgrade-2026-10-17.db");
            va::RepositoryLayout copy_layout;
            copy_layout.root = layout.root / "copy-SAMPLE";
            std::filesystem::create_directories(copy_layout.root);
            std::filesystem::copy_file(upgrade->copy, copy_layout.database());
            CHECK(stored_schema_version(copy_layout) == 1);
            std::filesystem::remove_all(copy_layout.root);
            std::filesystem::remove(upgrade->copy);
        }
    };
    CHECK(va::database_state(layout) == va::DatabaseState::Usable);

    // Version 1 without one of its tables: damaged, not upgraded.
    run_sql(layout, "DROP TABLE verify_progress;");
    CHECK(va::database_state(layout) == va::DatabaseState::Damaged);
    {
        auto reopened = *context;
        const auto opened = va::open_repository(reopened, test::today);
        CHECK(!opened && opened.error().message == "the repository's database " + database_text +
                                                       " is damaged; make it again from the manifests with `repo "
                                                       "rebuild-index`");
        CHECK(stored_schema_version(layout) == 1 && !reopened.schema_upgrade && copies().empty());
    }

    // Version 1 made again by a rebuild that did not finish: unfilled.
    std::ofstream(database, std::ios::binary | std::ios::trunc) << version_1;
    run_sql(layout, "INSERT INTO settings VALUES ('rebuild:replaced-database', "
                    "'{\"state\":\"damaged\",\"moved_to\":\"veriarchive.db.damaged-SAMPLE\",\"set_aside\":[]}');");
    CHECK(va::database_state(layout) == va::DatabaseState::Unfilled);
    {
        auto reopened = *context;
        CHECK(!va::open_repository(reopened, test::today));
        CHECK(stored_schema_version(layout) == 1 && copies().empty());
        auto rebuilding = *context;
        const auto marked = va::open_repository_to_rebuild(rebuilding, test::today);
        CHECK(marked && *marked && (*marked)->moved_to == layout.root / "veriarchive.db.damaged-SAMPLE");
        CHECK(stored_schema_version(layout) == 3 && va::database_state(layout) == va::DatabaseState::Unfilled);
        upgraded_with_copy(rebuilding);
    }

    // A usable version 1 database, opened for the rebuild: upgraded in place.
    std::ofstream(database, std::ios::binary | std::ios::trunc) << version_1;
    {
        auto rebuilding = *context;
        const auto opened = va::open_repository_to_rebuild(rebuilding, test::today);
        CHECK(opened && !*opened);
        CHECK(stored_schema_version(layout) == 3 && va::database_state(layout) == va::DatabaseState::Usable);
        CHECK(label_kept());
        upgraded_with_copy(rebuilding);
    }

    // Version 1 without one of its tables, opened for the rebuild: put aside
    // as it was, and made again at this version.
    std::ofstream(database, std::ios::binary | std::ios::trunc) << version_1;
    run_sql(layout, "DROP TABLE verify_items;");
    const auto damaged = file_bytes(database);
    {
        auto rebuilding = *context;
        const auto replaced = va::open_repository_to_rebuild(rebuilding, test::today);
        CHECK(replaced && *replaced && (*replaced)->state == va::DatabaseState::Damaged && (*replaced)->moved_to);
        if (replaced && *replaced && (*replaced)->moved_to) {
            CHECK(file_bytes(*(*replaced)->moved_to) == damaged);
        }
        CHECK(stored_schema_version(layout) == 3 && va::database_state(layout) == va::DatabaseState::Unfilled);
        CHECK(!rebuilding.schema_upgrade && copies().empty());
    }

    // A later version's database: refused by the rebuild too, unchanged.
    std::ofstream(database, std::ios::binary | std::ios::trunc) << current;
    run_sql(layout, "UPDATE schema_version SET version = 4;");
    const auto later = file_bytes(database);
    {
        auto rebuilding = *context;
        const auto refused = va::open_repository_to_rebuild(rebuilding, test::today);
        CHECK(!refused && refused.error().message == "unsupported repository schema");
        CHECK(file_bytes(database) == later && copies().empty());
    }
}

// A repository at this version is opened without the write lock, which an
// operation of another process may hold for a long time (the record of a
// large archive): opening it never waits for that operation.
void opening_a_current_repository_takes_no_write_lock()
{
    TemporaryDirectory temporary("schema-lock");
    auto options = make_options(temporary.path());
    auto context = va::resolve_repository(options, always_writable_local);
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer(), test::today));
    if (!context) {
        return;
    }
    sqlite3* holder = nullptr;
    const auto name = context->repo.database().u8string();
    CHECK(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &holder) == SQLITE_OK);
    CHECK(sqlite3_exec(holder, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto started = std::chrono::steady_clock::now();
    CHECK(va::open_repository(*context, test::today).has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(sqlite3_exec(holder, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(holder);
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
    elevated_warning_holds_also_without_a_repository();
    database_state_tells_what_cannot_be_used();
    rebuild_makes_a_database_that_cannot_be_used_again();
    a_new_database_left_beside_its_place_is_taken_up();
    an_older_repository_is_raised_to_schema_version_3();
    an_older_schema_is_upgraded_only_where_it_can_be_used();
    a_copy_before_an_upgrade_never_replaces_a_file();
    an_upgrade_without_its_copy_is_refused();
    an_upgrade_that_fails_keeps_no_copy();
    an_upgrade_waits_for_the_write_lock();
    an_open_waits_for_another_upgrade();
    an_upgrade_gives_up_after_its_patience();
    opening_a_current_repository_takes_no_write_lock();
}