#pragma once
#include "app/app_context.h"
#include "common/error.h"
#include "core/interfaces.h"
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
// complete but was never pinned (decision 0011).
std::expected<void, IoError> initialise_repository(AppContext& context, IFileWriter& writer);
// Reads repository.json and checks the database's schema. A database that is
// missing, empty, not a database or damaged (database_state) fails with a
// message that names `repo rebuild-index`, which makes it again (decision
// 0036); any other failure keeps SQLite's own reason.
std::expected<void, IoError> open_repository(AppContext& context);
// R-83 step 5, for the pinned repository only (decision 0037): when
// repository.json's uuid differs from the one recorded in the config, sets
// context.previous_repository_uuid (the caller warns) and records the new uuid
// in the existing config file. A repository chosen with --repository or
// VERIARCHIVE_REPOSITORY is neither compared nor recorded.
std::expected<void, IoError> record_repository_uuid(AppContext& context, IFileWriter& writer);
std::expected<void, IoError> validate_schema(const RepositoryLayout& layout);

// What is at a repository's database, veriarchive.db (decision 0036).
enum class DatabaseState {
    Usable,        // a database of this version's schema
    Missing,       // no file
    Empty,         // a file of 0 bytes, or a database without any table
    NotADatabase,  // SQLite does not read the file as a database
    Damaged,       // SQLite reads it as a damaged database ("database disk image is malformed")
};

// The state of the repository's database, read only. A database that cannot
// be read for another reason (locked, access denied, a disk error), and one
// that holds tables of another program or of another version's schema, is an
// error: neither `repo rebuild-index` nor anything else replaces it.
std::expected<DatabaseState, IoError> database_state(const RepositoryLayout& layout);

// The database that replace_database put aside.
struct ReplacedDatabase {
    DatabaseState state = DatabaseState::Missing;   // why it was replaced
    std::optional<std::filesystem::path> moved_to;   // where veriarchive.db went; none where it was missing
};

// Opens the repository for `repo rebuild-index` (decision 0036): reads
// repository.json, and where the database is missing, empty, not a database
// or damaged, moves what is there aside, veriarchive.db (and its -wal, -shm
// and -journal files) to veriarchive.db.damaged-<UTC date and time>, and
// creates a new, empty database of this version's schema in its place; the
// rebuild then fills its index from manifests/ (R-80). The repository keeps
// its uuid. Nothing where the database can be used; an error, with nothing
// moved, where it cannot be read for another reason (database_state).
std::expected<std::optional<ReplacedDatabase>, IoError> open_repository_to_rebuild(AppContext& context);

} // namespace va