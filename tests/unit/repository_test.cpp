#include "test_support.h"

#include "core/repository/repository.h"
#include "app/session.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/write_file.h"
#include "platform/file_writer.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
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
    const auto initialized = va::initialise_repository(*context, *va::platform::make_file_writer());
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
    CHECK(created && va::initialise_repository(*created, *va::platform::make_file_writer()));
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
    CHECK(context && va::initialise_repository(*context, *va::platform::make_file_writer()));
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
        const auto opened = va::open_repository(reopened);
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
        const auto opened = va::open_repository(reopened);
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
    run_sql(database, "CREATE TABLE schema_version (version INTEGER NOT NULL); INSERT INTO schema_version VALUES (2);");
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
        auto made = va::open_repository_to_rebuild(reopened);
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
    const auto first = va::open_repository_to_rebuild(first_context);
    CHECK(first && *first && (*first)->moved_to && !std::filesystem::exists(made_beside));
    if (!first || !*first || !(*first)->moved_to) {
        return;
    }
    std::filesystem::rename(database, made_beside);
    CHECK(va::database_state(layout) == va::DatabaseState::Missing);
    auto again_context = *context;
    const auto again = va::open_repository_to_rebuild(again_context);
    CHECK(again && *again && (*again)->state == va::DatabaseState::NotADatabase &&
          (*again)->moved_to == (*first)->moved_to);
    CHECK(aside_count() == 1 && !std::filesystem::exists(made_beside) &&
          va::database_state(layout) == va::DatabaseState::Unfilled);

    std::ofstream(made_beside, std::ios::binary) << "sample leftover";
    std::ofstream(database, std::ios::binary | std::ios::trunc) << sample_bytes;
    auto third_context = *context;
    const auto third = va::open_repository_to_rebuild(third_context);
    CHECK(third && *third && (*third)->moved_to && (*third)->moved_to != (*first)->moved_to);
    CHECK(aside_count() == 2 && !std::filesystem::exists(made_beside) &&
          va::database_state(layout) == va::DatabaseState::Unfilled);
    if (third && *third && (*third)->moved_to) {
        CHECK(file_bytes(*(*third)->moved_to) == sample_bytes);
    }
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
}