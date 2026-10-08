#pragma once
// The .vam manifest format, version 1 (HANDOFF §B4). Line 1 is the header,
// every following line is one entry, all canonical JSON. The validated bytes of
// this file, not the database, are the integrity truth (R-14).
#include "common/types.h"
#include "core/manifest/entry.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace va {

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
    std::uint64_t chunk_size = 0;             // 0 in V1: no chunk lines
    int exclusions_version = 1;
    std::vector<std::string> exclusions;
    ManifestCounts counts;                    // filled in by the writer
    std::string body_blake3;                  // filled in by the writer
    std::uint64_t body_length = 0;
    std::uint64_t body_lines = 0;
};

// Canonical JSON for one header line / entry line, without the trailing '\n'.
std::string header_to_json(const ManifestHeader& header);
std::string entry_to_json(const Entry& entry);

// Parsing; throws nlohmann::json::exception on malformed input (callers catch).
ManifestHeader header_from_json(const nlohmann::json& json);
Entry entry_from_json(const nlohmann::json& json);

// What went wrong while reading a manifest. BodyMismatch means the bytes do not
// match the header's body hash: the run fails with ManifestHashMismatch (§A6).
struct ManifestFailure {
    enum class Kind { Io, Malformed, BodyMismatch };
    Kind kind;
    std::string message;
};

} // namespace va
