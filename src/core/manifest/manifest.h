#pragma once
// The .vam manifest format, version 1 (HANDOFF §B4). Line 1 is the header;
// every following line is one entry and then, in a manifest with a chunk
// size, one chunk line per chunk of each file larger than it (decision 0031),
// all canonical JSON. The validated bytes of this file, not the database, are
// the integrity truth (R-14).
#include "common/error.h"
#include "common/types.h"
#include "core/manifest/entry.h"
#include "core/merkle/merkle.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace va {

// hash.chunk_size (§B4.3, decision 0031): 0 means no chunk lines, as in every
// V1 manifest; otherwise a positive multiple of chunk_size_unit no larger than
// max_chunk_size. Real archives use default_chunk_size (64 MiB, D2); tests
// use smaller ones.
inline constexpr std::uint64_t chunk_size_unit = 64 * 1024;
inline constexpr std::uint64_t max_chunk_size = std::uint64_t{1} << 30;
inline constexpr std::uint64_t default_chunk_size = 64 * 1024 * 1024;
bool valid_chunk_size(std::uint64_t chunk_size);
// Why `chunk_size` is not valid, for an error message.
std::string invalid_chunk_size(std::uint64_t chunk_size);

// Whether `entry` has chunk lines in a manifest with `chunk_size`: a file with
// a content hash that is larger than the chunk size. A smaller file is one
// chunk, the whole file, whose hash is its b3 (R-31); unreadable files,
// directories, links and streams have no chunks.
bool has_chunk_lines(const Entry& entry, std::uint64_t chunk_size);
// The number of chunk lines of a file of `size` bytes that has them.
std::uint64_t chunk_line_count(std::uint64_t size, std::uint64_t chunk_size);

// One chunk line (§B4.4): chunk `idx` of the file `p`, the `length` bytes of
// the file from `offset`.
struct ChunkLine {
    std::string p;
    std::uint64_t idx = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string b3;
};

// Chunk `idx` of the file `p` of `size` bytes: offset idx * chunk_size, as
// long as the chunk size except for a shorter last chunk; b3 left empty.
ChunkLine chunk_of(const std::string& p, std::uint64_t size, std::uint64_t idx, std::uint64_t chunk_size);

// Adds the leaves `entry` gives the Merkle tree of a manifest with chunk lines
// (§B6): the hashes of its chunks if it has chunk lines, else the b3 of a
// file, link or stream; nothing for a directory or an entry without a content
// hash. Adds nothing and returns false when one of them is not 64 lowercase
// hex digits.
bool add_merkle_leaves(MerkleTreeBuilder& tree, const Entry& entry, std::uint64_t chunk_size);

struct ManifestCounts {
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t links = 0;
    std::uint64_t streams = 0;
    std::uint64_t chunks = 0;
    std::uint64_t bytes = 0;   // content bytes covered by hashes
};

struct ManifestHeader {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 1;
    std::optional<std::uint32_t> supersedes;  // omitted for revision 1
    std::optional<Uuid> copy_of;              // decision 0005, display only
    Timestamp created;
    std::string app_version;
    std::string os;                           // "windows" or "linux"
    std::string host;
    VolumeInfo identity;                      // carrier this revision was read from (decision 0004)
    std::string root_subpath;                 // stored inside "identity"
    std::string root_hint;                    // display only (R-45)
    std::uint64_t chunk_size = 0;             // filled in by the writer; 0: no chunk lines (V1)
    int exclusions_version = 1;
    std::vector<std::string> exclusions;
    ManifestCounts counts;                    // filled in by the writer
    std::string merkle_root;                  // filled in by the writer; empty (omitted) for chunk size 0
    std::string body_blake3;                  // filled in by the writer
    std::uint64_t body_length = 0;
    std::uint64_t body_lines = 0;
};

// Canonical JSON for one header line / entry line / chunk line, without the
// trailing '\n'.
std::string header_to_json(const ManifestHeader& header);
std::string entry_to_json(const Entry& entry);
std::string chunk_line_to_json(const ChunkLine& chunk);

// Parsing; throws nlohmann::json::exception on malformed input (callers catch).
ManifestHeader header_from_json(const nlohmann::json& json);
Entry entry_from_json(const nlohmann::json& json);
// Also throws when b3 is not 64 lowercase hex digits.
ChunkLine chunk_line_from_json(const nlohmann::json& json);

// What went wrong while reading a manifest. BodyMismatch means the bytes do not
// match the header's body hash, HeaderMismatch that line 1 is not the one the
// repository recorded (decision 0028), MerkleMismatch that the content hashes
// do not match its merkle_root (decision 0031): each fails the run with
// ManifestHashMismatch (§A6).
struct ManifestFailure {
    enum class Kind { Io, Malformed, BodyMismatch, HeaderMismatch, MerkleMismatch };
    Kind kind;
    std::string message;
    IoErrorKind io_kind = IoErrorKind::Other;  // Io: what the file system said, such as NotFound

    bool hash_mismatch() const
    {
        return kind == Kind::BodyMismatch || kind == Kind::HeaderMismatch || kind == Kind::MerkleMismatch;
    }
};

} // namespace va
