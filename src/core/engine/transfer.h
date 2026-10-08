#pragma once
// Moving manifests between repositories (R-87): export copies a validated
// rNNNN.vam plus identity.json; import validates a manifest, enforces rollback
// protection (R-15) and indexes it. Labels may collide after import (R-88).
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <string>

namespace va {

struct ExportResult {
    std::filesystem::path manifest;
    std::filesystem::path identity;
};

// Fails with the manifest failure class when the stored manifest is not the one
// the repository recorded.
std::expected<ExportResult, std::string> export_archive(ArchiveStore& store,
    IFileSystemAccess& repository_files, IFileWriter& writer, const RepositoryLayout& layout,
    const Uuid& archive_uuid, const std::filesystem::path& out_directory);

struct ImportResult {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    bool already_present = false;   // the same bytes were already in the repository
    bool new_archive = false;
};

struct ImportFailure {
    std::string failure_class;      // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError
    std::string message;
};

std::expected<ImportResult, ImportFailure> import_manifest(ArchiveStore& store,
    IFileSystemAccess& files, IFileWriter& writer, const RepositoryLayout& layout,
    const std::filesystem::path& manifest, bool force_rollback);

// identity.json (§B1.2 ArchiveIdentity), written by export and by the media copy (R-89).
std::string identity_file_json(const Uuid& archive_uuid, const std::string& label,
    std::uint32_t revision, const std::string& root_subpath, const std::string& created);

// Streams `source` into `destination` through IFileWriter (atomic, durable).
std::expected<void, IoError> copy_file(IFileSystemAccess& files, IFileWriter& writer,
    const std::filesystem::path& source, const std::filesystem::path& destination);

} // namespace va
