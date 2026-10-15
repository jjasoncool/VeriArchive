#pragma once
// ArchiveStore: reads and writes the repository database (HANDOFF §B8). The
// database is an index and history store, never an integrity source (R-14):
// verification always compares against validated manifest bytes.
#include "common/archive_path.h"
#include "common/error.h"
#include "common/repository_layout.h"
#include "common/types.h"
#include "core/manifest/entry.h"
#include "core/repository/sqlite_raii.h"

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace va {

struct ArchiveRecord {
    Uuid archive_uuid;
    std::string label;
    Timestamp created;
    std::uint32_t current_revision = 0;
    std::string status;                 // REGISTERED | REGISTRATION_INCOMPLETE | RETIRED
    std::string root_subpath;
    std::string notes;
};

struct RevisionRecord {
    Uuid archive_uuid;
    std::uint32_t revision = 0;
    std::string manifest_relpath;       // relative to RepositoryRoot, '/'-separated
    std::string header_json;            // line 1 of the manifest
    std::string header_blake3;
    std::string body_blake3;
    std::string imported_from;
    Timestamp created;
};

struct EvidenceRecord {
    Uuid archive_uuid;                  // empty when no archive was selected
    std::string run_id;
    Timestamp seen_at;
    std::string volume_json;            // raw VolumeInfo (R-96)
    std::string match_result;           // MATCH, MATCH_WEAK, ...
    std::string operator_note;
};

// One verification run. Rows are append-only and hash-chained (R-18, R-90):
// row_hash = BLAKE3(prev_hash + canonical JSON of the row without row_hash).
struct VerifyRunRecord {
    std::string run_id;
    Uuid archive_uuid;
    std::uint32_t revision = 0;
    std::string mode;                   // FULL (V1), QUICK, DEEP
    Timestamp started;
    Timestamp finished;
    std::string result;                 // PASSED | PASSED_WITH_WARNINGS | FAILED | INCOMPLETE
    std::optional<std::uint64_t> coverage_bytes;
    std::optional<std::uint64_t> coverage_chunks;
    std::string sample_seed;
    std::string counts_json;
    std::string failure_classes;        // comma-separated run-level failure classes
    std::string key_id;
    std::string host;
    std::string app_version;
    std::string prev_hash;              // filled in by append_verify_run
    std::string row_hash;               // filled in by append_verify_run
};

struct VerifyItemRecord {
    std::string run_id;
    ArchivePath path;
    std::string stream;
    std::string status;                 // Modified, Missing, New, Unreadable(media), ...
    std::string detail;
};

// Result of recomputing the history chain (repo verify-history).
struct HistoryCheck {
    std::size_t runs = 0;
    std::optional<std::string> first_broken_run; // edited row or a deleted predecessor
    std::string problem;
};

// A registration that has not finished (R-104): running now, in this or another
// process, or interrupted and waiting for `register --resume`. Its plan is the
// setting "registration:<archive_uuid>" (decision 0022); the files hashed so
// far are checkpointed into the files table under the revision being created.
struct PendingRegistration {
    Uuid archive_uuid;
    std::string label;
    std::string root;                           // UTF-8, as the registration was given it
    bool volume_root = false;                   // the root is a whole volume (empty root subpath)
    std::uint32_t revision = 0;                 // the revision being created
    Timestamp started;                          // when the plan was made (the first attempt)
    std::uint64_t files_done = 0;               // files hashed and checkpointed so far
    std::uint64_t bytes_done = 0;               // their bytes
    bool done_counted = true;                   // false: the two above were not counted (not in WAL mode)
    std::optional<std::uint64_t> files_total;   // from the plan, once the scan has counted them
    std::optional<std::uint64_t> bytes_total;
    Timestamp updated;                          // the plan's "updated": when it last said it was working
    Timestamp stopped;                          // the plan's "stopped": when the attempt ended without
                                                // finishing (cancelled, failed); empty while it may run
};

// A verification that has not finished: running now (it has no history row
// yet), or recorded INCOMPLETE and waiting for `verify --resume`. It is the
// setting "run:<run_id>" (decision 0022); the entries whose content matched
// so far are checkpointed into verify_progress (R-104).
struct UnfinishedVerification {
    std::string run_id;
    Uuid archive_uuid;
    std::string label;
    std::string root;                           // where it runs when recorded, else where it was registered
    std::uint32_t revision = 0;                 // the revision being verified
    Timestamp started;
    bool recorded_incomplete = false;           // its history row says INCOMPLETE: it stopped
    bool resumable = true;                      // false when the archive has another revision now (R-104)
    std::uint64_t files_done = 0;               // files (and streams) read and matched so far
    std::uint64_t bytes_done = 0;               // their registered bytes
    bool done_counted = true;                   // false: the two above were not counted (not in WAL mode)
    std::uint64_t files_total = 0;              // the files (and streams) with content to read: from the
                                                // setting, or for older runs the manifest header's counts
                                                // (which include those unreadable at registration)
    std::uint64_t bytes_total = 0;              // the bytes their hashes cover: the bytes to read
    Timestamp updated;                          // the setting's "updated": when it last said it was working
};

// Files (and streams) and their bytes, counted from a run's checkpoints.
struct CheckpointCount {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

class ArchiveStore {
public:
    static std::expected<ArchiveStore, IoError> open(const RepositoryLayout& layout);
    // Opens the existing database for reading only, for a process that only
    // watches (the GUI's Archive List): a missing database is an error, never
    // created, and closing never writes to it.
    static std::expected<ArchiveStore, IoError> open_read_only(const RepositoryLayout& layout);

    // Groups many writes in one transaction (R-91: never one commit per file).
    std::expected<void, IoError> begin();
    std::expected<void, IoError> commit();
    void rollback();
    // Runs `writes` in one transaction. When they or the commit fail, the
    // transaction is rolled back and the error returned: none of the writes
    // stay, and the next transaction can begin. (A transaction left open
    // after a failed COMMIT would take every later write into it.)
    std::expected<void, IoError> write_transaction(const std::function<std::expected<void, IoError>()>& writes);
    // Runs `reads` in one read transaction, so that they all see the database
    // as it was when the first query ran, also while another process commits
    // (the reads below that take their own include). It takes no write lock,
    // so the process doing the work never waits. Only in WAL mode (R-91): in
    // the rollback-journal mode a reader's lock keeps writers from committing
    // for as long as it is held, so there each query reads on its own.
    std::expected<void, IoError> read_transaction(const std::function<std::expected<void, IoError>()>& reads);

    // Labels: "ARCH-" + zero-padded sequence, assigned by this repository (R-33).
    std::expected<std::string, IoError> next_label();

    std::expected<void, IoError> insert_archive(const ArchiveRecord& archive);
    std::expected<void, IoError> update_archive(const ArchiveRecord& archive);
    std::expected<std::optional<ArchiveRecord>, IoError> find_archive(const Uuid& archive_uuid);
    std::expected<std::vector<ArchiveRecord>, IoError> archives();

    std::expected<void, IoError> insert_revision(const RevisionRecord& revision);
    std::expected<std::vector<RevisionRecord>, IoError> revisions(const Uuid& archive_uuid);
    std::expected<std::optional<RevisionRecord>, IoError> revision(
        const Uuid& archive_uuid, std::uint32_t revision);

    std::expected<void, IoError> insert_evidence(const EvidenceRecord& evidence);

    // Rebuildable index of manifest entries (files table, R-80).
    std::expected<void, IoError> insert_file_rows(
        const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries);
    std::expected<void, IoError> delete_file_rows(const Uuid& archive_uuid, std::uint32_t revision);
    // Registration checkpoints (R-104): hashed entries of an unfinished revision.
    std::expected<void, IoError> upsert_file_rows(
        const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries);
    // Deletes the rows of these entries only (path, raw and stream), such as
    // checkpoints a resumed registration does not reuse.
    std::expected<void, IoError> delete_file_rows(
        const Uuid& archive_uuid, std::uint32_t revision, const std::vector<Entry>& entries);
    std::expected<std::vector<Entry>, IoError> file_rows(const Uuid& archive_uuid, std::uint32_t revision);
    // Empties the rebuildable index tables (archive_revisions, files) for rebuild-index.
    std::expected<void, IoError> clear_index();

    std::expected<VerifyRunRecord, IoError> append_verify_run(
        VerifyRunRecord run, const std::string& repository_uuid);
    std::expected<std::vector<VerifyRunRecord>, IoError> verify_runs(const Uuid& archive_uuid);
    std::expected<HistoryCheck, IoError> check_history(const std::string& repository_uuid);

    std::expected<void, IoError> insert_verify_items(const std::vector<VerifyItemRecord>& items);
    std::expected<std::vector<VerifyItemRecord>, IoError> verify_items(const std::string& run_id);

    // Resume checkpoints (R-104): keys of entries already verified in a run.
    std::expected<void, IoError> add_progress(
        const std::string& run_id, const std::vector<std::string>& entry_keys);
    std::expected<std::set<std::string>, IoError> progress(const std::string& run_id);
    std::expected<void, IoError> clear_progress(const std::string& run_id);

    // Unfinished operations, for showing their progress while another process
    // may be running them. Read only. Each operation costs one pass over its
    // own checkpoint rows through the primary keys (files: archive_uuid,
    // revision; verify_progress: run_id), never a scan of the whole table.
    // Not in WAL mode, the checkpoints are not counted at all (done_counted is
    // false): one count holds the reader's lock for its whole length, and a
    // writer waits for it at most 5 seconds, so counting a large operation's
    // checkpoints would make its own checkpoints and its final record fail.
    std::expected<std::vector<PendingRegistration>, IoError> pending_registrations();
    // Verifications that cannot be resumed or that a later verification of the
    // same archive supersedes are left out (decision 0022): one whose archive
    // has another revision now (R-104 refuses its resume) and that was
    // recorded INCOMPLETE, or never recorded and silent for minutes at `now`
    // (its process was killed); and one that a later verification of the same
    // archive supersedes: for a run recorded INCOMPLETE, one recorded after it
    // or started after it stopped; for a run never recorded, one that started,
    // worked or finished when it had been silent for minutes, or, once it has
    // been silent for minutes at `now`, one started after it last said
    // anything that PASSED (with warnings or not). `counted`: the checkpoints
    // of runs recorded INCOMPLETE, counted by an earlier read; such a run
    // never changes, so it is not counted again.
    std::expected<std::vector<UnfinishedVerification>, IoError> unfinished_verifications(
        std::chrono::sys_seconds now, const std::map<std::string, CheckpointCount>& counted = {});
    // A mark of what the Archive List shows of the archives (archive_statuses):
    // it changes whenever an archive is added or changes its label, revision,
    // root or status, a revision is recorded, or a verification is recorded.
    // Cheap: one row per archive, and a count and the newest row of the
    // revisions and the verifications.
    std::expected<std::string, IoError> archive_list_mark();

    std::expected<std::string, IoError> setting(const std::string& key);
    std::expected<void, IoError> set_setting(const std::string& key, const std::string& value);
    // Changes the value of a setting that exists; does nothing when it does not.
    std::expected<void, IoError> update_setting(const std::string& key, const std::string& value);
    std::expected<void, IoError> remove_setting(const std::string& key);
    std::expected<std::vector<std::pair<std::string, std::string>>, IoError> settings_with_prefix(
        const std::string& prefix);

private:
    explicit ArchiveStore(repository_detail::SqliteDatabase database);
    std::expected<void, IoError> write_file_rows(const char* verb, const Uuid& archive_uuid,
        std::uint32_t revision, const std::vector<Entry>& entries);
    repository_detail::SqliteDatabase database_;
};

// The canonical JSON a verify run's row_hash covers (R-90), exposed for tests.
std::string verify_run_hash_input(const VerifyRunRecord& run);

} // namespace va
