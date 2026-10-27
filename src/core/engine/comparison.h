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
    // Called with keys of entries whose content matched, in batches
    // (checkpoints). Returning false stops the comparison, as a cancel does
    // (the result is incomplete): the operation found that it can no longer
    // finish, such as a relocation whose revision another operation recorded
    // (decision 0026).
    std::function<bool(const std::vector<std::string>&)> checkpoint;
    // Called on the comparison's thread for each entry as it is classified
    // with a status other than Verified, with whether that status fails the
    // run (content_failure): a relocation marks its record as soon as the new
    // carrier is known not to hold the recorded content (decision 0026).
    std::function<void(const VerifyItemRecord& item, bool fails)> on_item;
    // Asked when an entry cannot be read, before it is classified (decision
    // 0034): false once the volume of the root is gone, which makes the
    // comparison incomplete (volume_gone) instead of the entry Unreadable.
    // Unset, the volume counts as attached.
    std::function<bool()> volume_attached;
};

struct ComparisonResult {
    bool incomplete = false;                // cancelled or the device disappeared
    bool volume_gone = false;               // incomplete because the device disappeared (decision 0034)
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
