#include "core/repository/repository.h"

#include "common/canonical_json.h"
#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/repository/archive_store.h"
#include "core/repository/sqlite_raii.h"
#include "core/write_file.h"
#include "schema_sql.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sqlite3.h>
#include <system_error>

namespace va {
namespace {

// The schema version this program creates and opens (§B8). Version 2 has the
// tables of version 1 (V1), which created chunks and
// archive_revisions.merkle_root and left them empty; version 2 fills them
// (decision 0032). Version 3 adds verify_progress_chunks, the chunk hashes
// that a FULL verify adding them keeps with its checkpoints (decision 0033).
constexpr int schema_version = 3;

// R-92: forward-only migrations, each keyed by the version it upgrades and run
// in the transaction that raises the version by one.
struct Migration {
    int from;
    const char* sql;
};
// 1 -> 2 adds nothing: every table and column is there. The version is raised
// so that a V1 program, which neither writes nor removes chunk rows and whose
// relocation drops chunk lines (decision 0031), refuses the repository from
// then on ("unsupported repository schema"). 2 -> 3 adds a table, as V1.1
// may (§B8), to which no other table refers.
constexpr Migration migrations[] = {
    {1, ""},
    {2, "CREATE TABLE verify_progress_chunks (run_id TEXT NOT NULL,path TEXT NOT NULL,raw_hex TEXT NOT NULL DEFAULT "
        "'',stream TEXT NOT NULL DEFAULT '',idx INTEGER NOT NULL,b3 TEXT NOT NULL,PRIMARY KEY(run_id,path,raw_hex,"
        "stream,idx)) WITHOUT ROWID;"},
};

// The tables of §B8, each with the schema version that first has it. A
// database of a version holds all of that version's tables: this program, and
// V1 before it, create all of them in one transaction (initialise_database),
// and a migration adds its tables in the transaction that raises the version.
struct RequiredTable {
    const char* name;
    int since;
};
constexpr RequiredTable required_tables[] = {
    {"archives", 1}, {"archive_revisions", 1}, {"media_evidence", 1}, {"files", 1}, {"chunks", 1},
    {"verify_runs", 1}, {"verify_items", 1}, {"verify_progress", 1}, {"verify_progress_chunks", 3},
    {"history_checkpoints", 1}, {"trusted_keys", 1}, {"settings", 1},
};

// No SQLite result code: database_state tells SQLite's own codes apart
// (SQLITE_CORRUPT is 11, the exit code these errors once carried).
IoError repository_error(std::string message)
{
    return {IoErrorKind::Other, 0, std::move(message)};
}

// std::filesystem::exists without exceptions: the throwing overload would end
// the program on, for example, access denied. A path that cannot be checked
// counts as missing; the later read or write then reports the real error.
bool path_exists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

} // namespace

std::filesystem::path config_file_path(const RepositoryOptions& options)
{
    if (options.explicit_config) {
        return *options.explicit_config;
    }

    const auto portable_config = options.executable_dir / "config.json";
    if (path_exists(portable_config)) {
        return portable_config;
    }

    return options.user_config_dir / "config.json";
}

namespace {

std::optional<std::string> read_repository_uuid(const std::filesystem::path& root)
{
    std::ifstream input(root / "repository.json", std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    const std::string json{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_object() || !document.contains("format") ||
        document["format"] != "veriarchive-repo" ||
        !document.contains("version") || document["version"] != 1 ||
        !document.contains("repository_uuid") || !document["repository_uuid"].is_string()) {
        return std::nullopt;
    }
    const auto uuid = document["repository_uuid"].get<std::string>();
    if (!is_canonical_uuid(uuid)) {
        return std::nullopt;
    }
    return uuid;
}

std::expected<void, IoError> write_repository_metadata(AppContext& context, IFileWriter& writer)
{
    context.repository_uuid = generate_uuid();
    const auto contents = "{\"format\":\"veriarchive-repo\",\"version\":1"
                          ",\"repository_uuid\":" +
                          canonical_json::quote(context.repository_uuid) +
                          ",\"created\":" + canonical_json::quote(current_timestamp()) + "}\n";
    if (auto written = write_file_atomically(writer, context.repo.root / "repository.json", contents);
        !written) {
        return std::unexpected(repository_error("cannot write repository.json: " +
                                                written.error().message));
    }
    return {};
}

std::expected<void, IoError> create_repository_directories(const RepositoryLayout& layout)
{
    const std::array directories{
        layout.root,
        layout.manifests(),
        layout.keys(),
        layout.logs(),
        layout.cache(),
    };

    for (const auto& directory : directories) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return std::unexpected(repository_error(
                "cannot create repository directory: " + error.message()));
        }
    }
    return {};
}

std::expected<bool, IoError> schema_exists(
    const repository_detail::SqliteDatabase& database)
{
    auto statement = database.prepare(
        "SELECT count(*) FROM sqlite_master "
        "WHERE type='table' AND name='schema_version'");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    if (sqlite3_step(statement->get()) != SQLITE_ROW) {
        return std::unexpected(repository_error("cannot inspect repository schema"));
    }
    return sqlite3_column_int(statement->get(), 0) > 0;
}

// Creates the schema in one transaction where the database has none: a crash
// leaves either no schema or the complete schema. In WAL mode (R-91), but for
// a database made again (decision 0036), which is given `mark`, written in
// that transaction (replaced_database_key): it is made beside the database it
// replaces and closed before it is moved into place, so it is made in the
// rollback-journal mode, whose commit leaves nothing in a -wal file that would
// not move with it, and put in WAL mode in its place.
std::expected<void, IoError> initialise_database(
    const std::filesystem::path& file, const std::optional<std::string>& mark = std::nullopt)
{
    auto database = repository_detail::SqliteDatabase::open(file);
    if (!database) {
        return std::unexpected(database.error());
    }

    if (!mark) {
        if (auto result = database->execute(
                "PRAGMA journal_mode=WAL;"
                "PRAGMA synchronous=NORMAL;");
            !result) {
            return result;
        }
    }

    auto exists = schema_exists(*database);
    if (!exists) {
        return std::unexpected(exists.error());
    }
    if (*exists) {
        return {};
    }
    if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun) {
        return begun;
    }
    const auto created = [&]() -> std::expected<void, IoError> {
        if (auto made = database->execute(schema_sql); !made) {
            return made;
        }
        if (!mark) {
            return {};
        }
        auto statement = database->prepare("INSERT INTO settings(key, value) VALUES(?, ?)");
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return statement->bind(1, replaced_database_key).bind(2, *mark).run();
    }();
    if (!created) {
        (void)database->execute("ROLLBACK;");
        return created;
    }
    return database->execute("COMMIT;");
}

// The value written to config.repository when a repository is pinned. A
// repository inside the config file's folder is stored relative to that
// folder ("data"), so a portable folder that is moved or copied keeps finding
// its own repository (decision 0019). Anything else stays absolute.
// The check is lexical: a path spelled differently (another letter case on
// Windows, a symbolic link) counts as outside, which keeps the old, absolute
// pin rather than guessing.
std::filesystem::path pin_value(const std::filesystem::path& root,
    const std::filesystem::path& config_folder)
{
    const auto normal_root = root.lexically_normal();
    const auto relative = normal_root.lexically_relative(config_folder.lexically_normal());
    // Empty: nothing in common (another drive letter). ".": the folder itself.
    if (relative.empty() || relative == ".") {
        return normal_root;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return normal_root; // outside the folder
        }
    }
    // Forward slashes ("repositories/main"), which Windows and Linux both read,
    // so one portable folder can serve the builds for both.
    return std::filesystem::path(relative.generic_u8string());
}

// The version in schema_version, or nothing when it cannot be read (a
// database without VeriArchive's tables, a damaged or locked one), which
// validate_schema reports in its own words.
std::optional<int> read_schema_version(const repository_detail::SqliteDatabase& database)
{
    auto version = database.prepare("SELECT version FROM schema_version");
    if (!version) {
        return std::nullopt;
    }
    const auto row = version->step();
    if (!row || !*row) {
        return std::nullopt;
    }
    return sqlite3_column_int(version->get(), 0);
}

// The migration that upgrades a database of schema version `from`, if any.
const Migration* migration_from(int from)
{
    const auto* found = std::find_if(
        std::begin(migrations), std::end(migrations), [&](const Migration& step) { return step.from == from; });
    return found == std::end(migrations) ? nullptr : found;
}

// Whether migrations lead from `version` to schema_version, one step at a time.
bool upgradable(int version)
{
    for (int at = version; at < schema_version; ++at) {
        if (!migration_from(at)) {
            return false;
        }
    }
    return version < schema_version;
}

// Keeps a copy of the database of schema version `from`, beside it in
// RepositoryRoot, before it is upgraded (decision 0032): the verification
// history lives only in the database, and a program of an earlier version
// cannot open it once it is upgraded. Called while the transaction that
// upgrades holds the write lock and has written nothing yet, so a connection
// of its own reads the very database to be upgraded (the backup API reads no
// connection in a write transaction). Named for the version and the day, so
// that anyone sees what it is: veriarchive-schema-1-before-upgrade-2026-10-17.db,
// or -2, -3, ... before .db when that name is taken, as a file of that name
// is never replaced. The copy is written under the name with ".partial"
// added and renamed once its own commit has synced it, so that a copy cut
// short by a crash never has the name of a whole one; a leftover ".partial"
// of that name is replaced, as only an attempt that holds the write lock
// writes one. Returns where the copy is.
std::expected<std::filesystem::path, IoError> keep_copy_before_upgrade(
    const RepositoryLayout& layout, int from, std::chrono::year_month_day today)
{
    const auto stem = std::format("veriarchive-schema-{}-before-upgrade-{:%F}", from, today);
    auto copy = layout.root / (stem + ".db");
    for (int number = 2; path_exists(copy); ++number) {
        copy = layout.root / std::format("{}-{}.db", stem, number);
    }
    const auto failed = [&](const std::string& cause) {
        return std::unexpected(repository_error(
            "cannot keep a copy of the repository database at " + path_to_utf8(copy) +
            " before upgrading it from schema version " + std::to_string(from) + " to " +
            std::to_string(schema_version) + ", so the repository was not upgraded: " + cause));
    };
    auto partial = copy;
    partial += ".partial";
    auto partial_journal = partial;
    partial_journal += "-journal";
    const auto remove_partial = [&] {
        std::error_code ignored;
        std::filesystem::remove(partial_journal, ignored);
        std::filesystem::remove(partial, ignored);
    };
    remove_partial();
    auto reader = repository_detail::SqliteDatabase::open(layout.database(), true);
    if (!reader) {
        return failed(reader.error().message);
    }
    if (auto copied = reader->copy_to(partial); !copied) {
        remove_partial();
        return failed(copied.error().message);
    }
    // The name was free a moment ago, and only an attempt that holds the
    // write lock gives one to a copy.
    std::error_code error;
    std::filesystem::rename(partial, copy, error);
    if (error) {
        remove_partial();
        return failed(error.message());
    }
    return copy;
}

// Brings the database of a repository of an older schema version to
// schema_version (R-92) in one write transaction, so that a crash leaves the
// old version or the new one. The version is read first without the write
// lock, which an operation of another process may hold for long: a
// repository at this version, at a later one, or one that cannot be read is
// left as it is, for validate_schema to judge. Before it changes anything,
// the transaction keeps a copy of the database as it is (keep_copy_before_upgrade);
// when the copy cannot be made, nothing is upgraded. Returns the upgrade, or
// nothing when there was none.
std::expected<std::optional<SchemaUpgrade>, IoError> migrate_schema(
    const RepositoryLayout& layout, std::chrono::year_month_day today)
{
    {
        auto database = repository_detail::SqliteDatabase::open(layout.database(), true);
        if (!database) {
            return std::nullopt;
        }
        const auto version = read_schema_version(*database);
        if (!version || *version >= schema_version) {
            return std::nullopt;
        }
    }
    auto database = repository_detail::SqliteDatabase::open(layout.database());
    if (!database) {
        return std::unexpected(database.error());
    }
    const auto failed = [](const IoError& error) {
        return std::unexpected(repository_error("cannot upgrade the repository database to schema version " +
                                                std::to_string(schema_version) + ": " + error.message));
    };
    if (auto begun = database->execute("PRAGMA busy_timeout=5000; BEGIN IMMEDIATE;"); !begun) {
        return failed(begun.error());
    }
    // Again under the lock: another process may have upgraded it meanwhile.
    const auto version = read_schema_version(*database);
    if (!version) {
        (void)database->execute("ROLLBACK;");
        return failed(repository_error("cannot inspect repository schema"));
    }
    if (!upgradable(*version)) {
        // Upgraded meanwhile, or a version no migration starts from, which
        // validate_schema refuses.
        (void)database->execute("ROLLBACK;");
        return std::nullopt;
    }
    // The database as this transaction found it is the one it upgrades: the
    // write lock keeps every other process from changing it meanwhile.
    auto copy = keep_copy_before_upgrade(layout, *version, today);
    if (!copy) {
        (void)database->execute("ROLLBACK;");
        return std::unexpected(copy.error());
    }
    const auto upgraded = [&]() -> std::expected<void, IoError> {
        for (int at = *version; at < schema_version; ++at) {
            const auto* migration = migration_from(at);
            if (*migration->sql != '\0') {
                if (auto migrated = database->execute(migration->sql); !migrated) {
                    return migrated;
                }
            }
            if (auto raised = database->execute("UPDATE schema_version SET version = " + std::to_string(at + 1));
                !raised) {
                return raised;
            }
        }
        return database->execute("COMMIT;");
    }();
    if (!upgraded) {
        (void)database->execute("ROLLBACK;");
        // Not upgraded: the copy is of a database that stays as it was.
        std::error_code ignored;
        std::filesystem::remove(*copy, ignored);
        return failed(upgraded.error());
    }
    return SchemaUpgrade{*version, schema_version, *copy};
}

std::expected<bool, IoError> table_exists(
    const repository_detail::SqliteDatabase& database,
    const char* table)
{
    auto statement = database.prepare(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
    if (!statement) {
        return std::unexpected(statement.error());
    }

    sqlite3_bind_text(statement->get(), 1, table, -1, SQLITE_STATIC);
    if (sqlite3_step(statement->get()) != SQLITE_ROW) {
        return std::unexpected(repository_error("cannot inspect repository tables"));
    }
    return sqlite3_column_int(statement->get(), 0) == 1;
}

} // namespace

std::expected<AppContext, IoError> resolve_repository(
    const RepositoryOptions& options,
    const RepositoryProbes& probes)
{
    AppContext context;
    context.executable_dir = options.executable_dir;
    context.user_config_dir = options.user_config_dir;
    context.config_file = config_file_path(options);

    if (context.config_file.is_relative()) {
        context.config_file = options.executable_dir / context.config_file;
    }
    const auto config = read_config(context.config_file);
    if (!config) {
        return std::unexpected(config.error());
    }
    if (*config) {
        context.config = **config;
    } else if (options.explicit_config) {
        return std::unexpected(repository_error("Explicit config does not exist"));
    }

    bool first_run = false;
    if (options.explicit_repository) {
        context.repo.root = *options.explicit_repository;
        context.repo_selected_by = "explicit";
    } else if (options.environment_repository) {
        context.repo.root = *options.environment_repository;
        context.repo_selected_by = "explicit";
    } else if (context.config.repository && !context.config.repository->empty()) {
        // A relative pin belongs to the config file that holds it: it is
        // resolved against that file's folder (decision 0019), never against
        // the current directory (R-84). context.config keeps the stored value,
        // so later writes of the config do not turn it into an absolute path.
        const auto& pin = *context.config.repository;
        context.repo.root = pin.is_relative() ? context.config_file.parent_path() / pin : pin;
        context.repo_selected_by = "pinned";
    } else {
        if (!options.initialise) {
            return std::unexpected(repository_error("No repository configured; run repo init first"));
        }
        first_run = true;
        const auto portable_repository = options.executable_dir / "data";
        if (probes.writable(portable_repository)) {
            context.repo.root = portable_repository;
            context.repo_selected_by = "first-run:exe-dir";
        } else {
            context.repo.root = options.user_home / "VeriArchive";
            context.repo_selected_by = "first-run:home";
            if (!probes.writable(context.repo.root)) {
                return std::unexpected(repository_error("no writable repository location"));
            }
        }
    }

    // A relative --repository or VERIARCHIVE_REPOSITORY is taken from the
    // executable's folder, never from the current directory (R-84).
    if (context.repo.root.is_relative()) {
        context.repo.root = options.executable_dir / context.repo.root;
    }
    context.repo.root = context.repo.root.lexically_normal();
    // `repo init --repository P` may create a repository at P (§B16); every
    // other use of an explicit or pinned location requires an existing one.
    const bool explicit_init = options.initialise && context.repo_selected_by == "explicit";
    const bool exists = read_repository_uuid(context.repo.root).has_value();
    if (!first_run && !explicit_init && !exists) {
        // Named with its path, which the operator may have to attach (an
        // external disk) or correct, and where it was named (decision 0037).
        const auto root = path_to_utf8(context.repo.root);
        if (context.repo_selected_by == "pinned") {
            return std::unexpected(repository_error("the configured repository " + root +
                                                    " is unavailable or is not a VeriArchive repository (the config "
                                                    "file " + path_to_utf8(context.config_file) + " names it)"));
        }
        if (!options.explicit_repository) {
            return std::unexpected(repository_error("VERIARCHIVE_REPOSITORY names the repository " + root +
                                                    ", which is unavailable or is not a VeriArchive repository"));
        }
        return std::unexpected(
            repository_error("the repository " + root + " is unavailable or is not a VeriArchive repository"));
    }
    if (explicit_init && !exists && path_exists(context.repo.root / "repository.json")) {
        return std::unexpected(repository_error("not a VeriArchive repository: invalid repository.json"));
    }
    if (explicit_init && !exists && !probes.writable(context.repo.root)) {
        return std::unexpected(repository_error("cannot create a repository at the given path"));
    }

    if (probes.network_filesystem(context.repo.root) &&
        !context.config.allow_network_repository) {
        return std::unexpected(repository_error(
            "SQLite on a network filesystem is unsafe (locking, WAL); "
            "set allow_network_repository or move the repository"));
    }

    // Pin the new repository, unless an explicit init would replace an existing pin.
    // The config file is chosen first: the pin is written relative to its folder.
    if (first_run || (explicit_init && !context.config.repository)) {
        if (!options.explicit_config && probes.writable(options.executable_dir)) {
            context.config_file = options.executable_dir / "config.json";
        }
        context.config.repository = pin_value(context.repo.root, context.config_file.parent_path());
        context.pins_repository = true;
    }
    return context;
}

namespace {

// The last step of an init (decision 0011): the pin, and its record of the
// repository's uuid (R-83 step 5), written into the config file. Only an init
// that pins the repository writes it: `repo init --repository P` beside an
// existing pin leaves that pin and its record as they are (decision 0037).
std::expected<void, IoError> write_pin(AppContext& context, IFileWriter& writer)
{
    if (!context.pins_repository) {
        return {};
    }
    context.config.last_repository_uuid = context.repository_uuid;
    std::error_code error;
    std::filesystem::create_directories(context.config_file.parent_path(), error);
    if (auto pinned = write_file_atomically(writer, context.config_file, config_to_json(context.config)); !pinned) {
        return std::unexpected(repository_error("cannot pin repository in config: " + pinned.error().message));
    }
    return {};
}

} // namespace

std::expected<void, IoError> initialise_repository(
    AppContext& context, IFileWriter& writer, std::chrono::year_month_day today)
{
    // repository.json marks a complete repository (decision 0011). An init
    // that finds one opens it, and pins it where this init pins: a first
    // init that stopped between repository.json and the pin left it
    // complete but unpinned, and so did one whose config file was lost
    // since; running `repo init` again finishes the job.
    if (path_exists(context.repo.root / "repository.json")) {
        if (auto opened = open_repository(context, today); !opened) {
            return opened;
        }
        return write_pin(context, writer);
    }
    if (auto result = create_repository_directories(context.repo); !result) {
        return result;
    }
    if (auto result = initialise_database(context.repo.database()); !result) {
        return result;
    }
    if (auto result = write_repository_metadata(context, writer); !result) {
        return result;
    }
    return write_pin(context, writer);
}

std::string_view database_state_code(DatabaseState state)
{
    switch (state) {
    case DatabaseState::Usable:
        return "usable";
    case DatabaseState::Missing:
        return "missing";
    case DatabaseState::Empty:
        return "empty";
    case DatabaseState::NotADatabase:
        return "not_a_database";
    case DatabaseState::Damaged:
        return "damaged";
    case DatabaseState::Unfilled:
        return "unfilled";
    }
    return "usable";
}

std::string unusable_database_message(const RepositoryLayout& layout, DatabaseState state)
{
    const auto database = path_to_utf8(layout.database());
    const auto how = "; make it again from the manifests with `repo rebuild-index`";
    switch (state) {
    case DatabaseState::Missing:
        return "the repository's database " + database + " is missing" + how;
    case DatabaseState::Empty:
        return "the repository's database " + database + " is empty" + how;
    case DatabaseState::NotADatabase:
        return "the repository's database " + database + " is not a database" + how;
    case DatabaseState::Damaged:
        return "the repository's database " + database + " is damaged" + how;
    case DatabaseState::Unfilled:
        return "the repository's database " + database +
               " was made again, but its rebuild from the manifests did not finish; finish it with `repo "
               "rebuild-index`";
    case DatabaseState::Usable:
        break;
    }
    return {};
}

std::expected<void, IoError> open_repository(AppContext& context, std::chrono::year_month_day today)
{
    context.schema_upgrade.reset();
    const auto uuid = read_repository_uuid(context.repo.root);
    if (!uuid) {
        return std::unexpected(repository_error("Invalid repository.json"));
    }
    // A database that `repo rebuild-index` makes again, or one that it made
    // and has not filled, says so, in every command and in `repo init`
    // (decision 0036); any other failure keeps SQLite's own reason. Only a
    // database that can be used is upgraded (decision 0032): one of an older
    // version is usable where it holds every table of its version.
    const auto state = database_state(context.repo);
    if (!state) {
        return std::unexpected(state.error());
    }
    if (*state != DatabaseState::Usable) {
        return std::unexpected(repository_error(unusable_database_message(context.repo, *state)));
    }
    auto migrated = migrate_schema(context.repo, today);
    if (!migrated) {
        return std::unexpected(migrated.error());
    }
    context.schema_upgrade = std::move(*migrated);
    if (auto result = validate_schema(context.repo); !result) {
        return result;
    }
    context.repository_uuid = *uuid;
    return {};
}

namespace {

// Which schema versions check_schema takes: this program's alone, as the
// database must be once open_repository has upgraded it (validate_schema); or
// also an older one that migrations lead from, which opening the repository
// upgrades (database_state, decision 0032).
enum class SchemaVersions { Current, CurrentOrUpgradable };

// The schema of this version, or of an older one where `versions` takes it,
// read through `database`, with every table of its version.
std::expected<void, IoError> check_schema(
    const repository_detail::SqliteDatabase& database, SchemaVersions versions)
{
    auto version = database.prepare("SELECT version FROM schema_version");
    // A locked, damaged or unreadable database is not an unsupported schema:
    // report SQLite's own reason ("database is locked", "file is not a
    // database"). Plain SQLITE_ERROR is what a missing schema_version table
    // gives, which means some other program's database.
    if (!version && (version.error().native_code & 0xff) != SQLITE_ERROR) {
        return std::unexpected(version.error());
    }
    if (!version) {
        return std::unexpected(repository_error("unsupported repository schema"));
    }
    const auto row = version->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return std::unexpected(repository_error("unsupported repository schema"));
    }
    // Another version's schema, such as a later program's, is no state of
    // the database: nothing replaces or upgrades it (decisions 0032, 0036).
    const int found = sqlite3_column_int(version->get(), 0);
    if (found != schema_version && (versions == SchemaVersions::Current || !upgradable(found))) {
        return std::unexpected(repository_error("unsupported repository schema"));
    }

    // A database of a schema version that lacks one of that version's tables
    // is damaged (decision 0036): a program made all of them in one
    // transaction (initialise_database), and a migration adds its own in the
    // transaction that raises the version.
    for (const auto& table : required_tables) {
        if (table.since > found) {
            continue;
        }
        auto exists = table_exists(database, table.name);
        if (!exists) {
            return std::unexpected(exists.error());
        }
        if (!*exists) {
            return std::unexpected(IoError{
                IoErrorKind::DatabaseDamaged, SQLITE_CORRUPT, "unsupported or incomplete repository schema"});
        }
    }
    return {};
}

// The mark of a database made again (replaced_database_key), read through
// `database`; nothing where it holds none.
std::expected<std::optional<std::string>, IoError> read_mark(const repository_detail::SqliteDatabase& database)
{
    auto statement = database.prepare("SELECT value FROM settings WHERE key = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, replaced_database_key);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return std::nullopt;
    }
    return statement->text(0);
}

// The state of the database at `file` (database_state), read only.
std::expected<DatabaseState, IoError> state_at(const std::filesystem::path& file, DatabaseCheck check)
{
    std::error_code error;
    const auto status = std::filesystem::status(file, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return DatabaseState::Missing;
    }
    if (error) {
        return std::unexpected(repository_error("cannot read the repository's database: " + error.message()));
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(
            repository_error("the repository's database " + path_to_utf8(file) + " is not a file"));
    }
    // SQLite reads a file of 0 bytes as a database without any table (Empty).
    // SQLite's own codes tell a file that is no database, and a damaged
    // one, from a database that cannot be read now (locked, access denied).
    const auto told = [](const IoError& failure) -> std::expected<DatabaseState, IoError> {
        switch (failure.native_code & 0xff) {
        case SQLITE_NOTADB:
            return DatabaseState::NotADatabase;
        case SQLITE_CORRUPT:
            return DatabaseState::Damaged;
        default:
            return std::unexpected(failure);
        }
    };
    auto database = repository_detail::SqliteDatabase::open(file, true);
    if (!database) {
        return told(database.error());
    }
    {
        auto tables = database->prepare("SELECT count(*) FROM sqlite_master");
        if (!tables) {
            return told(tables.error());
        }
        const auto row = tables->step();
        if (!row) {
            return told(row.error());
        }
        if (*row && sqlite3_column_int(tables->get(), 0) == 0) {
            return DatabaseState::Empty;
        }
    }
    if (auto valid = check_schema(*database, SchemaVersions::CurrentOrUpgradable); !valid) {
        return told(valid.error());
    }
    // Every page, its first row "ok" where none is damaged: a damaged page of
    // a table (sample damage in the tests) reads as damaged only here, or
    // where a command reads that page (sqlite_error).
    if (check == DatabaseCheck::EveryPage) {
        auto pages = database->prepare("PRAGMA quick_check(1)");
        if (!pages) {
            return told(pages.error());
        }
        const auto row = pages->step();
        if (!row) {
            return told(row.error());
        }
        if (!*row || pages->text(0) != "ok") {
            return DatabaseState::Damaged;
        }
    }
    const auto mark = read_mark(*database);
    if (!mark) {
        return told(mark.error());
    }
    return *mark ? DatabaseState::Unfilled : DatabaseState::Usable;
}

} // namespace

std::expected<DatabaseState, IoError> database_state(const RepositoryLayout& layout, DatabaseCheck check)
{
    return state_at(layout.database(), check);
}

namespace {

// The files SQLite keeps beside a database (its journal and shared memory):
// moved with it, so that a new database never takes up the journal of the
// one put aside. The database itself, "", comes last.
constexpr std::array database_suffixes{"-wal", "-shm", "-journal", ""};

// The new database that replace_database makes beside the one it replaces,
// veriarchive.db.new, before it moves it into place.
std::filesystem::path new_database(const RepositoryLayout& layout)
{
    return layout.root / path_from_utf8(path_to_utf8(layout.database().filename()) + ".new");
}

// Removes the new database of a rebuild that stopped before it was moved into
// place, with the files beside it: it holds nothing but the tables and mark
// it was made with, and the database it was to replace is still there.
std::expected<void, IoError> remove_new_database(const RepositoryLayout& layout)
{
    const auto made = path_to_utf8(new_database(layout));
    for (const char* suffix : database_suffixes) {
        std::error_code error;
        std::filesystem::remove(path_from_utf8(made + suffix), error);
        if (error) {
            return std::unexpected(repository_error(
                "cannot remove the unfinished new database " + made + suffix + ": " + error.message()));
        }
    }
    return {};
}

// Whether any of the files SQLite keeps beside veriarchive.db is there.
bool files_beside_the_database(const RepositoryLayout& layout)
{
    const auto name = path_to_utf8(layout.database());
    return std::ranges::any_of(database_suffixes, [&](const char* suffix) {
        return *suffix != '\0' && path_exists(path_from_utf8(name + suffix));
    });
}

// Replaces the database that cannot be used (decision 0036): makes the new
// one beside it, with its mark, moves what is there aside, with the files
// SQLite keeps beside it, and moves the new one into place. Nothing is
// deleted but the new database of a rebuild that stopped before it was in
// place.
std::expected<void, IoError> replace_database(const RepositoryLayout& layout, DatabaseState state)
{
    // "veriarchive.db.damaged-2026-10-27T10-15-30Z": its own name and when it
    // was put aside (UTC), the time without colons, which Windows does not
    // take in a file name; "-2", "-3", ... where that name is taken.
    auto stamp = current_timestamp();
    for (auto& character : stamp) {
        if (character == ':') {
            character = '-';
        }
    }
    const auto name = path_to_utf8(layout.database().filename());
    std::string aside;
    for (int attempt = 1;; ++attempt) {
        aside = name + ".damaged-" + stamp + (attempt > 1 ? "-" + std::to_string(attempt) : std::string{});
        const bool taken = std::ranges::any_of(database_suffixes,
            [&](const char* suffix) { return path_exists(layout.root / path_from_utf8(aside + suffix)); });
        if (!taken) {
            break;
        }
    }
    std::vector<const char*> there;
    for (const char* suffix : database_suffixes) {
        if (path_exists(layout.root / path_from_utf8(name + suffix))) {
            there.push_back(suffix);
        }
    }
    const bool database_there = !there.empty() && *there.back() == '\0';
    auto set_aside = nlohmann::json::array();
    for (const char* suffix : there) {
        set_aside.push_back(aside + suffix);
    }
    const nlohmann::json mark{{"state", database_state_code(state)},
        {"moved_to", database_there ? nlohmann::json(aside) : nlohmann::json(nullptr)},
        {"set_aside", std::move(set_aside)}};

    // The new database first, complete with its mark: until it is in place,
    // the database to replace stays where it was.
    if (auto removed = remove_new_database(layout); !removed) {
        return removed;
    }
    const auto made = new_database(layout);
    if (auto created = initialise_database(made, mark.dump()); !created) {
        (void)remove_new_database(layout);
        return std::unexpected(created.error());
    }
    for (const char* suffix : there) {
        const auto from = layout.root / path_from_utf8(name + suffix);
        std::error_code error;
        std::filesystem::rename(from, layout.root / path_from_utf8(aside + suffix), error);
        if (error) {
            return std::unexpected(repository_error(
                "cannot put the repository's database aside: " + path_to_utf8(from) + ": " + error.message()));
        }
    }
    std::error_code error;
    std::filesystem::rename(made, layout.database(), error);
    if (error) {
        return std::unexpected(repository_error("cannot put the new repository database in place: " +
                                                path_to_utf8(made) + ": " + error.message()));
    }
    return {};
}

// The database put aside, as the mark of the database made again says.
std::optional<ReplacedDatabase> replaced_by_mark(const RepositoryLayout& layout, const std::string& mark)
{
    const auto json = nlohmann::json::parse(mark, nullptr, false);
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplacedDatabase replaced;
    const auto state = json.find("state");
    const auto code = state != json.end() && state->is_string() ? state->get<std::string>() : std::string{};
    if (code == database_state_code(DatabaseState::Missing)) {
        replaced.state = DatabaseState::Missing;
    } else if (code == database_state_code(DatabaseState::Empty)) {
        replaced.state = DatabaseState::Empty;
    } else if (code == database_state_code(DatabaseState::NotADatabase)) {
        replaced.state = DatabaseState::NotADatabase;
    } else {
        replaced.state = DatabaseState::Damaged;
    }
    if (const auto moved = json.find("moved_to"); moved != json.end() && moved->is_string()) {
        replaced.moved_to = layout.root / path_from_utf8(moved->get<std::string>());
    }
    if (const auto files = json.find("set_aside"); files != json.end() && files->is_array()) {
        for (const auto& file : *files) {
            if (file.is_string()) {
                replaced.set_aside.push_back(layout.root / path_from_utf8(file.get<std::string>()));
            }
        }
    }
    return replaced;
}

// The mark of the database at its place: of the database made now, or by a
// rebuild that did not finish, which names the database put aside then; none
// where it holds none. Such a database is put in WAL mode (R-91), in which it
// was not made.
std::expected<std::optional<ReplacedDatabase>, IoError> marked_replacement(const RepositoryLayout& layout)
{
    auto database = repository_detail::SqliteDatabase::open(layout.database());
    if (!database) {
        return std::unexpected(database.error());
    }
    const auto mark = read_mark(*database);
    if (!mark) {
        return std::unexpected(mark.error());
    }
    if (!*mark) {
        return std::nullopt;
    }
    if (auto wal = database->execute("PRAGMA journal_mode=WAL;"); !wal) {
        return std::unexpected(wal.error());
    }
    return replaced_by_mark(layout, **mark);
}

} // namespace

std::expected<std::optional<ReplacedDatabase>, IoError> open_repository_to_rebuild(
    AppContext& context, std::chrono::year_month_day today)
{
    context.schema_upgrade.reset();
    const auto uuid = read_repository_uuid(context.repo.root);
    if (!uuid) {
        return std::unexpected(repository_error("Invalid repository.json"));
    }
    const auto& layout = context.repo;
    // Every page: the rebuild reads all of the database, so one damaged
    // beyond its first page is made again before, never filled.
    auto state = database_state(layout, DatabaseCheck::EveryPage);
    if (!state) {
        return std::unexpected(state.error());
    }
    // A rebuild that stopped between moving the database aside and moving its
    // new one into place: the new one, complete with its mark, which names
    // the database put aside, is taken up, unless a journal is left beside
    // the place, which it would take up.
    if (*state == DatabaseState::Missing && !files_beside_the_database(layout)) {
        const auto made = new_database(layout);
        if (const auto made_state = state_at(made, DatabaseCheck::Schema);
            made_state && *made_state == DatabaseState::Unfilled) {
            std::error_code error;
            std::filesystem::rename(made, layout.database(), error);
            if (error) {
                return std::unexpected(repository_error("cannot put the new repository database in place: " +
                                                        path_to_utf8(made) + ": " + error.message()));
            }
            state = DatabaseState::Unfilled;
        }
    }
    context.repository_uuid = *uuid;
    if (*state != DatabaseState::Usable && *state != DatabaseState::Unfilled) {
        if (auto replaced = replace_database(layout, *state); !replaced) {
            return std::unexpected(replaced.error());
        }
    } else {
        // A database of an older schema version, also one that a rebuild of
        // an earlier version made again, is upgraded as opening the
        // repository upgrades it, keeping a copy of it first, so that the
        // rebuild fills an index of this version (decision 0032). A database
        // made again now has this version's schema.
        auto migrated = migrate_schema(layout, today);
        if (!migrated) {
            return std::unexpected(migrated.error());
        }
        context.schema_upgrade = std::move(*migrated);
        if (auto valid = validate_schema(layout); !valid) {
            return std::unexpected(valid.error());
        }
    }
    return marked_replacement(layout);
}

std::expected<ReplacedDatabase, IoError> make_damaged_database_again(const RepositoryLayout& layout)
{
    if (auto replaced = replace_database(layout, DatabaseState::Damaged); !replaced) {
        return std::unexpected(replaced.error());
    }
    auto marked = marked_replacement(layout);
    if (!marked) {
        return std::unexpected(marked.error());
    }
    return marked->value_or(ReplacedDatabase{DatabaseState::Damaged, std::nullopt, {}});
}

std::expected<void, IoError> record_repository_uuid(AppContext& context, IFileWriter& writer)
{
    // The record belongs to the pin (decision 0037): a repository chosen for
    // a command with --repository or VERIARCHIVE_REPOSITORY is never compared
    // with it, nor written into it, so using one once does not make the next
    // command on the pinned repository warn. An init that pins writes the
    // record with the pin (initialise_repository).
    if (context.repo_selected_by != "pinned" || context.config.last_repository_uuid == context.repository_uuid) {
        return {};
    }
    if (!context.config.last_repository_uuid.empty()) {
        context.previous_repository_uuid = context.config.last_repository_uuid;
    }
    if (!path_exists(context.config_file)) {
        return {};
    }
    context.config.last_repository_uuid = context.repository_uuid;
    if (auto written = write_file_atomically(
            writer, context.config_file, config_to_json(context.config));
        !written) {
        return std::unexpected(repository_error(
            "cannot record repository_uuid in config: " + written.error().message));
    }
    return {};
}

std::expected<void, IoError> validate_schema(const RepositoryLayout& layout)
{
    auto database = repository_detail::SqliteDatabase::open(layout.database(), true);
    if (!database) {
        return std::unexpected(database.error());
    }
    return check_schema(*database, SchemaVersions::Current);
}

} // namespace va