#include "core/repository/archive_store.h"

#include "common/canonical_json.h"
#include "core/hash/hashers.h"

#include <format>

namespace va {
namespace {

using repository_detail::SqliteDatabase;
using repository_detail::SqliteStatement;

std::int64_t as_int(std::uint64_t value)
{
    return static_cast<std::int64_t>(value);
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

std::expected<ArchiveStore, IoError> ArchiveStore::open(const RepositoryLayout& layout)
{
    auto database = SqliteDatabase::open(layout.database());
    if (!database) {
        return std::unexpected(database.error());
    }
    if (auto configured = database->execute("PRAGMA busy_timeout=5000; PRAGMA synchronous=NORMAL;");
        !configured) {
        return std::unexpected(configured.error());
    }
    return ArchiveStore(std::move(*database));
}

std::expected<void, IoError> ArchiveStore::begin()
{
    return database_.execute("BEGIN IMMEDIATE;");
}

std::expected<void, IoError> ArchiveStore::commit()
{
    return database_.execute("COMMIT;");
}

void ArchiveStore::rollback()
{
    (void)database_.execute("ROLLBACK;");
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

std::expected<void, IoError> ArchiveStore::remove_setting(const std::string& key)
{
    auto statement = database_.prepare("DELETE FROM settings WHERE key = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, key).run();
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
    auto statement = database_.prepare(
        "INSERT INTO files(archive_uuid, revision, path, raw_hex, stream, type, size, mtime_ns, "
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

std::expected<void, IoError> ArchiveStore::clear_index()
{
    return database_.execute("DELETE FROM files; DELETE FROM archive_revisions;");
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

std::expected<void, IoError> ArchiveStore::clear_progress(const std::string& run_id)
{
    auto statement = database_.prepare("DELETE FROM verify_progress WHERE run_id = ?");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, run_id).run();
}

} // namespace va
