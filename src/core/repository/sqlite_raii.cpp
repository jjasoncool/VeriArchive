#include "core/repository/sqlite_raii.h"

#include "common/utf8.h"

#include <sqlite3.h>

#include <string>
#include <utility>

namespace va::repository_detail {

namespace {

IoError sqlite_error(sqlite3* database, std::string_view operation)
{
    const int code = database ? sqlite3_errcode(database) : 0;
    // A damaged database, or a file that is none, wherever SQLite finds it
    // (a damaged page that opening the repository does not read): what is
    // wrong in plain words, with the database's path and the way to fix it,
    // as opening the repository says it (decision 0036).
    if ((code & 0xff) == SQLITE_CORRUPT || (code & 0xff) == SQLITE_NOTADB) {
        const char* file = sqlite3_db_filename(database, "main");
        return {IoErrorKind::DatabaseDamaged, code,
            "the repository's database " + std::string(file ? file : "") +
                ((code & 0xff) == SQLITE_CORRUPT ? " is damaged" : " is not a database") +
                "; make it again from the manifests with `repo rebuild-index`"};
    }
    std::string message(operation);
    message += ": ";
    message += database ? sqlite3_errmsg(database) : "unknown SQLite error";
    return {IoErrorKind::Other, code, std::move(message)};
}

} // namespace

SqliteStatement::SqliteStatement(sqlite3_stmt* statement) noexcept : statement_(statement) {}

SqliteStatement::~SqliteStatement()
{
    sqlite3_finalize(statement_);
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr))
{
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept
{
    if (this != &other) {
        sqlite3_finalize(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

sqlite3_stmt* SqliteStatement::get() const noexcept
{
    return statement_;
}

SqliteDatabase::SqliteDatabase(sqlite3* database) noexcept : database_(database) {}

SqliteDatabase::~SqliteDatabase()
{
    sqlite3_close(database_);
}

SqliteDatabase::SqliteDatabase(SqliteDatabase&& other) noexcept
    : database_(std::exchange(other.database_, nullptr))
{
}

SqliteDatabase& SqliteDatabase::operator=(SqliteDatabase&& other) noexcept
{
    if (this != &other) {
        sqlite3_close(database_);
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

std::expected<SqliteDatabase, IoError> SqliteDatabase::open(
    const std::filesystem::path& path,
    bool read_only)
{
    sqlite3* database = nullptr;
    const int flags = read_only ? SQLITE_OPEN_READONLY
                                : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    const int result = sqlite3_open_v2(path_to_utf8(path).c_str(), &database, flags, nullptr);
    if (result != SQLITE_OK) {
        const auto error = sqlite_error(database, "cannot open repository database");
        sqlite3_close(database);
        return std::unexpected(error);
    }
    return SqliteDatabase(database);
}

std::expected<void, IoError> SqliteDatabase::execute(std::string_view sql) const
{
    if (sqlite3_exec(database_, std::string(sql).c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database_, "cannot execute SQL"));
    }
    return {};
}

std::expected<SqliteStatement, IoError> SqliteDatabase::prepare(std::string_view sql) const
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database_, "cannot prepare SQL"));
    }
    return std::expected<SqliteStatement, IoError>(std::in_place, statement);
}

bool SqliteDatabase::in_transaction() const
{
    return sqlite3_get_autocommit(database_) == 0;
}

std::int64_t SqliteDatabase::last_insert_rowid() const
{
    return sqlite3_last_insert_rowid(database_);
}

std::expected<void, IoError> SqliteDatabase::copy_to(const std::filesystem::path& destination) const
{
    sqlite3* copy = nullptr;
    const auto failed = [&copy]() {
        IoError error{IoErrorKind::Other, copy ? sqlite3_errcode(copy) : 0,
            copy ? sqlite3_errmsg(copy) : "unknown SQLite error"};
        sqlite3_close(copy);
        return std::unexpected(std::move(error));
    };
    if (sqlite3_open_v2(path_to_utf8(destination).c_str(), &copy, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr) != SQLITE_OK ||
        sqlite3_exec(copy, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return failed();
    }
    sqlite3_backup* backup = sqlite3_backup_init(copy, "main", database_, "main");
    if (!backup) {
        return failed();
    }
    // One step copies every page while this connection's read holds: the copy
    // is the database as of one moment. sqlite3_backup_finish leaves the
    // step's error, if any, on the copy's connection.
    const int stepped = sqlite3_backup_step(backup, -1);
    if (sqlite3_backup_finish(backup) != SQLITE_OK || stepped != SQLITE_DONE) {
        return failed();
    }
    if (sqlite3_close(copy) != SQLITE_OK) {
        return failed();
    }
    return {};
}

} // namespace va::repository_detail
namespace va::repository_detail {

SqliteStatement& SqliteStatement::bind(int index, std::string_view text)
{
    sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    return *this;
}

SqliteStatement& SqliteStatement::bind(int index, std::int64_t value)
{
    sqlite3_bind_int64(statement_, index, value);
    return *this;
}

SqliteStatement& SqliteStatement::bind_null(int index)
{
    sqlite3_bind_null(statement_, index);
    return *this;
}

SqliteStatement& SqliteStatement::bind_blob(int index, std::span<const std::uint8_t> bytes)
{
    if (bytes.empty()) {
        return bind_null(index);
    }
    sqlite3_bind_blob(statement_, index, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    return *this;
}

std::expected<bool, IoError> SqliteStatement::step()
{
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    return std::unexpected(sqlite_error(sqlite3_db_handle(statement_), "cannot run SQL"));
}

std::expected<void, IoError> SqliteStatement::run()
{
    const auto stepped = step();
    if (!stepped) {
        return std::unexpected(stepped.error());
    }
    reset();
    return {};
}

void SqliteStatement::reset()
{
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

std::string SqliteStatement::text(int column) const
{
    const auto* value = sqlite3_column_text(statement_, column);
    const int length = sqlite3_column_bytes(statement_, column);
    return value ? std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(length))
                 : std::string{};
}

std::int64_t SqliteStatement::integer(int column) const
{
    return sqlite3_column_int64(statement_, column);
}

bool SqliteStatement::is_null(int column) const
{
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::vector<std::uint8_t> SqliteStatement::blob(int column) const
{
    const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, column));
    const int length = sqlite3_column_bytes(statement_, column);
    return bytes ? std::vector<std::uint8_t>(bytes, bytes + length) : std::vector<std::uint8_t>{};
}

} // namespace va::repository_detail
