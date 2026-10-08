#pragma once
// Compares a validated manifest with what is on the media now: scan, merge the
// two sorted entry lists (R-43), re-hash every recorded file, and classify each
// entry with an item status (§A6). Used by verification and by relocation.
#include "core/interfaces.h"
#include "core/manifest/manifest.h"
#include "core/repository/archive_store.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace va {

struct ComparisonInput {
    std::filesystem::path root;
    std::string filesystem_type;            // of the verifying mount (R-52 tolerances)
    std::optional<bool> rotational;         // reader count (R-110)
    std::string host_os;                    // "windows" / "linux" (R-44, decision 0008)
    const ManifestHeader* header = nullptr;
    const std::vector<Entry>* recorded = nullptr;
    bool strict_metadata = false;
    // R-104 resume: entry keys whose content already matched in the run being resumed.
    std::set<std::string> content_verified;
    // Called with keys of entries whose content matched, in batches (checkpoints).
    std::function<void(const std::vector<std::string>&)> checkpoint;
};

struct ComparisonResult {
    bool incomplete = false;                // cancelled or the device disappeared
    bool content_failure = false;           // an item status that fails the run
    bool warning = false;                   // an item status that caps the run at warnings
    std::string scan_error;                 // root could not be scanned (FilesystemAccessError)
    std::map<std::string, std::uint64_t> counts;
    std::vector<VerifyItemRecord> items;    // non-Verified items (run_id left empty)
    std::uint64_t coverage_bytes = 0;       // bytes whose content matched
    // The media as it is now, with recorded hashes for matched content and
    // known-unreadable entries carried over unchanged (decision 0010): the
    // entries of a relocated revision (decision 0004). Canonical order.
    std::vector<Entry> current_entries;
};

ComparisonResult compare_with_manifest(
    IFileSystemAccess& files,
    const ComparisonInput& input,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
