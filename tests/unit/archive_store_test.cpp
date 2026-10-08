#include "test_support.h"

#include "core/repository/archive_store.h"
#include "core/repository/repository.h"
#include "platform/file_writer.h"

#include <chrono>
#include <sqlite3.h>

namespace {

struct TestRepository {
    va::AppContext context;

    TestRepository()
    {
        const auto root = std::filesystem::temp_directory_path() /
            ("va-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        context.repo.root = root / "repository";
        context.config_file = root / "config.json";
        const auto initialised = va::initialise_repository(context, *va::platform::make_file_writer());
        CHECK(initialised.has_value());
    }

    ~TestRepository()
    {
        std::error_code error;
        std::filesystem::remove_all(context.repo.root.parent_path(), error);
    }

    void execute(const char* sql) const
    {
        sqlite3* database = nullptr;
        const auto name = context.repo.database().u8string();
        sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &database);
        CHECK(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(database);
    }
};

va::VerifyRunRecord run(std::string id, std::string result)
{
    va::VerifyRunRecord record;
    record.run_id = std::move(id);
    record.archive_uuid = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";
    record.revision = 1;
    record.mode = "FULL";
    record.started = "2026-10-08T00:00:00Z";
    record.finished = "2026-10-08T01:00:00Z";
    record.result = std::move(result);
    record.coverage_bytes = 123;
    record.counts_json = "{\"verified\":3}";
    return record;
}

void archives_revisions_and_labels()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    CHECK(store.has_value());
    if (!store) {
        return;
    }
    const auto first_label = store->next_label();
    const auto second_label = store->next_label();
    CHECK(first_label && *first_label == "ARCH-0001");
    CHECK(second_label && *second_label == "ARCH-0002");

    va::ArchiveRecord archive{"28ff645e-a220-4aac-ae0b-6c328bf1a2b3", "ARCH-0001",
        "2026-10-08T00:00:00Z", 1, "REGISTERED", "Backups/2026", ""};
    CHECK(store->insert_archive(archive));
    archive.status = "RETIRED";
    CHECK(store->update_archive(archive));
    const auto found = store->find_archive(archive.archive_uuid);
    CHECK(found && *found && (*found)->status == "RETIRED" && (*found)->root_subpath == "Backups/2026");
    CHECK(store->archives() && store->archives()->size() == 1);

    va::RevisionRecord revision{archive.archive_uuid, 1, "manifests/x/r0001.vam", "{}", "h", "b", "",
        "2026-10-08T00:00:00Z"};
    CHECK(store->insert_revision(revision));
    revision.revision = 2;
    CHECK(store->insert_revision(revision));
    const auto revisions = store->revisions(archive.archive_uuid);
    CHECK(revisions && revisions->size() == 2 && revisions->back().revision == 2);

    va::Entry entry;
    entry.path.p = "a.txt";
    entry.size = 5;
    entry.b3 = "b3";
    CHECK(store->insert_file_rows(archive.archive_uuid, 1, {entry}));
    CHECK(store->insert_evidence({archive.archive_uuid, "", "2026-10-08T00:00:00Z", "{}", "MATCH", ""}));
}

// R-18 / R-90: edited or deleted history rows are detected.
void history_chain_detects_tampering()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    if (!store) {
        CHECK(false);
        return;
    }
    const auto& repository_uuid = repository.context.repository_uuid;
    const auto first = store->append_verify_run(run("run-1", "PASSED"), repository_uuid);
    CHECK(first && first->prev_hash == repository_uuid);
    const auto second = store->append_verify_run(run("run-2", "FAILED"), repository_uuid);
    CHECK(second && first && second->prev_hash == first->row_hash);
    CHECK(store->append_verify_run(run("run-3", "PASSED"), repository_uuid));

    auto check = store->check_history(repository_uuid);
    CHECK(check && check->runs == 3 && !check->first_broken_run);

    repository.execute("UPDATE verify_runs SET result = 'PASSED' WHERE run_id = 'run-2'");
    check = store->check_history(repository_uuid);
    CHECK(check && check->first_broken_run == std::optional<std::string>("run-2"));

    repository.execute("UPDATE verify_runs SET result = 'FAILED' WHERE run_id = 'run-2'");
    repository.execute("DELETE FROM verify_runs WHERE run_id = 'run-2'");
    check = store->check_history(repository_uuid);
    CHECK(check && check->first_broken_run == std::optional<std::string>("run-3"));
}

void items_and_progress()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    if (!store) {
        CHECK(false);
        return;
    }
    va::VerifyItemRecord item;
    item.run_id = "run-1";
    item.path.p = "a.txt";
    item.status = "Modified";
    CHECK(store->insert_verify_items({item}));
    const auto items = store->verify_items("run-1");
    CHECK(items && items->size() == 1 && items->front().status == "Modified");

    va::Entry entry;
    entry.path.p = "dir/b.txt";
    entry.path.raw = "ff";
    CHECK(store->add_progress("run-1", {va::entry_key(entry)}));
    auto progress = store->progress("run-1");
    CHECK(progress && progress->contains(va::entry_key(entry)));
    CHECK(store->clear_progress("run-1"));
    progress = store->progress("run-1");
    CHECK(progress && progress->empty());
}

} // namespace

void archive_store_tests()
{
    archives_revisions_and_labels();
    history_chain_detects_tampering();
    items_and_progress();
}
