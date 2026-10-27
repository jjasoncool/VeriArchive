#pragma once
// What is at a repository's database, veriarchive.db, and what `repo
// rebuild-index` does of one that cannot be used (decision 0036). Defined in
// repository.cpp.
#include "common/error.h"
#include "common/repository_layout.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace va {

// What is at a repository's database, veriarchive.db (decision 0036).
enum class DatabaseState {
    Usable,        // a database of this version's schema
    Missing,       // no file
    Empty,         // a file of 0 bytes, or a database without any table
    NotADatabase,  // SQLite does not read the file as a database
    Damaged,       // SQLite reads it as a damaged database ("database disk image is malformed"), or one of
                   // this version's schema lacks a table
    Unfilled,      // a database that `repo rebuild-index` made again, whose index no rebuild has filled yet:
                   // it still holds the mark it was made with (replaced_database_key)
};

// How much of the database database_state reads (decision 0036): its first
// page, schema and mark, as every command opens it; or also every page
// (SQLite's quick_check), as `repo rebuild-index` does, which reads all of
// it, so that it never fills a database that is damaged beyond its first page.
enum class DatabaseCheck { Schema, EveryPage };

// How --json and the mark of a database made again name a state: "missing",
// "empty", "not_a_database", "damaged" ("usable", "unfilled").
std::string_view database_state_code(DatabaseState state);

// The state of the repository's database, read only. A database that cannot
// be read for another reason (locked, access denied, a disk error), and one
// that holds tables of another program or of another version's schema, is an
// error: neither `repo rebuild-index` nor anything else replaces it.
std::expected<DatabaseState, IoError> database_state(
    const RepositoryLayout& layout, DatabaseCheck check = DatabaseCheck::Schema);

// What every command says of a database in `state` (any but Usable): what is
// wrong with it, with its path, and that `repo rebuild-index` makes it again
// or finishes it (decision 0036).
std::string unusable_database_message(const RepositoryLayout& layout, DatabaseState state);

// The database that a rebuild put aside, as the mark of the new one says.
struct ReplacedDatabase {
    DatabaseState state = DatabaseState::Missing;   // why it was replaced
    std::optional<std::filesystem::path> moved_to;   // where veriarchive.db went; none where it was missing
};

// Puts the repository's database aside as damaged and makes it again, as
// open_repository_to_rebuild does, for a rebuild that met damage that the
// check of every page did not see (IoErrorKind::DatabaseDamaged, such as an
// index that does not agree with its table). Gives the database put aside.
std::expected<ReplacedDatabase, IoError> make_damaged_database_again(const RepositoryLayout& layout);

} // namespace va
