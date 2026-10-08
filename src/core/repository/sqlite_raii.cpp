#include "core/repository/sqlite_raii.h"

#include "common/utf8.h"

#include <sqlite3.h>

#include <string>
#include <utility>

namespace va::repository_detail {

namespace {

IoError sqlite_error(sqlite3* database, std::string_view operation)
{
    std::string message(operation);
    message += ": ";
    message += database ? sqlite3_errmsg(database) : "unknown SQLite error";
    return {IoErrorKind::Other, database ? sqlite3_errcode(database) : 0, std::move(message)};
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

} // namespace va::repository_detail