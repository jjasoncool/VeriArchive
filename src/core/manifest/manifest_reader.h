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
// what it read differs from what open() validated (its BLAKE3, length and
// lines). So a caller uses no entry before next() has returned the end of the
// body. A line that does not parse, comes out of canonical order or ends the
// file without its newline is a malformed manifest only when what was read,
// read on to the end of the body, is the body validated; otherwise the second
// read gave other bytes, and next() fails with BodyMismatch (decision 0028).
// Once next() has failed, it fails the same way again.
//
// A manifest with a chunk size (decision 0031) is read strictly: its chunk
// lines are the last counts.chunks lines of the body, and each file that has
// chunk lines owns the next ones, in order. A second position in the file
// reads them alongside the entries, and each such file is returned with its
// chunk hashes (Entry::chunks). Each position's read is held to the stretch
// of the body that open() validated for it: the first to the entries, the
// second to the chunk lines. The end of the entries is reported only once
// every chunk line has found its file and the content hashes give the
// header's merkle_root. A manifest without a chunk size (every V1 manifest)
// is read as before: chunk lines are skipped and merkle_root is ignored.
#include "core/hash/hashers.h"
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/merkle/merkle.h"

#include <limits>
#include <memory>
#include <optional>
#include <string_view>

namespace va {

class ManifestReader {
public:
    // Fails when the manifest cannot be read, its header is not one, or its
    // body does not match the header. Everything else, a header that breaks
    // the rules of chunk lines included, fails next().
    //
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
    // BodyMismatch (see above). After a failure every further call fails the
    // same way.
    std::expected<std::optional<Entry>, ManifestFailure> next();

private:
    // What a position reads of the body, hashed as it is read.
    struct BodyRead {
        Blake3Hasher hash;
        std::uint64_t length = 0;
        std::uint64_t lines = 0;
    };

    // A stretch of the body as open() validated it.
    struct Stretch {
        std::uint64_t length = 0;
        std::uint64_t lines = 0;
        std::string blake3;
    };

    // Reads the lines of one stretch of the body.
    struct LineCursor {
        std::vector<std::byte> buffer;
        std::size_t start = 0;
        std::size_t end = 0;
        std::uint64_t offset = 0;                     // of the next byte to read
        std::uint64_t limit = std::numeric_limits<std::uint64_t>::max(); // where the stretch ends
        std::unique_ptr<BodyRead> read;               // what it read, until it is held to its stretch
        bool differs = false;                         // what it read is not the stretch validated
    };

    ManifestReader() = default;
    // Reads the next piece of the cursor's stretch into its buffer, hashed
    // while the cursor's read is to be checked; 0 at the end of the stretch.
    std::expected<std::size_t, ManifestFailure> fill(LineCursor& cursor);
    std::expected<bool, ManifestFailure> read_line(LineCursor& cursor, std::string& line);
    // Whether what `cursor` read of its stretch, read on to its end, is
    // `validated` (R-62).
    std::expected<bool, ManifestFailure> read_as_validated(LineCursor& cursor, const Stretch& validated);
    // The same for both positions.
    std::expected<bool, ManifestFailure> read_as_validated();
    // next() fails with `failure`, and so again whenever it is asked. A
    // malformed line is told from one that a read gave otherwise.
    std::unexpected<ManifestFailure> failed(ManifestFailure failure);
    std::expected<std::optional<Entry>, ManifestFailure> next_entry();
    std::expected<void, ManifestFailure> attach_chunks(Entry& file);
    std::expected<void, ManifestFailure> check_end();

    std::unique_ptr<IByteSource> source_;
    LineCursor entries_;
    LineCursor chunks_;                               // with a chunk size only
    Stretch entry_lines_;                             // what entries_ reads: the body, or its entries
    Stretch chunk_lines_;                             // what chunks_ reads: the chunk lines
    ManifestHeader header_;
    std::string header_line_;
    std::string header_blake3_;
    std::optional<Entry> previous_;
    MerkleTreeBuilder merkle_;
    bool finished_ = false;
    std::optional<ManifestFailure> failed_;
};

} // namespace va
