#include "core/repository/repository.h"

#include "common/canonical_json.h"
#include "common/uuid.h"
#include "core/repository/sqlite_raii.h"
#include "core/write_file.h"
#include "schema_sql.h"

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <sqlite3.h>

namespace va {
namespace {

constexpr int repository_error_code = 11;

IoError repository_error(std::string message)
{
    return {IoErrorKind::Other, repository_error_code, std::move(message)};
}

std::filesystem::path select_config_path(const RepositoryOptions& options)
{
    if (options.explicit_config) {
        return *options.explicit_config;
    }

    const auto portable_config = options.executable_dir / "config.json";
    if (std::filesystem::exists(portable_config)) {
        return portable_config;
    }

    return options.user_config_dir / "config.json";
}

std::string current_timestamp()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", now);
}

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
    context.config_file = select_config_path(options);

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
        context.repo.root = *context.config.repository;
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

    if (context.repo.root.is_relative()) {
        context.repo.root = options.executable_dir / context.repo.root;
    }
    context.repo.root = context.repo.root.lexically_normal();
    if (!first_run && !read_repository_uuid(context.repo.root)) {
        return std::unexpected(repository_error(
            "configured repository is unavailable or is not a VeriArchive repository"));
    }

    if (probes.network_filesystem(context.repo.root) &&
        !context.config.allow_network_repository) {
        return std::unexpected(repository_error(
            "SQLite on a network filesystem is unsafe (locking, WAL); "
            "set allow_network_repository or move the repository"));
    }

    if (first_run) {
        context.config.repository = context.repo.root;
        if (!options.explicit_config && probes.writable(options.executable_dir)) {
            context.config_file = options.executable_dir / "config.json";
        }
    }
    return context;
}

std::expected<void, IoError> initialise_repository(AppContext& context, IFileWriter& writer)
{
    if (std::filesystem::exists(context.repo.root / "repository.json")) {
        return open_repository(context);
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

    context.config.last_repository_uuid = context.repository_uuid;
    if (context.config.repository) {
        std::error_code error;
        std::filesystem::create_directories(context.config_file.parent_path(), error);
        if (auto pinned = write_file_atomically(
                writer, context.config_file, config_to_json(context.config));
            !pinned) {
            return std::unexpected(
                repository_error("cannot pin repository in config: " + pinned.error().message));
        }
    }
    return {};
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
    if (context.config.last_repository_uuid == context.repository_uuid) {
        return {};
    }
    if (!context.config.last_repository_uuid.empty()) {
        context.previous_repository_uuid = context.config.last_repository_uuid;
    }
    if (!std::filesystem::exists(context.config_file)) {
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
    if (!version || sqlite3_step(version->get()) != SQLITE_ROW ||
        sqlite3_column_int(version->get(), 0) != 1) {
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