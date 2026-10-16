#include "core/repository/archive_store.h"

#include "common/canonical_json.h"
#include "common/time.h"
#include "core/hash/hashers.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <tuple>
#include <utility>

namespace va {
namespace {

using repository_detail::SqliteDatabase;
using repository_detail::SqliteStatement;

std::int64_t as_int(std::uint64_t value)
{
    return static_cast<std::int64_t>(value);
}

// Members of a setting's JSON, read without exceptions: a value of another
// type (a damaged setting) reads as missing, so it cannot hide the others.
std::string json_text(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::optional<std::uint64_t> json_unsigned(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    if (found == object.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

// The two numbers of a "SELECT COUNT(*), COALESCE(SUM(...), 0)" whose values
// are bound; the statement is reset for the next use.
std::expected<std::pair<std::uint64_t, std::uint64_t>, IoError> count_and_bytes(SqliteStatement& statement)
{
    const auto row = statement.step();
    if (!row) {
        return std::unexpected(row.error());
    }
    std::pair<std::uint64_t, std::uint64_t> result{0, 0};
    if (*row) {
        result = {static_cast<std::uint64_t>(statement.integer(0)), static_cast<std::uint64_t>(statement.integer(1))};
    }
    statement.reset();
    return result;
}

// True when the database is in WAL mode, which the repository sets up (R-91).
// The older rollback-journal mode stays where WAL could not be turned on, such
// as on a network share. There a reader's lock keeps a writer from committing
// for as long as it is held, and a writer waits for it at most 5 seconds
// (busy_timeout): a long read would make the operations' checkpoints fail.
bool write_ahead_log(const SqliteDatabase& database)
{
    auto mode = database.prepare("PRAGMA journal_mode;");
    if (!mode) {
        return false;
    }
    const auto row = mode->step();
    return row && *row && mode->text(0) == "wal";
}

// A read transaction: every query inside it sees the database as it was when
// the first one ran, even while another process commits checkpoints. Without
// it a run that finishes between two queries could show as half cleared up.
// DEFERRED takes no write lock, so the process doing the work never waits.
// Inside a transaction already open (read_transaction) it does nothing: the
// outer one holds.
//
// Only in WAL mode. In the rollback-journal mode one read across every run
// could outlast the writer's 5-second wait (write_ahead_log). Without the
// snapshot each query holds the lock only while it runs, and a run that
// finishes meanwhile shows for one read at most.
class ReadSnapshot {
public:
    explicit ReadSnapshot(const SqliteDatabase& database)
        : database_(database),
          open_(!database.in_transaction() && write_ahead_log(database) &&
                database.execute("BEGIN DEFERRED;").has_value())
    {
    }
    ~ReadSnapshot()
    {
        if (open_) {
            (void)database_.execute("COMMIT;");
        }
    }
    ReadSnapshot(const ReadSnapshot&) = delete;
    ReadSnapshot& operator=(const ReadSnapshot&) = delete;

private:
    const SqliteDatabase& database_;
    bool open_;
};

// A verification never recorded whose setting has said nothing for this long
// has stopped (its process was killed): a working run writes its "updated"
// every 30 seconds. A later verification of the same archive that started,
// worked or finished this long after the run last said anything supersedes it.
constexpr auto silent_before_superseded = std::chrono::minutes(5);

// True when `later` is at least that long after `since`; false when either is
// not known (a timestamp missing or unreadable).
bool silent_between(const std::optional<std::chrono::sys_seconds>& since,
    const std::optional<std::chrono::sys_seconds>& later)
{
    return since && later && *later - *since >= silent_before_superseded;
}

void bind_optional_text(SqliteStatement& statement, int index, const std::string& value)
{
    if (value.empty()) {
        statement.bind_null(index);
    } else {
        statement.bind(index, value);
    }
}

ArchiveRecord read_archive(const SqliteStatement& row)
{
    ArchiveRecord archive;
    archive.archive_uuid = row.text(0);
    archive.label = row.text(1);
    archive.created = row.text(2);
    archive.current_revision = static_cast<std::uint32_t>(row.integer(3));
    archive.status = row.text(4);
    archive.root_subpath = row.text(5);
    archive.notes = row.text(6);
    return archive;
}

RevisionRecord read_revision(const SqliteStatement& row)
{
    RevisionRecord revision;
    revision.archive_uuid = row.text(0);
    revision.revision = static_cast<std::uint32_t>(row.integer(1));
    revision.manifest_relpath = row.text(2);
    revision.header_json = row.text(3);
    revision.header_blake3 = row.text(4);
    revision.body_blake3 = row.text(5);
    revision.imported_from = row.text(6);
    revision.created = row.text(7);
    return revision;
}

VerifyRunRecord read_run(const SqliteStatement& row)
{
    VerifyRunRecord run;
    run.run_id = row.text(0);
    run.archive_uuid = row.text(1);
    run.revision = static_cast<std::uint32_t>(row.integer(2));
    run.mode = row.text(3);
    run.started = row.text(4);
    run.finished = row.text(5);
    run.result = row.text(6);
    if (!row.is_null(7)) {
        run.coverage_bytes = static_cast<std::uint64_t>(row.integer(7));
    }
    if (!row.is_null(8)) {
        run.coverage_chunks = static_cast<std::uint64_t>(row.integer(8));
    }
    run.sample_seed = row.text(9);
    run.counts_json = row.text(10);
    run.failure_classes = row.text(11);
    run.key_id = row.text(12);
    run.host = row.text(13);
    run.app_version = row.text(14);
    run.prev_hash = row.text(15);
    run.row_hash = row.text(16);
    return run;
}

constexpr const char* run_columns =
    "run_id, archive_uuid, revision, mode, started, finished, result, coverage_bytes, "
    "coverage_chunks, sample_seed, counts_json, failure_classes, key_id, host, app_version, "
    "prev_hash, row_hash";

} // namespace

std::string verify_run_hash_input(const VerifyRunRecord& run)
{
    canonical_json::ObjectWriter object;
    object.add_string("run_id", run.run_id)
        .add_string("archive_uuid", run.archive_uuid)
        .add_unsigned("revision", run.revision)
        .add_string("mode", run.mode)
        .add_string("started", run.started)
        .add_string("finished", run.finished)
        .add_string("result", run.result);
    if (run.coverage_bytes) {
        object.add_unsigned("coverage_bytes", *run.coverage_bytes);
    }
    if (run.coverage_chunks) {
        object.add_unsigned("coverage_chunks", *run.coverage_chunks);
    }
    const std::pair<const char*, const std::string*> optional_texts[] = {
        {"sample_seed", &run.sample_seed}, {"counts_json", &run.counts_json},
        {"failure_classes", &run.failure_classes}, {"key_id", &run.key_id},
        {"host", &run.host}, {"app_version", &run.app_version},
    };
    for (const auto& [name, value] : optional_texts) {
        if (!value->empty()) {
            object.add_string(name, *value);
        }
    }
    object.add_string("prev_hash", run.prev_hash);
    return object.str();
}

ArchiveStore::ArchiveStore(SqliteDatabase database) : database_(std::move(database)) {}

std::expected<ArchiveStore, IoError> ArchiveStore::open(
    const RepositoryLayout& layout, std::chrono::milliseconds lock_wait)
{
    auto database = SqliteDatabase::open(layout.database());
    if (!database) {
        return std::unexpected(database.error());
    }
    if (auto configured = database->execute(
            "PRAGMA busy_timeout=" + std::to_string(lock_wait.count()) + "; PRAGMA synchronous=NORMAL;");
        !configured) {
        return std::unexpected(configured.error());
    }
    return ArchiveStore(std::move(*database));
}

std::expected<ArchiveStore, IoError> ArchiveStore::open_read_only(const RepositoryLayout& layout)
{
    // SQLITE_OPEN_READONLY: a missing file is an error rather than a new,
    // empty database, and a read-only connection never writes the database
    // file, not even the WAL checkpoint the last writer to close would make.
    // query_only refuses a write by mistake as well.
    auto database = SqliteDatabase::open(layout.database(), true);
    if (!database) {
        return std::unexpected(database.error());
    }
    if (auto configured = database->execute("PRAGMA busy_timeout=5000; PRAGMA query_only=1;"); !configured) {
        return std::unexpected(configured.error());
    }
    return ArchiveStore(std::move(*database));
}

std::expected<void, IoError> ArchiveStore::begin()
{
    return database_.execute("BEGIN IMMEDIATE;");
}

std::expected<void, IoError> ArchiveStore::begin_waiting(
    std::chrono::steady_clock::duration patience, const CancelToken& cancel)
{
    const auto until = std::chrono::steady_clock::now() + patience;
    for (;;) {
        auto begun = begin();
        // SQLITE_BUSY: another connection still held the write lock when the
        // wait of this try (busy_timeout) was over.
        if (begun || (begun.error().native_code & 0xff) != SQLITE_BUSY ||
            std::chrono::steady_clock::now() >= until || cancel.cancelled()) {
            return begun;
        }
        // A pause, in case a try ever fails without waiting.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::expected<void, IoError> ArchiveStore::commit()
{
    return database_.execute("COMMIT;");
}

void ArchiveStore::rollback()
{
    (void)database_.execute("ROLLBACK;");
}

std::expected<void, IoError> ArchiveStore::write_transaction(
    const std::function<std::expected<void, IoError>()>& writes)
{
    if (auto begun = begin(); !begun) {
        return begun;
    }
    auto written = writes();
    if (written) {
        written = commit();
    }
    if (!written) {
        rollback();
    }
    return written;
}

std::expected<void, IoError> ArchiveStore::read_transaction(
    const std::function<std::expected<void, IoError>()>& reads)
{
    const ReadSnapshot snapshot(database_);
    return reads();
}

std::expected<std::string, IoError> ArchiveStore::setting(const std::string& key)
{
    auto statement = database_.prepare("SELECT value FROM settings WHERE key = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, key);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    return *row ? statement->text(0) : std::string{};
}

std::expected<void, IoError> ArchiveStore::set_setting(const std::string& key, const std::string& value)
{
    auto statement = database_.prepare(
        "INSERT INTO settings(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, key).bind(2, value).run();
}

std::expected<void, IoError> ArchiveStore::update_setting(const std::string& key, const std::string& value)
{
    auto statement = database_.prepare("UPDATE settings SET value = ? WHERE key = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, value).bind(2, key).run();
}

std::expected<void, IoError> ArchiveStore::update_registration_plan(const Uuid& archive_uuid,
    std::uint32_t revision, const Timestamp& created, const std::string& value)
{
    // The plan's members are read by SQLite's JSON functions; a damaged plan
    // (not JSON) is never this registration's, and CASE keeps json_extract
    // from reading it.
    auto statement = database_.prepare(
        "UPDATE settings SET value = ?1 WHERE key = ?2 "
        "AND (CASE WHEN json_valid(value) THEN json_extract(value, '$.revision') END) = ?3 "
        "AND (CASE WHEN json_valid(value) THEN json_extract(value, '$.created') END) = ?4 "
        "AND NOT EXISTS (SELECT 1 FROM archive_revisions WHERE archive_uuid = ?5 AND revision = ?3)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, value)
        .bind(2, "registration:" + archive_uuid)
        .bind(3, static_cast<std::int64_t>(revision))
        .bind(4, created)
        .bind(5, archive_uuid)
        .run();
}

std::expected<bool, IoError> ArchiveStore::registration_planned(const Uuid& archive_uuid, std::uint32_t revision,
    const Timestamp& created)
{
    const auto value = setting("registration:" + archive_uuid);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto plan = nlohmann::json::parse(*value, nullptr, false);
    return plan.is_object() && json_unsigned(plan, "revision") == std::optional<std::uint64_t>(revision) &&
           json_text(plan, "created") == created;
}

std::expected<void, IoError> ArchiveStore::remove_setting(const std::string& key)
{
    auto statement = database_.prepare("DELETE FROM settings WHERE key = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, key).run();
}

std::expected<std::optional<std::string>, IoError> ArchiveStore::registration_plan(
    const Uuid& archive_uuid, std::uint32_t revision)
{
    const auto value = setting("registration:" + archive_uuid);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto plan = nlohmann::json::parse(*value, nullptr, false);
    if (!plan.is_object() || json_unsigned(plan, "revision") != std::optional<std::uint64_t>(revision)) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>(json_text(plan, "root"));
}

std::expected<void, IoError> ArchiveStore::remove_registration_plan(const Uuid& archive_uuid, std::uint32_t revision)
{
    const auto plan = registration_plan(archive_uuid, revision);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (!*plan) {
        return {};
    }
    return remove_setting("registration:" + archive_uuid);
}

std::expected<void, IoError> ArchiveStore::update_relocation(const Uuid& archive_uuid, const std::string& run_id,
    std::uint32_t creates, const std::string& value)
{
    // As update_registration_plan: a damaged record (not JSON) is never this
    // relocation's, and CASE keeps json_extract from reading it.
    auto statement = database_.prepare(
        "UPDATE settings SET value = ?1 WHERE key = ?2 "
        "AND (CASE WHEN json_valid(value) THEN json_extract(value, '$.run_id') END) = ?3 "
        "AND NOT EXISTS (SELECT 1 FROM archive_revisions WHERE archive_uuid = ?4 AND revision = ?5)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, value)
        .bind(2, relocation_key(archive_uuid))
        .bind(3, run_id)
        .bind(4, archive_uuid)
        .bind(5, static_cast<std::int64_t>(creates))
        .run();
}

std::expected<bool, IoError> ArchiveStore::relocation_recorded(const Uuid& archive_uuid, const std::string& run_id)
{
    const auto value = setting(relocation_key(archive_uuid));
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto record = nlohmann::json::parse(*value, nullptr, false);
    return record.is_object() && !run_id.empty() && json_text(record, "run_id") == run_id;
}

std::expected<void, IoError> ArchiveStore::remove_relocation(const Uuid& archive_uuid, const std::string& run_id)
{
    const auto recorded = relocation_recorded(archive_uuid, run_id);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }
    if (*recorded) {
        if (auto removed = remove_setting(relocation_key(archive_uuid)); !removed) {
            return removed;
        }
    }
    // Its checkpoints are its own, whoever removed the record.
    return clear_progress(run_id);
}

std::expected<void, IoError> ArchiveStore::remove_relocation_creating(const Uuid& archive_uuid, std::uint32_t revision)
{
    const auto value = setting(relocation_key(archive_uuid));
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto record = nlohmann::json::parse(*value, nullptr, false);
    const auto verifies = record.is_object() ? json_unsigned(record, "revision") : std::nullopt;
    const auto run_id = record.is_object() ? json_text(record, "run_id") : std::string{};
    if (!verifies || *verifies + 1 != revision || run_id.empty()) {
        return {};
    }
    return remove_relocation(archive_uuid, run_id);
}

std::expected<std::vector<std::pair<std::string, std::string>>, IoError> ArchiveStore::settings_with_prefix(
    const std::string& prefix)
{
    auto statement = database_.prepare("SELECT key, value FROM settings WHERE substr(key, 1, ?) = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, static_cast<std::int64_t>(prefix.size())).bind(2, prefix);
    std::vector<std::pair<std::string, std::string>> result;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return result;
        }
        result.emplace_back(statement->text(0), statement->text(1));
    }
}

std::expected<std::string, IoError> ArchiveStore::next_label()
{
    const auto current = setting("next_label_seq");
    if (!current) {
        return std::unexpected(current.error());
    }
    std::uint64_t sequence = 1;
    if (!current->empty()) {
        try {
            sequence = std::stoull(*current);
        } catch (const std::exception&) {
            sequence = 1;
        }
    }
    if (auto saved = set_setting("next_label_seq", std::to_string(sequence + 1)); !saved) {
        return std::unexpected(saved.error());
    }
    return std::format("ARCH-{:04}", sequence);
}

std::expected<void, IoError> ArchiveStore::insert_archive(const ArchiveRecord& archive)
{
    auto statement = database_.prepare(
        "INSERT INTO archives(archive_uuid, label, created, current_revision, status, "
        "root_subpath, notes) VALUES(?, ?, ?, ?, ?, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive.archive_uuid)
        .bind(2, archive.label)
        .bind(3, archive.created)
        .bind(4, static_cast<std::int64_t>(archive.current_revision))
        .bind(5, archive.status)
        .bind(6, archive.root_subpath);
    bind_optional_text(*statement, 7, archive.notes);
    return statement->run();
}

std::expected<void, IoError> ArchiveStore::update_archive(const ArchiveRecord& archive)
{
    auto statement = database_.prepare(
        "UPDATE archives SET label = ?, current_revision = ?, status = ?, root_subpath = ?, "
        "notes = ? WHERE archive_uuid = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive.label)
        .bind(2, static_cast<std::int64_t>(archive.current_revision))
        .bind(3, archive.status)
        .bind(4, archive.root_subpath);
    bind_optional_text(*statement, 5, archive.notes);
    statement->bind(6, archive.archive_uuid);
    return statement->run();
}

namespace {

// The record of archive ?1 holds nothing but itself (decision 0025): it says
// that the archive never had a revision, and nothing refers to it. A record
// that says it has a revision, or one that a revision, a verification or
// media evidence refers to, never matches, whatever the index has lost.
constexpr const char* without_revision =
    "archive_uuid = ?1 AND current_revision = 0 AND status IN ('REGISTRATION_INCOMPLETE', 'RETIRED') "
    "AND NOT EXISTS (SELECT 1 FROM archive_revisions WHERE archive_uuid = ?1) "
    "AND NOT EXISTS (SELECT 1 FROM verify_runs WHERE archive_uuid = ?1) "
    "AND NOT EXISTS (SELECT 1 FROM media_evidence WHERE archive_uuid = ?1)";

} // namespace

std::expected<bool, IoError> ArchiveStore::archive_without_revision(const Uuid& archive_uuid)
{
    auto statement = database_.prepare(std::string("SELECT 1 FROM archives WHERE ") + without_revision);
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid);
    return statement->step();
}

std::expected<bool, IoError> ArchiveStore::remove_archive_without_revision(const Uuid& archive_uuid)
{
    auto statement = database_.prepare(std::string("DELETE FROM archives WHERE ") + without_revision);
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid);
    if (auto removed = statement->run(); !removed) {
        return std::unexpected(removed.error());
    }
    auto changes = database_.prepare("SELECT changes()");
    if (!changes) {
        return std::unexpected(changes.error());
    }
    const auto row = changes->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    return *row && changes->integer(0) > 0;
}

std::expected<std::optional<ArchiveRecord>, IoError> ArchiveStore::find_archive(const Uuid& archive_uuid)
{
    auto statement = database_.prepare(
        "SELECT archive_uuid, label, created, current_revision, status, root_subpath, notes "
        "FROM archives WHERE archive_uuid = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return std::optional<ArchiveRecord>{};
    }
    return std::optional<ArchiveRecord>(read_archive(*statement));
}

std::expected<std::vector<ArchiveRecord>, IoError> ArchiveStore::archives()
{
    auto statement = database_.prepare(
        "SELECT archive_uuid, label, created, current_revision, status, root_subpath, notes "
        "FROM archives ORDER BY label, archive_uuid");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    std::vector<ArchiveRecord> result;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return result;
        }
        result.push_back(read_archive(*statement));
    }
}

std::expected<void, IoError> ArchiveStore::insert_revision(const RevisionRecord& revision)
{
    auto statement = database_.prepare(
        "INSERT INTO archive_revisions(archive_uuid, revision, manifest_relpath, header_json, "
        "header_blake3, body_blake3, merkle_root, signed, key_id, imported_from, created) "
        "VALUES(?, ?, ?, ?, ?, ?, NULL, 0, NULL, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, revision.archive_uuid)
        .bind(2, static_cast<std::int64_t>(revision.revision))
        .bind(3, revision.manifest_relpath)
        .bind(4, revision.header_json)
        .bind(5, revision.header_blake3)
        .bind(6, revision.body_blake3);
    bind_optional_text(*statement, 7, revision.imported_from);
    statement->bind(8, revision.created);
    return statement->run();
}

std::expected<bool, IoError> ArchiveStore::revision_recorded(const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare("SELECT 1 FROM archive_revisions WHERE archive_uuid = ? AND revision = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision));
    return statement->step();
}

std::expected<std::vector<RevisionRecord>, IoError> ArchiveStore::revisions(const Uuid& archive_uuid)
{
    auto statement = database_.prepare(
        "SELECT archive_uuid, revision, manifest_relpath, header_json, header_blake3, "
        "body_blake3, imported_from, created FROM archive_revisions WHERE archive_uuid = ? "
        "ORDER BY revision");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid);
    std::vector<RevisionRecord> result;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return result;
        }
        result.push_back(read_revision(*statement));
    }
}

std::expected<std::optional<RevisionRecord>, IoError> ArchiveStore::revision(
    const Uuid& archive_uuid, std::uint32_t revision)
{
    auto all = revisions(archive_uuid);
    if (!all) {
        return std::unexpected(all.error());
    }
    for (auto& record : *all) {
        if (record.revision == revision) {
            return std::optional<RevisionRecord>(std::move(record));
        }
    }
    return std::optional<RevisionRecord>{};
}

std::expected<void, IoError> ArchiveStore::insert_evidence(const EvidenceRecord& evidence)
{
    auto statement = database_.prepare(
        "INSERT INTO media_evidence(archive_uuid, run_id, seen_at, volume_json, match_result, "
        "operator_note, disk_fingerprint) VALUES(?, ?, ?, ?, ?, ?, NULL)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    bind_optional_text(*statement, 1, evidence.archive_uuid);
    bind_optional_text(*statement, 2, evidence.run_id);
    statement->bind(3, evidence.seen_at).bind(4, evidence.volume_json).bind(5, evidence.match_result);
    bind_optional_text(*statement, 6, evidence.operator_note);
    return statement->run();
}

std::expected<void, IoError> ArchiveStore::insert_file_rows(
    const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries)
{
    return write_file_rows("INSERT", archive_uuid, revision, entries);
}

std::expected<void, IoError> ArchiveStore::upsert_file_rows(
    const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries)
{
    return write_file_rows("INSERT OR REPLACE", archive_uuid, revision, entries);
}

std::expected<std::vector<Entry>, IoError> ArchiveStore::file_rows(
    const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare(
        "SELECT path, raw_hex, stream, type, size, mtime_ns, b3, sha256 FROM files "
        "WHERE archive_uuid = ? AND revision = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision));
    std::vector<Entry> entries;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return entries;
        }
        Entry entry;
        entry.path.p = statement->text(0);
        entry.path.raw = statement->text(1);
        entry.stream = statement->text(2);
        const auto type = statement->text(3);
        entry.type = type.empty() ? 'f' : type[0];
        if (!statement->is_null(4)) {
            entry.size = static_cast<std::uint64_t>(statement->integer(4));
        }
        if (!statement->is_null(5)) {
            entry.mtime_ns = statement->integer(5);
        }
        entry.b3 = statement->text(6);
        entry.sha256 = statement->text(7);
        entries.push_back(std::move(entry));
    }
}

std::expected<bool, IoError> ArchiveStore::has_file_rows(const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare("SELECT 1 FROM files WHERE archive_uuid = ? AND revision = ? LIMIT 1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision));
    return statement->step();
}

std::expected<CheckpointCount, IoError> ArchiveStore::file_row_count(const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare(
        "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM files WHERE archive_uuid = ? AND revision = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision));
    const auto counted = count_and_bytes(*statement);
    if (!counted) {
        return std::unexpected(counted.error());
    }
    return CheckpointCount{counted->first, counted->second};
}

std::expected<void, IoError> ArchiveStore::write_file_rows(const char* verb,
    const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries)
{
    auto statement = database_.prepare(std::string(verb) +
        " INTO files(archive_uuid, revision, path, raw_hex, stream, type, size, mtime_ns, "
        "b3, sha256, flags) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    for (const auto& entry : entries) {
        statement->bind(1, archive_uuid)
            .bind(2, static_cast<std::int64_t>(revision))
            .bind(3, entry.path.p)
            .bind(4, entry.path.raw)
            .bind(5, entry.stream)
            .bind(6, std::string(1, entry.type));
        if (entry.size) {
            statement->bind(7, as_int(*entry.size));
        } else {
            statement->bind_null(7);
        }
        if (entry.mtime_ns) {
            statement->bind(8, *entry.mtime_ns);
        } else {
            statement->bind_null(8);
        }
        bind_optional_text(*statement, 9, entry.b3);
        bind_optional_text(*statement, 10, entry.sha256);
        bind_optional_text(*statement, 11, entry.unreadable.empty() ? "" : "unreadable:" + entry.unreadable);
        if (auto inserted = statement->run(); !inserted) {
            return inserted;
        }
    }
    return {};
}

std::expected<void, IoError> ArchiveStore::delete_file_rows(const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare("DELETE FROM files WHERE archive_uuid = ? AND revision = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision)).run();
}

std::expected<void, IoError> ArchiveStore::delete_file_rows(
    const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries)
{
    // By the primary key, one row each.
    auto statement = database_.prepare(
        "DELETE FROM files WHERE archive_uuid = ? AND revision = ? AND path = ? AND raw_hex = ? AND stream = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    for (const auto& entry : entries) {
        statement->bind(1, archive_uuid)
            .bind(2, static_cast<std::int64_t>(revision))
            .bind(3, entry.path.p)
            .bind(4, entry.path.raw)
            .bind(5, entry.stream);
        if (auto deleted = statement->run(); !deleted) {
            return deleted;
        }
    }
    return {};
}

std::expected<void, IoError> ArchiveStore::clear_index(const std::set<std::pair<Uuid, std::uint32_t>>& keep)
{
    if (keep.empty()) {
        return database_.execute("DELETE FROM files; DELETE FROM archive_revisions;");
    }
    if (auto cleared = database_.execute("DELETE FROM archive_revisions;"); !cleared) {
        return cleared;
    }
    // Every revision the files table holds rows of, read through its primary
    // key, then the rows of each that is not kept. Read to the end before the
    // first delete.
    auto held = database_.prepare("SELECT DISTINCT archive_uuid, revision FROM files");
    if (!held) {
        return std::unexpected(held.error());
    }
    std::vector<std::pair<Uuid, std::uint32_t>> revisions;
    for (;;) {
        const auto row = held->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            break;
        }
        revisions.emplace_back(held->text(0), static_cast<std::uint32_t>(held->integer(1)));
    }
    held->reset();
    for (const auto& revision : revisions) {
        if (keep.contains(revision)) {
            continue;
        }
        if (auto deleted = delete_file_rows(revision.first, revision.second); !deleted) {
            return deleted;
        }
    }
    return {};
}

std::expected<VerifyRunRecord, IoError> ArchiveStore::append_verify_run(
    VerifyRunRecord run, const std::string& repository_uuid)
{
    // The chain follows insertion order (rowid); the first row links to the
    // repository uuid (R-90).
    auto last = database_.prepare("SELECT row_hash FROM verify_runs ORDER BY rowid DESC LIMIT 1");
    if (!last) {
        return std::unexpected(last.error());
    }
    const auto has_last = last->step();
    if (!has_last) {
        return std::unexpected(has_last.error());
    }
    run.prev_hash = *has_last ? last->text(0) : repository_uuid;
    run.row_hash = blake3_hex(run.prev_hash + verify_run_hash_input(run));

    auto statement = database_.prepare(std::string("INSERT INTO verify_runs(") + run_columns +
                                       ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, run.run_id)
        .bind(2, run.archive_uuid)
        .bind(3, static_cast<std::int64_t>(run.revision))
        .bind(4, run.mode)
        .bind(5, run.started);
    bind_optional_text(*statement, 6, run.finished);
    statement->bind(7, run.result);
    if (run.coverage_bytes) {
        statement->bind(8, as_int(*run.coverage_bytes));
    } else {
        statement->bind_null(8);
    }
    if (run.coverage_chunks) {
        statement->bind(9, as_int(*run.coverage_chunks));
    } else {
        statement->bind_null(9);
    }
    bind_optional_text(*statement, 10, run.sample_seed);
    bind_optional_text(*statement, 11, run.counts_json);
    bind_optional_text(*statement, 12, run.failure_classes);
    bind_optional_text(*statement, 13, run.key_id);
    bind_optional_text(*statement, 14, run.host);
    bind_optional_text(*statement, 15, run.app_version);
    statement->bind(16, run.prev_hash).bind(17, run.row_hash);
    if (auto inserted = statement->run(); !inserted) {
        return std::unexpected(inserted.error());
    }
    return run;
}

std::expected<std::vector<VerifyRunRecord>, IoError> ArchiveStore::verify_runs(const Uuid& archive_uuid)
{
    auto statement = database_.prepare(std::string("SELECT ") + run_columns +
                                       " FROM verify_runs WHERE archive_uuid = ? ORDER BY rowid");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid);
    std::vector<VerifyRunRecord> result;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return result;
        }
        result.push_back(read_run(*statement));
    }
}

std::expected<HistoryCheck, IoError> ArchiveStore::check_history(const std::string& repository_uuid)
{
    auto statement = database_.prepare(std::string("SELECT ") + run_columns +
                                       " FROM verify_runs ORDER BY rowid");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    HistoryCheck check;
    std::string expected_prev = repository_uuid;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return check;
        }
        const auto run = read_run(*statement);
        ++check.runs;
        if (check.first_broken_run) {
            continue;
        }
        if (run.prev_hash != expected_prev) {
            check.first_broken_run = run.run_id;
            check.problem = "previous row was deleted or edited (prev_hash does not link)";
        } else if (blake3_hex(run.prev_hash + verify_run_hash_input(run)) != run.row_hash) {
            check.first_broken_run = run.run_id;
            check.problem = "row was edited (row_hash does not match its contents)";
        }
        expected_prev = run.row_hash;
    }
}

std::expected<void, IoError> ArchiveStore::insert_verify_items(const std::vector<VerifyItemRecord>& items)
{
    auto statement = database_.prepare(
        "INSERT OR REPLACE INTO verify_items(run_id, path, raw_hex, stream, status, detail) "
        "VALUES(?, ?, ?, ?, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    for (const auto& item : items) {
        statement->bind(1, item.run_id)
            .bind(2, item.path.p)
            .bind(3, item.path.raw)
            .bind(4, item.stream)
            .bind(5, item.status);
        bind_optional_text(*statement, 6, item.detail);
        if (auto inserted = statement->run(); !inserted) {
            return inserted;
        }
    }
    return {};
}

std::expected<std::vector<VerifyItemRecord>, IoError> ArchiveStore::verify_items(const std::string& run_id)
{
    auto statement = database_.prepare(
        "SELECT run_id, path, raw_hex, stream, status, detail FROM verify_items WHERE run_id = ? "
        "ORDER BY path, raw_hex, stream");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, run_id);
    std::vector<VerifyItemRecord> result;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return result;
        }
        VerifyItemRecord item;
        item.run_id = statement->text(0);
        item.path.p = statement->text(1);
        item.path.raw = statement->text(2);
        item.stream = statement->text(3);
        item.status = statement->text(4);
        item.detail = statement->text(5);
        result.push_back(std::move(item));
    }
}

std::expected<void, IoError> ArchiveStore::add_progress(
    const std::string& run_id, const std::vector<std::string>& entry_keys)
{
    auto statement = database_.prepare(
        "INSERT OR IGNORE INTO verify_progress(run_id, path, raw_hex, stream) VALUES(?, ?, ?, ?)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    for (const auto& key : entry_keys) {
        // entry_key(): p '\0' raw '\0' stream
        const auto first = key.find('\0');
        const auto second = key.find('\0', first + 1);
        statement->bind(1, run_id)
            .bind(2, std::string_view(key).substr(0, first))
            .bind(3, std::string_view(key).substr(first + 1, second - first - 1))
            .bind(4, std::string_view(key).substr(second + 1));
        if (auto inserted = statement->run(); !inserted) {
            return inserted;
        }
    }
    return {};
}

std::expected<std::set<std::string>, IoError> ArchiveStore::progress(const std::string& run_id)
{
    auto statement = database_.prepare(
        "SELECT path, raw_hex, stream FROM verify_progress WHERE run_id = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, run_id);
    std::set<std::string> keys;
    for (;;) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return keys;
        }
        std::string key = statement->text(0);
        key += '\0';
        key += statement->text(1);
        key += '\0';
        key += statement->text(2);
        keys.insert(std::move(key));
    }
}

std::expected<CheckpointCount, IoError> ArchiveStore::progress_count(
    const std::string& run_id, const Uuid& archive_uuid, std::uint32_t revision)
{
    auto statement = database_.prepare(
        "SELECT COUNT(*), COALESCE(SUM(f.size), 0) FROM verify_progress AS p "
        "LEFT JOIN files AS f ON f.archive_uuid = ? AND f.revision = ? AND f.path = p.path "
        "AND f.raw_hex = p.raw_hex AND f.stream = p.stream WHERE p.run_id = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, archive_uuid).bind(2, static_cast<std::int64_t>(revision)).bind(3, run_id);
    const auto counted = count_and_bytes(*statement);
    if (!counted) {
        return std::unexpected(counted.error());
    }
    return CheckpointCount{counted->first, counted->second};
}

std::expected<void, IoError> ArchiveStore::clear_progress(const std::string& run_id)
{
    auto statement = database_.prepare("DELETE FROM verify_progress WHERE run_id = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, run_id).run();
}

std::expected<std::vector<PendingRegistration>, IoError> ArchiveStore::pending_registrations()
{
    const ReadSnapshot snapshot(database_);
    // Counting is the one long statement of a read (write_ahead_log).
    const bool count = write_ahead_log(database_);
    const auto plans = settings_with_prefix("registration:");
    if (!plans) {
        return std::unexpected(plans.error());
    }
    // The archive row is written together with the plan (register_engine.cpp);
    // for a new revision it is the existing archive, with the same root.
    auto archive = database_.prepare("SELECT root_subpath FROM archives WHERE archive_uuid = ?");
    // Checkpoints hold only hashed files, so every row counts.
    auto hashed = database_.prepare(
        "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM files WHERE archive_uuid = ? AND revision = ?");
    if (!archive || !hashed) {
        return std::unexpected(!archive ? archive.error() : hashed.error());
    }

    std::vector<PendingRegistration> result;
    for (const auto& [key, value] : *plans) {
        const auto plan = nlohmann::json::parse(value, nullptr, false);
        if (!plan.is_object()) {
            continue;
        }
        PendingRegistration registration;
        registration.archive_uuid = json_text(plan, "archive_uuid");
        const auto revision = json_unsigned(plan, "revision");
        if (registration.archive_uuid.empty() || !revision) {
            continue; // not a plan this version wrote
        }
        registration.revision = static_cast<std::uint32_t>(*revision);
        registration.label = json_text(plan, "label");
        registration.root = json_text(plan, "root");
        registration.started = json_text(plan, "created");
        registration.files_total = json_unsigned(plan, "files_total");
        registration.bytes_total = json_unsigned(plan, "bytes_total");
        registration.updated = json_text(plan, "updated");
        registration.stopped = json_text(plan, "stopped");
        registration.plan = value;

        archive->bind(1, registration.archive_uuid);
        const auto found = archive->step();
        if (!found) {
            return std::unexpected(found.error());
        }
        registration.volume_root = *found && archive->text(0).empty();
        archive->reset();

        registration.done_counted = count;
        if (count) {
            hashed->bind(1, registration.archive_uuid).bind(2, static_cast<std::int64_t>(registration.revision));
            const auto done = count_and_bytes(*hashed);
            if (!done) {
                return std::unexpected(done.error());
            }
            std::tie(registration.files_done, registration.bytes_done) = *done;
        }
        result.push_back(std::move(registration));
    }
    std::ranges::sort(result, {}, [](const PendingRegistration& registration) {
        return std::tie(registration.started, registration.label, registration.archive_uuid);
    });
    return result;
}

std::expected<std::vector<UnfinishedVerification>, IoError> ArchiveStore::unfinished_verifications(
    std::chrono::sys_seconds now, const std::map<std::string, CheckpointCount>& counted)
{
    const ReadSnapshot snapshot(database_);
    // Counting is the one long statement of a read (write_ahead_log).
    const bool count = write_ahead_log(database_);
    const auto runs = settings_with_prefix("run:");
    if (!runs) {
        return std::unexpected(runs.error());
    }
    auto recorded = database_.prepare("SELECT result, rowid, finished FROM verify_runs WHERE run_id = ?");
    auto archive = database_.prepare("SELECT label, current_revision FROM archives WHERE archive_uuid = ?");
    // Every run of an archive ever recorded, in the order recorded (rowid).
    // verify_runs holds one row per verification, so this stays small.
    auto history =
        database_.prepare("SELECT rowid, started, finished, result FROM verify_runs WHERE archive_uuid = ?");
    auto header = database_.prepare(
        "SELECT header_json FROM archive_revisions WHERE archive_uuid = ? AND revision = ?");
    // Each checkpointed entry's size comes from the files index of the revision,
    // looked up by its primary key.
    auto matched = database_.prepare(
        "SELECT COUNT(*), COALESCE(SUM(f.size), 0) FROM verify_progress AS p "
        "LEFT JOIN files AS f ON f.archive_uuid = ? AND f.revision = ? AND f.path = p.path "
        "AND f.raw_hex = p.raw_hex AND f.stream = p.stream WHERE p.run_id = ?");
    for (const auto* statement : {&recorded, &archive, &history, &header, &matched}) {
        if (!*statement) {
            return std::unexpected(statement->error());
        }
    }

    // First what the settings and the history rows say about each run. The
    // costly part, counting checkpoints, comes last and only for the runs shown.
    struct Candidate {
        UnfinishedVerification run;
        bool totals_recorded = false;  // the setting has files_total and bytes_total
        std::int64_t row = 0;          // its history row, when recorded INCOMPLETE
        Timestamp stopped;             // when it was recorded INCOMPLETE
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, value] : *runs) {
        const auto setting = nlohmann::json::parse(value, nullptr, false);
        if (!setting.is_object()) {
            continue;
        }
        Candidate candidate;
        auto& run = candidate.run;
        run.run_id = key.substr(std::string_view("run:").size());
        run.archive_uuid = json_text(setting, "archive_uuid");
        const auto revision = json_unsigned(setting, "revision");
        if (run.archive_uuid.empty() || !revision) {
            continue;
        }
        run.revision = static_cast<std::uint32_t>(*revision);
        run.started = json_text(setting, "started");
        run.root = json_text(setting, "root");
        run.updated = json_text(setting, "updated");
        const auto files_total = json_unsigned(setting, "files_total");
        const auto bytes_total = json_unsigned(setting, "bytes_total");
        candidate.totals_recorded = files_total && bytes_total;
        run.files_total = files_total.value_or(0);
        run.bytes_total = bytes_total.value_or(0);

        // A run is recorded when it ends. An INCOMPLETE one keeps its setting
        // for --resume; any other result removes it in the same transaction.
        recorded->bind(1, run.run_id);
        const auto has_row = recorded->step();
        if (!has_row) {
            return std::unexpected(has_row.error());
        }
        const bool finished = *has_row && recorded->text(0) != "INCOMPLETE";
        run.recorded_incomplete = *has_row && !finished;
        if (run.recorded_incomplete) {
            candidate.row = recorded->integer(1);
            candidate.stopped = recorded->text(2);
        }
        recorded->reset();
        if (finished) {
            continue;
        }

        archive->bind(1, run.archive_uuid);
        const auto has_archive = archive->step();
        if (!has_archive) {
            return std::unexpected(has_archive.error());
        }
        run.label = *has_archive ? archive->text(0) : std::string{};
        // R-104: a run resumes only while its archive is at the revision it verifies.
        run.resumable = *has_archive && static_cast<std::uint32_t>(archive->integer(1)) == run.revision;
        archive->reset();
        // Stopped, and cannot be resumed: nothing will ever finish it. A run
        // never recorded has stopped once its record has said nothing for
        // minutes (its process was killed); until then it may still work and
        // is listed. One of an older version, without "updated", may still
        // work as well.
        const bool silent = silent_between(parse_timestamp(run.updated), now);
        if (!run.resumable && (run.recorded_incomplete || silent)) {
            continue;
        }
        candidates.push_back(std::move(candidate));
    }

    // The history of each archive with an unfinished run.
    struct Recorded {
        std::int64_t row;   // rowid: rows are numbered in the order recorded
        Timestamp started;
        Timestamp finished;
        std::string result;
    };
    std::map<Uuid, std::vector<Recorded>> histories;
    for (const auto& candidate : candidates) {
        const auto& archive_uuid = candidate.run.archive_uuid;
        if (histories.contains(archive_uuid)) {
            continue;
        }
        auto& rows = histories[archive_uuid];
        history->bind(1, archive_uuid);
        for (;;) {
            const auto row = history->step();
            if (!row) {
                return std::unexpected(row.error());
            }
            if (!*row) {
                break;
            }
            rows.push_back({history->integer(0), history->text(1), history->text(2), history->text(3)});
        }
        history->reset();
    }

    // A later verification of the same archive supersedes a run that stopped:
    // a finished one gives the archive a newer result, and a newer attempt is
    // the one to follow or resume. Without this, every cancelled run would be
    // listed for good.
    const auto superseded = [&](const Candidate& candidate) {
        const auto& run = candidate.run;
        const auto& rows = histories.at(run.archive_uuid);
        const auto same_archive = [&](const Candidate& other) {
            return &other != &candidate && other.run.archive_uuid == run.archive_uuid;
        };
        if (run.recorded_incomplete) {
            // Any run of the archive recorded after it (rowids grow), or one
            // started after it stopped that is not recorded yet.
            return std::ranges::any_of(rows, [&](const Recorded& row) { return row.row > candidate.row; }) ||
                   std::ranges::any_of(candidates, [&](const Candidate& other) {
                       return same_archive(other) && !other.run.recorded_incomplete &&
                              other.run.started > candidate.stopped;
                   });
        }
        // Never recorded: it runs now, or its process was killed. It counts
        // as stopped and superseded when another verification of the archive
        // started, worked (its "updated") or was recorded when this one had
        // been silent for minutes: a restart two minutes after the kill that
        // has since finished, or still works, is the one to follow. A run
        // that works says so every 30 seconds, so nothing is that much later
        // than it and it is never left out.
        if (!run.updated.empty()) {
            const auto silent_since = parse_timestamp(run.updated);
            // Silent for minutes now, so it was killed: a verification started
            // after it last said anything that PASSED (a small archive's,
            // within minutes of the kill) has done its work. One that FAILED
            // at once (the wrong media) has not, nor has one that ran beside
            // it.
            const bool killed = silent_between(silent_since, now);
            const auto passed_after = [&](const Recorded& row) {
                const auto started = parse_timestamp(row.started);
                return (row.result == "PASSED" || row.result == "PASSED_WITH_WARNINGS") && silent_since &&
                       started && *started > *silent_since;
            };
            return std::ranges::any_of(rows,
                       [&](const Recorded& row) {
                           // Recorded when it ended, never before it started.
                           return silent_between(silent_since,
                                      parse_timestamp(row.finished.empty() ? row.started : row.finished)) ||
                                  (killed && passed_after(row));
                       }) ||
                   std::ranges::any_of(candidates, [&](const Candidate& other) {
                       return same_archive(other) &&
                              (silent_between(silent_since, parse_timestamp(other.run.started)) ||
                                  silent_between(silent_since, parse_timestamp(other.run.updated)));
                   });
        }
        // A run of an older version never says it works, so nothing after its
        // start tells that it stopped: only a verification started minutes
        // after it supersedes it.
        const auto started = parse_timestamp(run.started);
        return std::ranges::any_of(rows,
                   [&](const Recorded& row) { return silent_between(started, parse_timestamp(row.started)); }) ||
               std::ranges::any_of(candidates, [&](const Candidate& other) {
                   return same_archive(other) && silent_between(started, parse_timestamp(other.run.started));
               });
    };
    std::vector<bool> shown;
    shown.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        shown.push_back(!superseded(candidate));
    }

    std::vector<UnfinishedVerification> result;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!shown[index]) {
            continue;
        }
        auto& candidate = candidates[index];
        auto& run = candidate.run;
        // Runs from before decision 0022 have no totals and no root in their
        // setting: the manifest header has the revision's counts (unreadable
        // entries included) and where it was registered.
        if (!candidate.totals_recorded || run.root.empty()) {
            header->bind(1, run.archive_uuid).bind(2, static_cast<std::int64_t>(run.revision));
            const auto has_header = header->step();
            if (!has_header) {
                return std::unexpected(has_header.error());
            }
            if (*has_header) {
                const auto json = nlohmann::json::parse(header->text(0), nullptr, false);
                const auto counts = json.is_object() ? json.find("counts") : json.end();
                if (!candidate.totals_recorded && counts != json.end() && counts->is_object()) {
                    run.files_total = json_unsigned(*counts, "files").value_or(0) +
                                      json_unsigned(*counts, "streams").value_or(0);
                    run.bytes_total = json_unsigned(*counts, "bytes").value_or(0);
                }
                if (run.root.empty() && json.is_object()) {
                    run.root = json_text(json, "root_hint");
                }
            }
            header->reset();
        }

        // Not in WAL mode, not counted at all (write_ahead_log). A run
        // recorded INCOMPLETE never changes again: an earlier count holds.
        run.done_counted = count;
        const auto known = counted.find(run.run_id);
        if (count && run.recorded_incomplete && known != counted.end()) {
            run.files_done = known->second.files;
            run.bytes_done = known->second.bytes;
        } else if (count) {
            matched->bind(1, run.archive_uuid)
                .bind(2, static_cast<std::int64_t>(run.revision))
                .bind(3, run.run_id);
            const auto done = count_and_bytes(*matched);
            if (!done) {
                return std::unexpected(done.error());
            }
            std::tie(run.files_done, run.bytes_done) = *done;
        }
        result.push_back(std::move(run));
    }
    std::ranges::sort(result, {}, [](const UnfinishedVerification& run) {
        return std::tie(run.started, run.label, run.run_id);
    });
    return result;
}

std::expected<std::vector<UnfinishedRelocation>, IoError> ArchiveStore::unfinished_relocations()
{
    const ReadSnapshot snapshot(database_);
    // Counting is the one long statement of a read (write_ahead_log).
    const bool count = write_ahead_log(database_);
    const auto records = settings_with_prefix("relocation:");
    if (!records) {
        return std::unexpected(records.error());
    }
    auto archive = database_.prepare("SELECT label, current_revision FROM archives WHERE archive_uuid = ?");
    // As a verification's checkpoints: each entry's size from the index of
    // the revision it verifies against, looked up by its primary key.
    auto matched = database_.prepare(
        "SELECT COUNT(*), COALESCE(SUM(f.size), 0) FROM verify_progress AS p "
        "LEFT JOIN files AS f ON f.archive_uuid = ? AND f.revision = ? AND f.path = p.path "
        "AND f.raw_hex = p.raw_hex AND f.stream = p.stream WHERE p.run_id = ?");
    if (!archive || !matched) {
        return std::unexpected(!archive ? archive.error() : matched.error());
    }

    std::vector<UnfinishedRelocation> result;
    for (const auto& [key, value] : *records) {
        const auto record = nlohmann::json::parse(value, nullptr, false);
        if (!record.is_object()) {
            continue;
        }
        UnfinishedRelocation relocation;
        relocation.archive_uuid = json_text(record, "archive_uuid");
        relocation.run_id = json_text(record, "run_id");
        const auto revision = json_unsigned(record, "revision");
        if (relocation.archive_uuid.empty() || relocation.run_id.empty() || !revision) {
            continue; // not a record this version wrote
        }
        relocation.revision = static_cast<std::uint32_t>(*revision);
        relocation.label = json_text(record, "label");
        relocation.root = json_text(record, "root");
        relocation.started = json_text(record, "started");
        relocation.files_total = json_unsigned(record, "files_total").value_or(0);
        relocation.bytes_total = json_unsigned(record, "bytes_total").value_or(0);
        relocation.updated = json_text(record, "updated");
        relocation.stopped = json_text(record, "stopped");
        relocation.differs = json_text(record, "differs");
        relocation.record = value;

        archive->bind(1, relocation.archive_uuid);
        const auto found = archive->step();
        if (!found) {
            return std::unexpected(found.error());
        }
        if (*found && relocation.label.empty()) {
            relocation.label = archive->text(0);
        }
        // It resumes only while its archive is at the revision it verifies,
        // and never once its new carrier was found to differ.
        relocation.resumable = *found && static_cast<std::uint32_t>(archive->integer(1)) == relocation.revision &&
                               relocation.differs.empty();
        archive->reset();

        relocation.done_counted = count;
        if (count) {
            matched->bind(1, relocation.archive_uuid)
                .bind(2, static_cast<std::int64_t>(relocation.revision))
                .bind(3, relocation.run_id);
            const auto done = count_and_bytes(*matched);
            if (!done) {
                return std::unexpected(done.error());
            }
            std::tie(relocation.files_done, relocation.bytes_done) = *done;
        }
        result.push_back(std::move(relocation));
    }
    std::ranges::sort(result, {}, [](const UnfinishedRelocation& relocation) {
        return std::tie(relocation.started, relocation.label, relocation.archive_uuid);
    });
    return result;
}

std::expected<std::string, IoError> ArchiveStore::archive_list_mark()
{
    const ReadSnapshot snapshot(database_);
    // Revisions and runs are only ever added (R-18): their count and newest
    // row change when one is.
    auto added = database_.prepare(
        "SELECT (SELECT COUNT(*) FROM archive_revisions), (SELECT COALESCE(MAX(rowid), 0) FROM archive_revisions), "
        "(SELECT COUNT(*) FROM verify_runs), (SELECT COALESCE(MAX(rowid), 0) FROM verify_runs)");
    // Every column the list shows or works out from (archive_statuses): the
    // label (shared or not), the revision whose header it shows, the status,
    // the root, and when a never verified archive is overdue from.
    auto archives = database_.prepare(
        "SELECT archive_uuid, label, created, current_revision, status, root_subpath FROM archives "
        "ORDER BY archive_uuid");
    if (!added || !archives) {
        return std::unexpected(!added ? added.error() : archives.error());
    }
    std::string mark;
    // Each value with its length in front, so no two lists of values read alike.
    const auto add = [&](std::string_view value) {
        mark += std::to_string(value.size());
        mark += ':';
        mark += value;
    };
    const auto counts = added->step();
    if (!counts) {
        return std::unexpected(counts.error());
    }
    for (int column = 0; *counts && column < 4; ++column) {
        add(std::to_string(added->integer(column)));
    }
    // Done with it: without a read transaction (not in WAL mode) a statement
    // left running would keep its lock.
    added->reset();
    for (;;) {
        const auto row = archives->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return mark;
        }
        for (int column = 0; column < 6; ++column) {
            add(archives->text(column));
        }
    }
}

} // namespace va
