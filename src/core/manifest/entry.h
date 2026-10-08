#pragma once
// One manifest entry (HANDOFF §B4.4): a file, directory, link or stream as it
// was recorded at registration. Hashes are empty for directories and for
// entries that could not be read (decision 0010).
#include "common/archive_path.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace va {

struct Entry {
    char type = 'f';                        // 'f' file, 'd' directory, 'l' link, 's' stream
    ArchivePath path;                       // p (+ enc/raw for non-Unicode names)
    std::optional<std::uint64_t> size;      // bytes; for links the target's byte length
    std::optional<std::int64_t> mtime_ns;   // nanoseconds since 1970-01-01 UTC
    std::string b3;                         // BLAKE3 hex of content (or link target)
    std::string sha256;                     // SHA-256 hex of the same bytes
    std::optional<std::uint32_t> mode;      // POSIX permission bits (Linux)
    std::optional<std::uint32_t> attrs;     // Windows attribute bitmask
    std::vector<std::string> warn;          // portability warnings, e.g. "case-collision"
    std::string stream;                     // ADS name, 's' only (V1.1)
    std::string unreadable;                 // "", "permission", "media" or "other"
};

// Canonical order (R-43): bytewise on p, then raw, then type (f, d, l before s),
// then stream. Never directory_iterator order or locale collation.
bool canonical_less(const Entry& left, const Entry& right);

// The identity of an entry within one revision: the same key the database uses
// (path, raw_hex, stream).
std::string entry_key(const Entry& entry);

} // namespace va
