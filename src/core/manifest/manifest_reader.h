#pragma once
// Reads a .vam manifest (R-62): line 1 is parsed, then the rest of the file is
// hashed and compared with the header's body.blake3 / body.length *before* any
// entry is returned. Entries are then streamed one by one, so multi-gigabyte
// manifests never have to fit in memory.
#include "core/interfaces.h"
#include "core/manifest/manifest.h"

#include <memory>

namespace va {

class ManifestReader {
public:
    static std::expected<ManifestReader, ManifestFailure> open(
        IFileSystemAccess& files, const std::filesystem::path& path);
    // Line 1 of the manifest at `path` without its '\n', reading no further:
    // its header as the manifest holds it (what V1.1 signs, R-60). Nothing of
    // it is checked here; for a check of a recorded revision that needs no
    // entries (load_recorded_header).
    static std::expected<std::string, ManifestFailure> read_header_line(
        IFileSystemAccess& files, const std::filesystem::path& path);

    const ManifestHeader& header() const { return header_; }
    const std::string& header_line() const { return header_line_; }
    const std::string& header_blake3() const { return header_blake3_; }

    // The next entry in canonical order, or nullopt after the last one.
    std::expected<std::optional<Entry>, ManifestFailure> next();

private:
    ManifestReader() = default;
    std::expected<bool, ManifestFailure> read_line(std::string& line);

    std::unique_ptr<IByteSource> source_;
    std::vector<std::byte> buffer_;
    std::size_t buffer_start_ = 0;
    std::size_t buffer_end_ = 0;
    std::uint64_t offset_ = 0;
    ManifestHeader header_;
    std::string header_line_;
    std::string header_blake3_;
    std::optional<Entry> previous_;
};

} // namespace va
