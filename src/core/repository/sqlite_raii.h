#pragma once

#include "common/error.h"

#include <expected>
#include <filesystem>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace va::repository_detail {

class SqliteStatement {
public:
    explicit SqliteStatement(sqlite3_stmt* statement) noexcept;
    ~SqliteStatement();

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    sqlite3_stmt* get() const noexcept;

private:
    sqlite3_stmt* statement_;
};

class SqliteDatabase {
public:
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&& other) noexcept;
    SqliteDatabase& operator=(SqliteDatabase&& other) noexcept;

    static std::expected<SqliteDatabase, IoError> open(
        const std::filesystem::path& path,
        bool read_only = false);

    std::expected<void, IoError> execute(std::string_view sql) const;
    std::expected<SqliteStatement, IoError> prepare(std::string_view sql) const;

private:
    explicit SqliteDatabase(sqlite3* database) noexcept;
    sqlite3* database_ = nullptr;
};

} // namespace va::repository_detail