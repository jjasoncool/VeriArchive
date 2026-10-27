#pragma once
// Moving manifests between repositories (R-87): export copies a validated
// rNNNN.vam plus identity.json; import validates a manifest, enforces rollback
// protection (R-15) and indexes it. Labels may collide after import (R-88).
// An imported manifest gets its name in the transaction that records its
// revision, as a registration's and a relocation's do (decision 0030).
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <string>

namespace va {

struct ExportResult {
    std::filesystem::path manifest;
    std::filesystem::path identity;
};

struct ExportFailure {
    std::string message;
    bool cancelled = false;  // nothing was written
};

// Fails with the manifest failure class when the stored manifest is not the one
// the repository recorded. R-94: `progress` counts the manifest's bytes
// copied; `cancel` is asked between the pieces copied, and a cancelled export
// writes nothing (its copy, not yet at its name, goes).
std::expected<ExportResult, ExportFailure> export_archive(ArchiveStore& store,
    IFileSystemAccess& repository_files, IFileWriter& writer, const RepositoryLayout& layout,
    const Uuid& archive_uuid, const std::filesystem::path& out_directory, CancelToken& cancel,
    IProgressSink& progress);

struct ImportResult {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    bool already_present = false;   // the same bytes were already in the repository
    bool new_archive = false;
    // The revision was recorded, and its manifest in the repository was
    // missing, could not be read or was not the one recorded: the file
    // imported, which is the one recorded, was put back at its place
    // (decision 0028). The record and the index stay as they were.
    bool restored = false;
};

struct ImportFailure {
    // Failed: the manifest cannot be imported. Refused: another operation
    // recorded its revision while this import ran, or an unfinished
    // registration with saved files creates it (decision 0030), or the record
    // of the revision whose manifest it restores changed meanwhile (decision
    // 0028); nothing was imported (exit 12). Cancelled: by the operator
    // (R-94), and nothing was imported.
    enum class Kind { Failed, Refused, Cancelled };
    std::string failure_class;      // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError; empty when refused
    std::string message;
    Kind kind = Kind::Failed;
};

// Copies the manifest under a temporary name and flushes it without a lock,
// then, in one write transaction that waits for the write lock as a record
// does (decision 0023), finds its revision not recorded, writes the records
// and the index, and names the manifest last: a recorded manifest is never
// replaced. In the same transaction it removes the plan of a registration and
// the record of a relocation that create the revision, as their own records do.
//
// A revision that is recorded already is compared with the manifest that the
// repository holds (R-87): the same bytes are already present. Where that
// manifest is missing, cannot be read or differs, the file restores it if it
// is the one recorded (its line 1 has the BLAKE3 recorded for it, and its body
// is the one that line names, decision 0028): staged the same way, it is
// named in a write transaction that finds the record still naming that line,
// and nothing else changes. Any other file is a ManifestHashMismatch.
//
// R-94: `progress` counts the entries read (items), then the manifest's bytes
// copied; `cancel` is asked between them, between the pieces copied and while
// the record waits for the write lock. A cancelled import records nothing,
// and its copy, not yet at its name, goes (Kind::Cancelled).
std::expected<ImportResult, ImportFailure> import_manifest(ArchiveStore& store,
    IFileSystemAccess& files, IFileWriter& writer, const RepositoryLayout& layout,
    const std::filesystem::path& manifest, bool force_rollback, CancelToken& cancel, IProgressSink& progress);

// identity.json (§B1.2 ArchiveIdentity), written by export and by the media copy (R-89).
std::string identity_file_json(const Uuid& archive_uuid, const std::string& label,
    std::uint32_t revision, const std::string& root_subpath, const std::string& created);

// Streams `source` into `destination` through IFileWriter (atomic, durable).
std::expected<void, IoError> copy_file(IFileSystemAccess& files, IFileWriter& writer,
    const std::filesystem::path& source, const std::filesystem::path& destination);

} // namespace va
