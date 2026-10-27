#pragma once
// A freshly initialised repository in a temporary directory, removed afterwards.
#include "test_support.h"

#include "core/repository/archive_store.h"
#include "core/repository/repository.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"

#include <chrono>
#include <sqlite3.h>

namespace test {

struct TestRepository {
    va::AppContext context;
    std::unique_ptr<va::IFileWriter> writer = va::platform::make_file_writer();
    std::unique_ptr<va::IFileSystemAccess> files = va::platform::make_file_system_access();

    TestRepository()
    {
        const auto root = std::filesystem::temp_directory_path() /
            ("va-repo-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        context.repo.root = root / "repository";
        context.config_file = root / "config.json";
        const auto initialised = va::initialise_repository(context, *writer);
        CHECK(initialised.has_value());
    }

    ~TestRepository()
    {
        std::error_code error;
        std::filesystem::remove_all(context.repo.root.parent_path(), error);
    }

    TestRepository(const TestRepository&) = delete;
    TestRepository& operator=(const TestRepository&) = delete;

    // A connection of its own, as another process would have; `lock_wait`:
    // how long its statements wait for another connection's write lock.
    va::ArchiveStore store(std::chrono::milliseconds lock_wait = va::statement_lock_wait) const
    {
        auto opened = va::ArchiveStore::open(context.repo, lock_wait);
        if (!opened) {
            std::cerr << "cannot open test repository: " << opened.error().message << '\n';
            std::abort();
        }
        return std::move(*opened);
    }

    // Raw SQL against the database, for tamper tests.
    void execute(const char* sql) const
    {
        sqlite3* database = nullptr;
        const auto name = context.repo.database().u8string();
        sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &database);
        CHECK(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(database);
    }

    // The first column of the first row of `sql` as text, read through a
    // connection of its own ("" for NULL, "-" when there is no row).
    std::string query(const std::string& sql) const
    {
        sqlite3* database = nullptr;
        const auto name = context.repo.database().u8string();
        std::string result = "-";
        if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READONLY,
                nullptr) == SQLITE_OK) {
            sqlite3_stmt* statement = nullptr;
            CHECK(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
            if (statement && sqlite3_step(statement) == SQLITE_ROW) {
                const auto* text = sqlite3_column_text(statement, 0);
                result = text ? reinterpret_cast<const char*>(text) : "";
            }
            sqlite3_finalize(statement);
        }
        sqlite3_close(database);
        return result;
    }
};

} // namespace test
