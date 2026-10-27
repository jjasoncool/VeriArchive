#include "core/repository/repository.h"

#include "common/canonical_json.h"
#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/repository/sqlite_raii.h"
#include "core/write_file.h"
#include "schema_sql.h"

#include <array>
#include <fstream>
#include <sqlite3.h>

namespace va {
namespace {

constexpr int repository_error_code = 11;

IoError repository_error(std::string message)
{
    return {IoErrorKind::Other, repository_error_code, std::move(message)};
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

std::expected<void, IoError> initialise_database(const RepositoryLayout& layout)
{
    auto database = repository_detail::SqliteDatabase::open(layout.database());
    if (!database) {
        return std::unexpected(database.error());
    }

    if (auto result = database->execute(
            "PRAGMA journal_mode=WAL;"
            "PRAGMA synchronous=NORMAL;");
        !result) {
        return result;
    }

    auto exists = schema_exists(*database);
    if (!exists) {
        return std::unexpected(exists.error());
    }
    if (*exists) {
        return {};
    }
    // One transaction: a crash leaves either no schema or the complete schema.
    if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun) {
        return begun;
    }
    if (auto created = database->execute(schema_sql); !created) {
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
    if (auto result = initialise_database(context.repo); !result) {
        return result;
    }
    if (auto result = write_repository_metadata(context, writer); !result) {
        return result;
    }
    return write_pin(context, writer);
}

std::expected<void, IoError> open_repository(AppContext& context)
{
    const auto uuid = read_repository_uuid(context.repo.root);
    if (!uuid) {
        return std::unexpected(repository_error("Invalid repository.json"));
    }
    if (auto result = validate_schema(context.repo); !result) {
        return result;
    }
    context.repository_uuid = *uuid;
    return {};
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

    auto version = database->prepare("SELECT version FROM schema_version");
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

    for (const char* table : required_tables) {
        auto exists = table_exists(*database, table);
        if (!exists) {
            return std::unexpected(exists.error());
        }
        if (!*exists) {
            return std::unexpected(
                repository_error("unsupported or incomplete repository schema"));
        }
    }
    return {};
}

} // namespace va