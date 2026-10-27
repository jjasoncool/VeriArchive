#pragma once
// ArchiveStore: reads and writes the repository database (HANDOFF §B8). The
// database is an index and history store, never an integrity source (R-14):
// verification always compares against validated manifest bytes.
#include "common/archive_path.h"
#include "common/error.h"
#include "common/repository_layout.h"
#include "common/types.h"
#include "core/interfaces.h"
#include "core/manifest/entry.h"
#include "core/repository/sqlite_raii.h"

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
    std::string counts_json;            // the item counts ("items"), and what the run says of itself (read_no_media)
    std::string failure_classes;        // comma-separated run-level failure classes
    std::string key_id;
    std::string host;
    std::string app_version;
    std::string prev_hash;              // filled in by append_verify_run
    std::string row_hash;               // filled in by append_verify_run
};

// Whether a recorded run read no media, by its counts (`counts_json`): those
// of a verification that failed on its manifest before it looked for the
// media say "media_read": false (decision 0028). Every other run read it.
bool read_no_media(std::string_view counts_json);

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
    std::string plan;                           // the plan as stored (JSON), for judging which process
                                                // runs its attempt (decision 0024)
    bool later_revision = false;                // made for an older revision: the archive has a later one
                                                // than the plan was made for, so it can never finish and
                                                // waits to be abandoned (decision 0030)
    bool archive_retired = false;               // a registration over a lost manifest (its plan names
                                                // "lost_manifest") of an archive retired since, as a
                                                // version before decision 0028, point 9 let one begin: a
                                                // retired archive takes no new revision, so it can never
                                                // finish and waits to be abandoned
};

// A verification that has not finished: running now (it has no history row
// yet), or recorded INCOMPLETE and waiting for `verify --resume`. It is the
// setting "run:<run_id>" (decision 0022); the entries whose content matched
// so far are checkpointed into verify_progress (R-104).
struct UnfinishedVerification {
    std::string run_id;
    Uuid archive_uuid;
    std::string label;
    std::string root;                           // where it runs, from the setting (UTF-8); empty in the setting
                                                // of a run of a version before decision 0022
    std::uint32_t revision = 0;                 // the revision being verified
    Timestamp started;
    bool recorded_incomplete = false;           // its history row says INCOMPLETE: it stopped
    bool resumable = true;                      // false when the archive has another revision now (R-104)
    std::uint64_t files_done = 0;               // files (and streams) read and matched so far
    std::uint64_t bytes_done = 0;               // their registered bytes
    bool done_counted = true;                   // false: the two above were not counted (not in WAL mode)
    bool totals_recorded = true;                // the setting has the two below; a run of a version before
                                                // decision 0022 has neither (nor a root): its revision's
                                                // manifest header has them (read_unfinished_operations)
    std::uint64_t files_total = 0;              // the files (and streams) with content to read
    std::uint64_t bytes_total = 0;              // the bytes their hashes cover: the bytes to read
    Timestamp updated;                          // the setting's "updated": when it last said it was working
    bool archive_retired = false;               // its archive is retired (decision 0003): where that archive's
                                                // latest manifest is lost, which this layer, reading no files,
                                                // leaves to read_unfinished_operations, the run can never be
                                                // resumed and is not listed (decision 0022, point 5)
};

// The setting that holds the record of an unfinished relocation of an archive
// (decision 0026): one per archive, never a verification's "run:<run_id>".
inline std::string relocation_key(const Uuid& archive_uuid)
{
    return "relocation:" + archive_uuid;
}

// The setting that marks an archive whose revision's manifest the last `repo
// rebuild-index` skipped, as one it could not read (decision 0028): its value
// names that revision ({"revision":N}). While it exists, the archive lists
// read the archive's manifest as the rebuild reads one, whole; each rebuild
// replaces the marks of the one before.
inline const std::string skipped_manifest_prefix = "rebuild-skipped:";
inline std::string skipped_manifest_key(const Uuid& archive_uuid)
{
    return skipped_manifest_prefix + archive_uuid;
}

// A relocation that has not finished (decision 0026): running now, in this or
// another process, or interrupted and waiting for `archive relocate --resume`.
// Its record is the setting relocation_key(archive_uuid); the entries of the
// new carrier whose content matched so far are checkpointed into
// verify_progress under its run_id (R-104), as a verification's are.
struct UnfinishedRelocation {
    Uuid archive_uuid;
    std::string label;
    std::string root;                           // UTF-8: the new root, where its latest attempt reads
    std::uint32_t revision = 0;                 // the revision it verifies against; it creates the next one
    std::string run_id;                         // its checkpoints, and its FULL verification's id once recorded
    Timestamp started;                          // when its first attempt started
    std::uint64_t files_done = 0;               // files (and streams) read and matched so far
    std::uint64_t bytes_done = 0;               // their recorded bytes
    bool done_counted = true;                   // false: the two above were not counted (not in WAL mode)
    std::uint64_t files_total = 0;              // the files (and streams) of the revision with content hashes
    std::uint64_t bytes_total = 0;              // their bytes
    Timestamp updated;                          // the record's "updated": when it last said it was working
    Timestamp stopped;                          // the record's "stopped": when an attempt ended without finishing
    Timestamp differs;                          // the record's "differs": when an attempt found that the new
                                                // carrier does not hold the recorded content; empty otherwise
    bool resumable = true;                      // false when the archive has another revision now, or the
                                                // record says "differs"
    bool later_revision = false;                // made for an older revision: the archive has a later one
                                                // than the one it verifies, so it can never finish and
                                                // waits to be abandoned (decision 0030)
    std::string record;                         // the record as stored (JSON), for judging which process runs
                                                // its attempt (decision 0024)
};

// Files (and streams) and their bytes, counted from a run's checkpoints.
struct CheckpointCount {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

// How long a statement waits for the write lock that another connection holds
// (busy_timeout) before it fails with "database is locked".
inline constexpr std::chrono::milliseconds statement_lock_wait{5000};
// How long the record of a registration or a relocation waits for the write
// lock (ArchiveStore::begin_waiting). Another operation's record of a large
// archive holds it for seconds, about three per million entries (its index),
// longer than a statement waits; the record of an operation that has done
// hours of work waits for it rather than fail (decision 0023).
inline constexpr std::chrono::minutes record_lock_wait{2};

class ArchiveStore {
public:
    // `lock_wait`: how long each statement waits for the write lock; shorter
    // only in tests.
    static std::expected<ArchiveStore, IoError> open(
        const RepositoryLayout& layout, std::chrono::milliseconds lock_wait = statement_lock_wait);
    // Opens the existing database for reading only, for a process that only
    // watches (the GUI's Archive List): a missing database is an error, never
    // created, and closing never writes to it.
    static std::expected<ArchiveStore, IoError> open_read_only(const RepositoryLayout& layout);

    // Groups many writes in one transaction (R-91: never one commit per file).
    std::expected<void, IoError> begin();
    // As begin(), for the record of an operation that has done its work: while
    // another connection holds the write lock, it tries again until `patience`
    // has passed or `cancel` is cancelled, where begin() fails once a
    // statement's wait is over.
    std::expected<void, IoError> begin_waiting(
        std::chrono::steady_clock::duration patience, const CancelToken& cancel);
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

    // Labels: "ARCH-" + zero-padded sequence, assigned by this repository
    // (R-33), skipping the numbers of the labels it has already, whatever
    // gave them (decision 0036).
    std::expected<std::string, IoError> next_label();

    std::expected<void, IoError> insert_archive(const ArchiveRecord& archive);
    std::expected<void, IoError> update_archive(const ArchiveRecord& archive);
    // True while the archive's record holds nothing but itself: the archive
    // never had a revision, so its record says revision 0 and that it was
    // never registered (REGISTRATION_INCOMPLETE, or RETIRED by the operator
    // since), and no revision, verification or media evidence refers to it
    // (decision 0025). Never the index alone (R-14): the record of an
    // archive whose index lost its revisions (a damaged database) still says
    // its revision.
    std::expected<bool, IoError> archive_without_revision(const Uuid& archive_uuid);
    // Removes the archive's record, in one statement, only while
    // archive_without_revision holds, as when its first registration is
    // abandoned (decision 0025); every other record is left alone. True when
    // it was removed.
    std::expected<bool, IoError> remove_archive_without_revision(const Uuid& archive_uuid);
    std::expected<std::optional<ArchiveRecord>, IoError> find_archive(const Uuid& archive_uuid);
    std::expected<std::vector<ArchiveRecord>, IoError> archives();

    std::expected<void, IoError> insert_revision(const RevisionRecord& revision);
    // True when this revision of the archive is recorded. Inside a write
    // transaction the answer holds until it ends: no other connection can
    // record a revision meanwhile (decision 0023).
    std::expected<bool, IoError> revision_recorded(const Uuid& archive_uuid, std::uint32_t revision);
    // The revision the archive is at, when it is later than `made_for` while
    // the revision after `made_for` is not recorded: an unfinished
    // registration or relocation made for revision `made_for` creates that
    // revision, and another operation, such as an import of a later revision,
    // has given the archive a later one meanwhile. That operation can never
    // finish: recording its revision would set the archive back (R-15,
    // decision 0030). Nothing otherwise, also for an archive without a
    // record. Inside a write transaction the answer holds until it ends.
    std::expected<std::optional<std::uint32_t>, IoError> later_revision(
        const Uuid& archive_uuid, std::uint32_t made_for);
    std::expected<std::vector<RevisionRecord>, IoError> revisions(const Uuid& archive_uuid);
    std::expected<std::optional<RevisionRecord>, IoError> revision(
        const Uuid& archive_uuid, std::uint32_t revision);
    // Removes the record of this revision of the archive and its index (its
    // rows of files), and nothing else: the record of the revision whose
    // manifest was lost, once a registration over that lost manifest records
    // the next revision (decision 0028, point 8), as `repo rebuild-index`
    // leaves out an earlier revision whose manifest it cannot read. Inside a
    // write transaction.
    std::expected<void, IoError> remove_revision(const Uuid& archive_uuid, std::uint32_t revision);

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
    // True when the files table holds a row under this revision of the
    // archive: its index once the revision is recorded, before that the
    // checkpoints of a registration creating it (R-104).
    std::expected<bool, IoError> has_file_rows(const Uuid& archive_uuid, std::uint32_t revision);
    // The rows under this revision of the archive and their bytes: before the
    // revision is recorded, the files a registration creating it has hashed
    // and saved (its checkpoints).
    std::expected<CheckpointCount, IoError> file_row_count(const Uuid& archive_uuid, std::uint32_t revision);
    // Empties the rebuildable index tables (archive_revisions, files) for
    // rebuild-index, except the record of each revision in `kept` (archive,
    // revision) with its rows in files, whose manifest rebuild-index could
    // not read (decision 0028), and the rows in files of `saved_progress`:
    // the saved progress of unfinished registrations (decision 0026).
    std::expected<void, IoError> clear_index(const std::set<std::pair<Uuid, std::uint32_t>>& kept = {},
        const std::set<std::pair<Uuid, std::uint32_t>>& saved_progress = {});

    std::expected<VerifyRunRecord, IoError> append_verify_run(
        VerifyRunRecord run, const std::string& repository_uuid);
    std::expected<std::vector<VerifyRunRecord>, IoError> verify_runs(const Uuid& archive_uuid);
    // The run recorded under `run_id`, or nothing when there is none.
    std::expected<std::optional<VerifyRunRecord>, IoError> verify_run(const std::string& run_id);
    std::expected<HistoryCheck, IoError> check_history(const std::string& repository_uuid);

    std::expected<void, IoError> insert_verify_items(const std::vector<VerifyItemRecord>& items);
    std::expected<std::vector<VerifyItemRecord>, IoError> verify_items(const std::string& run_id);

    // Resume checkpoints (R-104): keys of entries already verified in a run.
    std::expected<void, IoError> add_progress(
        const std::string& run_id, const std::vector<std::string>& entry_keys);
    std::expected<std::set<std::string>, IoError> progress(const std::string& run_id);
    // The entries checkpointed under `run_id` and their bytes, each entry's
    // size from the index of `revision` of the archive (the revision the run
    // verifies against).
    std::expected<CheckpointCount, IoError> progress_count(
        const std::string& run_id, const Uuid& archive_uuid, std::uint32_t revision);
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
    // anything that PASSED (with warnings or not). A verification that read
    // no media (read_no_media) supersedes none (decision 0028). A verification
    // of a retired archive is kept as any, and marked (archive_retired): a
    // retired archive can still be verified and its runs resumed (decision
    // 0003), but not where its latest manifest is lost (decision 0028, point
    // 9), which read_unfinished_operations judges from the manifest. `counted`:
    // the checkpoints of runs recorded INCOMPLETE, counted by an earlier read;
    // such a run never changes, so it is not counted again.
    std::expected<std::vector<UnfinishedVerification>, IoError> unfinished_verifications(
        std::chrono::sys_seconds now, const std::map<std::string, CheckpointCount>& counted = {});
    // Every relocation whose record exists (decision 0026), its checkpoints
    // counted as a verification's are (each entry's size from the index of
    // the revision it verifies against), and only in WAL mode.
    std::expected<std::vector<UnfinishedRelocation>, IoError> unfinished_relocations();
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
    // Writes the plan of a registration (the setting
    // "registration:<archive_uuid>"), in one statement, only while the key
    // holds this registration's plan, the one that creates `revision` and was
    // made at `created`, and that revision is not recorded. Once it is
    // recorded, the plan is done with and the key may hold the plan of the
    // archive's next registration (decision 0023); once the registration is
    // abandoned, the plan is gone, or the key holds a later registration's
    // plan (decision 0025). Neither may be written.
    std::expected<void, IoError> update_registration_plan(const Uuid& archive_uuid, std::uint32_t revision,
        const Timestamp& created, const std::string& value);
    // True while the key "registration:<archive_uuid>" holds this
    // registration's plan: the one that creates `revision`, made at
    // `created`. Inside a write transaction the answer holds until it ends.
    std::expected<bool, IoError> registration_planned(const Uuid& archive_uuid, std::uint32_t revision,
        const Timestamp& created);
    std::expected<void, IoError> remove_setting(const std::string& key);
    // The plan of an unfinished registration of the archive (the setting
    // "registration:<archive_uuid>", decision 0022) when it creates
    // `revision`: the root it registers (UTF-8). Nothing when there is no
    // plan, or it is the plan of another revision.
    std::expected<std::optional<std::string>, IoError> registration_plan(
        const Uuid& archive_uuid, std::uint32_t revision);
    // Removes that plan, and leaves alone a plan of another revision, such as
    // the one of the archive's next registration. Once the revision is
    // recorded the plan can never be finished (decision 0023). Inside a write
    // transaction, so that the plan read is the plan removed.
    std::expected<void, IoError> remove_registration_plan(const Uuid& archive_uuid, std::uint32_t revision);
    // Writes the record of a relocation (the setting relocation_key), in one
    // statement, only while the key holds this relocation's record (the one
    // with `run_id`) and the revision it creates, `creates`, is not recorded:
    // once it is, or the relocation was abandoned, the key holds no record or
    // another relocation's (decision 0026).
    std::expected<void, IoError> update_relocation(const Uuid& archive_uuid, const std::string& run_id,
        std::uint32_t creates, const std::string& value);
    // True while the key relocation_key(archive_uuid) holds the record of the
    // relocation with `run_id`. Inside a write transaction the answer holds
    // until it ends.
    std::expected<bool, IoError> relocation_recorded(const Uuid& archive_uuid, const std::string& run_id);
    // Removes the record of the relocation with `run_id` and its checkpoints
    // (decision 0026); leaves alone a record of another relocation under the
    // key. Inside a write transaction, so that the record read is the record
    // removed.
    std::expected<void, IoError> remove_relocation(const Uuid& archive_uuid, const std::string& run_id);
    // Removes the record and the checkpoints of an unfinished relocation of
    // the archive that creates `revision` (it verifies against the revision
    // before), if the key holds one: once that revision is recorded, by
    // whatever records it, the relocation can never finish (decision 0026).
    std::expected<void, IoError> remove_relocation_creating(const Uuid& archive_uuid, std::uint32_t revision);
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
