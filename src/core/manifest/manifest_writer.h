#pragma once
// Writes rNNNN.vam (R-61): entries are streamed to a temporary body file of
// this writer's own while its BLAKE3 and length are computed; finish() then
// writes header + body to a temporary file next to the destination and makes
// it durable. Nothing appears at the destination until the caller commits that
// file, which only renames it: in the transaction that records the revision,
// once it has found the revision not recorded, so that a recorded manifest is
// never replaced (decision 0023).
#include "core/hash/hashers.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"

#include <memory>

namespace va {

struct WrittenManifest {
    ManifestHeader header;      // with counts and body fields filled in
    std::string header_line;    // line 1 without '\n' (what V1.1 signs, R-60)
    std::string header_blake3;  // BLAKE3 of header_line
    // The manifest, durable but not at the destination yet: commit() renames
    // it there. Destroyed without a commit, its temporary file is deleted.
    std::unique_ptr<IOutputFile> file;
};

class ManifestWriter {
public:
    ManifestWriter(IFileWriter& writer, IFileSystemAccess& reader, std::filesystem::path destination);

    // Entries must arrive in canonical order (R-43).
    std::expected<void, IoError> add(const Entry& entry);
    // The body file is gone when it returns, whether it succeeded or not.
    std::expected<WrittenManifest, IoError> finish(ManifestHeader header);

private:
    std::expected<void, IoError> open_body();

    IFileWriter& writer_;
    IFileSystemAccess& reader_;
    std::filesystem::path destination_;
    std::filesystem::path body_path_;
    std::unique_ptr<IOutputFile> body_;
    Blake3Hasher body_hash_;
    ManifestCounts counts_;
    std::uint64_t body_length_ = 0;
    std::uint64_t body_lines_ = 0;
    std::optional<Entry> previous_;
};

// Path of revision `revision` of an archive inside the repository (R-63).
std::filesystem::path manifest_path(
    const std::filesystem::path& manifests_root, const Uuid& archive_uuid, std::uint32_t revision);

} // namespace va
