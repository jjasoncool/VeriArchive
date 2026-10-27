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
#include <fstream>
#include <sqlite3.h>

namespace va {
namespace {

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

std::expected<void, IoError> initialise_repository(AppContext& context, IFileWriter& writer)
{
    // repository.json marks a complete repository (decision 0011). An init
    // that finds one opens it, and pins it where this init pins: a first
    // init that stopped between repository.json and the pin left it
    // complete but unpinned, and so did one whose config file was lost
    // since; running `repo init` again finishes the job.
    if (path_exists(context.repo.root / "repository.json")) {
        if (auto opened = open_repository(context); !opened) {
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

std::expected<void, IoError> open_repository(AppContext& context)
{
    const auto uuid = read_repository_uuid(context.repo.root);
    if (!uuid) {
        return std::unexpected(repository_error("Invalid repository.json"));
    }
    // A database that `repo rebuild-index` makes again, or one that it made
    // and has not filled, says so, in every command and in `repo init`
    // (decision 0036); any other failure keeps SQLite's own reason.
    const auto state = database_state(context.repo);
    if (!state) {
        return std::unexpected(state.error());
    }
    if (*state != DatabaseState::Usable) {
        return std::unexpected(repository_error(unusable_database_message(context.repo, *state)));
    }
    context.repository_uuid = *uuid;
    return {};
}

namespace {

// The schema of this version (validate_schema), read through `database`.
std::expected<void, IoError> check_schema(const repository_detail::SqliteDatabase& database)
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
    if (!*row || sqlite3_column_int(version->get(), 0) != 1) {
        return std::unexpected(repository_error("unsupported repository schema"));
    }

    constexpr std::array required_tables{
        "archives", "archive_revisions", "media_evidence", "files", "chunks",
        "verify_runs", "verify_items", "verify_progress", "history_checkpoints",
        "trusted_keys", "settings",
    };

    // A database of this version's schema that lacks one of its tables is
    // damaged (decision 0036): this version made all of them in one
    // transaction (initialise_database).
    for (const char* table : required_tables) {
        auto exists = table_exists(database, table);
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
    if (auto valid = check_schema(*database); !valid) {
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
    const nlohmann::json mark{{"state", database_state_code(state)},
        {"moved_to", database_there ? nlohmann::json(aside) : nlohmann::json(nullptr)}};

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

std::expected<std::optional<ReplacedDatabase>, IoError> open_repository_to_rebuild(AppContext& context)
{
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
    return marked->value_or(ReplacedDatabase{DatabaseState::Damaged, std::nullopt});
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
    return check_schema(*database);
}

} // namespace va