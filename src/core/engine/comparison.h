#pragma once
// Compares a validated manifest with what is on the media now: scan, merge the
// two sorted entry lists (R-43), re-hash every recorded file, and classify each
// entry with an item status (§A6). Used by verification and by relocation.
#include "core/engine/read_speed.h"
#include "core/engine/volume_watch.h"
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
    // Decision 0032: whether a file with chunk lines is compared by chunk as
    // well. Its chunk hashes come from the read that hashes the whole file
    // (R-101), at the revision's chunk size, so a revision without one (0, as
    // V1 registered) does no chunk work. A file whose content differs names
    // the chunks that differ; one whose content matches its file hashes
    // while a chunk hash does not, or every chunk hash while a file hash
    // does not, gives a warning (ComparisonResult::warnings). A FULL verify
    // and a relocation compare by chunk (decision 0032, points 10 and 11); a
    // relocation carries the hashes of the revision it verifies, a hash that
    // cannot be relied on included (point 5).
    bool compare_chunks = false;
    // Adding chunk hashes (decision 0033): the chunk size at which a FULL
    // verify hashes the chunks of a revision that has none (chunk size 0, as
    // V1 registered it), in the read that hashes each file whole (R-101); 0
    // adds none. Nothing is compared by them, as none are recorded:
    // `chunks_read` is given the chunk hashes of each file larger than it
    // whose content matched, before its key joins a checkpoint.
    std::uint64_t adding_chunk_size = 0;
    std::function<void(const Entry& recorded, std::vector<std::string> chunks)> chunks_read;
    // R-104 resume: entry keys whose content already matched in the run being resumed.
    std::set<std::string> content_verified;
    // Called with keys of entries whose content matched, in batches
    // (checkpoints): compared by chunk, every chunk hash and both file hashes
    // of it, so that a resume reads again a file whose chunk hashes or file
    // hash cannot be relied on, and warns again (decision 0032). Returning
    // false stops the comparison, as a cancel does (the result is
    // incomplete): the operation found that it can no longer finish, such as
    // a relocation whose revision another operation recorded (decision 0026).
    std::function<bool(const std::vector<std::string>&)> checkpoint;
    // Called on the comparison's thread for each entry as it is classified
    // with a status other than Verified, with whether that status fails the
    // run (content_failure): a relocation marks its record as soon as the new
    // carrier is known not to hold the recorded content (decision 0026).
    std::function<void(const VerifyItemRecord& item, bool fails)> on_item;
    // Told of every read of the scan and of the hashing (decision 0034),
    // which asks it before an entry that cannot be read is classified: when
    // the device went away or stopped answering, the comparison is
    // incomplete (device_stop) instead of the entry Unreadable. An I/O error
    // it holds is classified only once a later read succeeds, or once every
    // file was read. Unset, only an error that names a removed device stops
    // the comparison.
    VolumeWatch* watch = nullptr;
    // The clock that times the read of each hundredth of the content
    // (decision 0044); unset, the steady clock. Tests give their own.
    ReadSpeedMeter::Clock clock = {};
};

struct ComparisonResult {
    bool incomplete = false;                // cancelled, or the device went away or stopped answering
    DeviceStop device_stop = DeviceStop::None; // why the device made it incomplete (decision 0034)
    bool content_failure = false;           // an item status that fails the run
    bool warning = false;                   // an item status that caps the run at warnings
    std::string scan_error;                 // root could not be scanned (FilesystemAccessError)
    std::map<std::string, std::uint64_t> counts;
    std::vector<VerifyItemRecord> items;    // non-Verified items (run_id left empty)
    // Warnings of the run, in the canonical order of the files they name:
    // compared by chunk, each file whose content matches both its file hashes
    // while a chunk hash of it does not, or matches every chunk hash while a
    // file hash does not (decision 0032). The file is fine and counts as
    // Verified; the hash recorded that does not match cannot be relied on.
    std::vector<std::string> warnings;
    std::uint64_t coverage_bytes = 0;       // bytes whose content matched
    // The media as it is now, with recorded hashes (chunk hashes included)
    // for matched content and known-unreadable entries carried over unchanged
    // (decision 0010): the entries of a relocated revision (decisions 0004,
    // 0032). Canonical order.
    std::vector<Entry> current_entries;
    // Decision 0044: the revision's content in bytes (every file and stream
    // it records with hashes), and the time spent reading each hundredth of
    // it that this comparison read, resumed files being skipped.
    std::uint64_t content_bytes = 0;
    std::vector<ReadPart> read_parts;
};

ComparisonResult compare_with_manifest(
    IFileSystemAccess& files,
    const ComparisonInput& input,
    CancelToken& cancel,
    IProgressSink& progress);

// §A6, R-42 (decision 0013): whether the entries of a revision record names
// that differ from another name in the same folder only by case or by
// Unicode normalisation (`warn`: case-collision, nfc-collision), portability
// warnings that make every FULL verification of that revision, a
// relocation's included, PASSED_WITH_WARNINGS at most, with the warning
// name_collision_warning.
bool records_name_collisions(const std::vector<Entry>& entries);
inline constexpr const char* name_collision_warning =
    "some names differ from another name in the same folder only in upper and lower case, or in how a letter with "
    "an accent or a mark is encoded (Unicode normalisation); a volume or system that does not tell such names apart "
    "cannot keep them all";

// R-30, §A6: the detail of the SkippedMountPoint item of a mount point where
// the revision records nothing at or below it. It holds no part of the
// archive, so its item changes no result; a mount point that covers recorded
// entries has another detail, and fails the run.
inline constexpr const char* mount_point_outside_archive =
    "another volume is mounted here; it is not part of the archive";

} // namespace va
