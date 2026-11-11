#pragma once

#include "common/error.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    // A BLOB (decision 0052: the pages of a SMART read); an empty one is NULL.
    SqliteStatement& bind_blob(int index, std::span<const std::uint8_t> bytes);
    // true when a result row is available, false when the statement is done.
    std::expected<bool, IoError> step();
    std::expected<void, IoError> run(); // step() expecting no rows
    void reset();

    std::string text(int column) const;
    std::int64_t integer(int column) const;
    bool is_null(int column) const;
    // The bytes of a BLOB column; none for NULL.
    std::vector<std::uint8_t> blob(int column) const;

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
    // True while a transaction is open on this connection (BEGIN without its
    // COMMIT or ROLLBACK yet).
    bool in_transaction() const;
    // The rowid of the row this connection inserted last.
    std::int64_t last_insert_rowid() const;
    // Copies the database, as this connection reads it in one read
    // transaction, into a new database file at `destination` through
    // SQLite's backup API, which refuses a connection in a write transaction
    // (SQLITE_BUSY). The copy's own commit syncs it to stable storage before
    // this returns (synchronous=FULL), which VACUUM INTO would not do. The
    // error's message is SQLite's own ("unable to open database file",
    // "database or disk is full").
    std::expected<void, IoError> copy_to(const std::filesystem::path& destination) const;

private:
    explicit SqliteDatabase(sqlite3* database) noexcept;
    sqlite3* database_ = nullptr;
};

} // namespace va::repository_detail