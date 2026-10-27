#pragma once
#include "app/app_context.h"
#include "common/error.h"
#include "core/interfaces.h"
#include "core/repository/database_state.h"
#include <chrono>
#include <expected>
#include <functional>
#include <optional>

namespace va {

struct RepositoryOptions {
    bool initialise = false;
    std::optional<std::filesystem::path> explicit_repository;
    std::optional<std::filesystem::path> explicit_config;
    std::optional<std::filesystem::path> environment_repository;
    std::filesystem::path executable_dir;
    std::filesystem::path user_config_dir;
    std::filesystem::path user_home;
};

struct RepositoryProbes {
    std::function<bool(const std::filesystem::path&)> writable;
    std::function<bool(const std::filesystem::path&)> network_filesystem;
};

// The config file used (R-82): --config, else <executable_dir>/config.json when
// it exists (portable install), else the user config directory's config.json.
std::filesystem::path config_file_path(const RepositoryOptions& options);

// R-83 with decision 0019. A relative --repository or VERIARCHIVE_REPOSITORY is
// taken from executable_dir; a relative config.repository from the folder of
// the config file that holds it; neither ever from the current directory (R-84).
// context.repo.root is always the resolved path, while context.config.repository
// keeps the value as stored. A new pin inside the config file's folder is
// stored relative to it ("data" in a portable install), so the program folder
// can be moved or copied; any other pin is stored as an absolute path.
std::expected<AppContext, IoError> resolve_repository(
    const RepositoryOptions& options,
    const RepositoryProbes& probes);
// Crash-safe order: directories, then the database schema in one transaction,
// then repository.json (its presence marks a complete repository), then the
// config pin, where this init pins (context.pins_repository). Re-running after
// a crash at any step completes the repository, and pins one that is
// complete but was never pinned (decision 0011). A repository that exists
// already is opened (open_repository, `today`).
std::expected<void, IoError> initialise_repository(
    AppContext& context, IFileWriter& writer, std::chrono::year_month_day today);
// Opens an existing repository: reads repository.json and checks the
// database's schema. A database that is missing, empty, not a database or
// damaged, or one that a rebuild made again and did not fill
// (database_state), fails with a message that names `repo rebuild-index`
// (decision 0036); any other failure keeps SQLite's own reason. A database
// that can be used, at an older schema version (V1's version 1, or version
// 2), is then raised to this program's, in one write transaction (R-92,
// decisions 0032 and 0033), which first keeps a copy of it as it is beside
// it, named for its version and `today`, the local date:
// veriarchive-schema-1-before-upgrade-2026-10-17.db (decision 0032). Where
// that copy cannot be made, nothing is upgraded and the open fails.
// context.schema_upgrade says what was upgraded, and where the copy is, or
// nothing. A database of a version this program does not know is refused
// ("unsupported repository schema") and left as it is.
std::expected<void, IoError> open_repository(AppContext& context, std::chrono::year_month_day today);
// R-83 step 5, for the pinned repository only (decision 0037): when
// repository.json's uuid differs from the one recorded in the config, sets
// context.previous_repository_uuid (the caller warns) and records the new uuid
// in the existing config file. A repository chosen with --repository or
// VERIARCHIVE_REPOSITORY is neither compared nor recorded.
std::expected<void, IoError> record_repository_uuid(AppContext& context, IFileWriter& writer);
std::expected<void, IoError> validate_schema(const RepositoryLayout& layout);

// Opens the repository for `repo rebuild-index` (decision 0036): reads
// repository.json, and where the database is missing, empty, not a database
// or damaged, replaces it. The new database, of this version's schema, is
// made beside it as veriarchive.db.new, with a mark that says why and where
// the old one goes (replaced_database_key), in the transaction that creates
// its tables; then what is there, veriarchive.db and its -wal, -shm and
// -journal files, is moved aside to veriarchive.db.damaged-<UTC date and
// time>, and the new database is put in its place. The mark stays until a
// rebuild has filled the index from manifests/ (R-80): while it is there, the
// database is Unfilled, every command names the rebuild for it, and the next
// rebuild names the database put aside again. A rebuild that stopped between
// moving the database aside and putting the new one in place left its new
// one beside it, which is taken up. A database that can be used, or that a
// rebuild made again, at an older schema version is raised to this
// program's, as open_repository raises it, keeping its copy named for
// `today` first (context.schema_upgrade), so that the rebuild fills an index
// of this version (decision 0032). The repository keeps its uuid. Gives the
// mark's database put aside, made now or by a rebuild that did not finish;
// nothing where the database can be used and holds no mark; an error, with
// nothing moved, where it cannot be read for another reason (database_state).
std::expected<std::optional<ReplacedDatabase>, IoError> open_repository_to_rebuild(
    AppContext& context, std::chrono::year_month_day today);

} // namespace va