#pragma once

#include "common/error.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
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

    // Parameters are numbered from 1, columns from 0 (SQLite's convention).
    SqliteStatement& bind(int index, std::string_view text);
    SqliteStatement& bind(int index, std::int64_t value);
    SqliteStatement& bind_null(int index);
    // true when a result row is available, false when the statement is done.
    std::expected<bool, IoError> step();
    std::expected<void, IoError> run(); // step() expecting no rows
    void reset();

    std::string text(int column) const;
    std::int64_t integer(int column) const;
    bool is_null(int column) const;

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