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

#include <expected>
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

class ArchiveStore {
public:
    static std::expected<ArchiveStore, IoError> open(const RepositoryLayout& layout);

    // Groups many writes in one transaction (R-91: never one commit per file).
    std::expected<void, IoError> begin();
    std::expected<void, IoError> commit();
    void rollback();

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

    std::expected<std::string, IoError> setting(const std::string& key);
    std::expected<void, IoError> set_setting(const std::string& key, const std::string& value);
    std::expected<void, IoError> remove_setting(const std::string& key);

private:
    explicit ArchiveStore(repository_detail::SqliteDatabase database);
    repository_detail::SqliteDatabase database_;
};

// The canonical JSON a verify run's row_hash covers (R-90), exposed for tests.
std::string verify_run_hash_input(const VerifyRunRecord& run);

} // namespace va
