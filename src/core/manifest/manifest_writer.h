#pragma once
// Writes rNNNN.vam (R-61): entries are streamed to a temporary body file while
// its BLAKE3 and length are computed; finish() then writes header + body to the
// destination atomically and durably. Nothing appears at the destination until
// finish() succeeds.
#include "core/hash/hashers.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"

#include <memory>

namespace va {

struct WrittenManifest {
    ManifestHeader header;      // with counts and body fields filled in
    std::string header_line;    // line 1 without '\n' (what V1.1 signs, R-60)
    std::string header_blake3;  // BLAKE3 of header_line
};

class ManifestWriter {
public:
    ManifestWriter(IFileWriter& writer, IFileSystemAccess& reader, std::filesystem::path destination);

    // Entries must arrive in canonical order (R-43).
    std::expected<void, IoError> add(const Entry& entry);
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
