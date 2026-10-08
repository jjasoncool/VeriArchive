#pragma once
// Loads a revision's manifest and accepts it only when its bytes are the ones
// the repository recorded (R-14): header and body BLAKE3 equal the database
// record, the body matches its own header (R-62), and it is not older than the
// highest known revision (R-15).
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <string>
#include <vector>

namespace va {

struct RecordedManifest {
    ManifestHeader header;
    std::vector<Entry> entries;
};

struct ManifestLoadFailure {
    std::string failure_class;  // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError
    std::string message;
};

std::expected<RecordedManifest, ManifestLoadFailure> load_recorded_manifest(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_uuid,
    std::uint32_t revision);

} // namespace va
