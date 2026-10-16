#pragma once
// Reads a .vam manifest (R-62): line 1 is parsed, then the rest of the file is
// hashed and compared with the header's body.blake3 / body.length *before* any
// entry is returned. Entries are then streamed one by one, so multi-gigabyte
// manifests never have to fit in memory.
//
// The entries are read from the file a second time, and that read is hashed
// as well: a file that changed since it was validated, or removable media
// that gives other bytes the second time, would give entries that are not the
// body validated. At the end of the body, next() fails with BodyMismatch when
// what it read differs from the header's body (its BLAKE3, length and lines).
// So a caller uses no entry before next() has returned the end of the body.
#include "core/hash/hashers.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"

#include <memory>
#include <optional>
#include <string_view>

namespace va {

class ManifestReader {
public:
    // With `recorded_header_blake3`, the BLAKE3 that a repository recorded
    // for the manifest's line 1, line 1 is compared with it before it is
    // parsed (decision 0028): a line 1 that is not the one recorded, also one
    // that no longer parses, ends without a newline or is empty, fails with
    // HeaderMismatch, never as a malformed manifest.
    static std::expected<ManifestReader, ManifestFailure> open(IFileSystemAccess& files,
        const std::filesystem::path& path, std::optional<std::string_view> recorded_header_blake3 = std::nullopt);
    // Line 1 of the manifest at `path` without its '\n', reading no further:
    // its header as the manifest holds it (what V1.1 signs, R-60). Nothing of
    // it is checked here; for a check of a recorded revision that needs no
    // entries (load_recorded_header).
    static std::expected<std::string, ManifestFailure> read_header_line(
        IFileSystemAccess& files, const std::filesystem::path& path);

    const ManifestHeader& header() const { return header_; }
    const std::string& header_line() const { return header_line_; }
    const std::string& header_blake3() const { return header_blake3_; }

    // The next entry in canonical order, or nullopt after the last one, once
    // what the entries were read from is the body validated; otherwise
    // BodyMismatch (see above).
    std::expected<std::optional<Entry>, ManifestFailure> next();

private:
    ManifestReader() = default;
    std::expected<bool, ManifestFailure> read_line(std::string& line);

    // What next() reads, hashed as it is read: from the end of open(), which
    // validated the body, to the end of the body.
    struct BodyRead {
        Blake3Hasher hash;
        std::uint64_t length = 0;
        std::uint64_t lines = 0;
    };

    std::unique_ptr<IByteSource> source_;
    std::vector<std::byte> buffer_;
    std::size_t buffer_start_ = 0;
    std::size_t buffer_end_ = 0;
    std::uint64_t offset_ = 0;
    ManifestHeader header_;
    std::string header_line_;
    std::string header_blake3_;
    std::optional<Entry> previous_;
    std::unique_ptr<BodyRead> body_read_;
    bool body_read_differs_ = false;
};

} // namespace va
