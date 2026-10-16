#pragma once
// Adding chunk hashes (decision 0033): a FULL verify of an archive whose
// current revision rN has no chunk size, as V1 registered it, that also hashes
// the chunks of its files in the read that hashes each file whole (R-101), and
// that records the next revision r(N+1) when every entry matched: rN's entry
// lines as they are, followed by the chunk lines of every file larger than the
// chunk size, with the Merkle root of §B6. Otherwise it records the
// verification alone, as any FULL verify does.
//
// While it has not finished, the run's record ("run:<run_id>", decision 0022)
// says that it adds chunk hashes and at which chunk size, so that `verify
// --resume` goes on adding them.
#include "core/engine/candidates.h"
#include "core/manifest/entry.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va {

// The member of a run's record that says that the run adds chunk hashes: the
// chunk size at which it hashes them.
inline constexpr const char* adding_member = "add_chunk_hashes";

// Why chunk hashes cannot be added to `archive` now, or nothing when they can:
// a retired archive takes no new revisions (decision 0003), and an archive
// whose current revision has a chunk size, as its manifest says (R-14), has
// them. Reads the first line of that manifest alone (load_recorded_header);
// fails when it cannot. The rule by which a verification refuses to add them
// and the GUI offers to.
std::expected<std::optional<std::string>, IoError> why_chunk_hashes_cannot_be_added(ArchiveStore& store,
    IFileSystemAccess& repository_files, const RepositoryLayout& layout, const ArchiveRecord& archive);

// The chunk size at which the run whose record is `record` adds chunk hashes;
// 0 when it adds none (a FULL verify that only verifies, or a record of a
// version before decision 0033), or when the value is no chunk size.
std::uint64_t adding_chunk_size(const nlohmann::json& record);

// The entries of r(N+1): rN's entries `recorded` as they are, in their order,
// each file that has chunk lines at `chunk_size` (has_chunk_lines) with the
// chunk hashes that `chunks` holds under its entry_key(). Fails, naming the
// file, when such a file has none there, or not as many as its size gives.
std::expected<std::vector<Entry>, IoError> entries_with_chunk_hashes(const std::vector<Entry>& recorded,
    const std::map<std::string, std::vector<std::string>>& chunks, std::uint64_t chunk_size);

// The header of r(N+1), before the writer fills in its chunk size, counts,
// Merkle root and body: rN's (`previous`: its label, copy_of, exclusions),
// with revision N+1 superseding N, made at `created` on `host`, from the
// carrier `identity` at the archive root `root_subpath` on it, found at
// `root_hint`: the carrier this revision was read from (decision 0004).
ManifestHeader header_with_chunk_hashes(const ManifestHeader& previous, const Timestamp& created,
    const HostInfo& host, const VolumeInfo& identity, const std::string& root_subpath, const std::string& root_hint);

} // namespace va
