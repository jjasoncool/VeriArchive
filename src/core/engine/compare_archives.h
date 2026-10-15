#pragma once
// Compares two registered archives entry by entry, from the manifests of their
// current revisions in the repository; no media is read (decision 0020). The
// typical use is a backup: archive B registered from a copy of archive A's
// disk. Archive A is the reference, so its words are verification's item
// statuses: Missing (in A, not in B), New (in B, not in A), Modified (content
// differs), TypeChanged (file in one, directory or link in the other).
// Unbaselined: an entry without content hash in A or B (unreadable at
// registration, decision 0010), or found inside a directory one side could not
// list. MetadataMismatch: a link whose absolute target differs between archives
// registered on different OSes, which a driver may have translated (decision
// 0008).
//
// Entries are matched by path and stream, not by type. A manifest records a
// directory only when it is empty (R-49), so a directory with contents is
// recognised from its descendants: "x" as a file in A and "x/a.txt" in B is
// "x" TypeChanged (f -> d) plus "x/a.txt" New; "x" as an empty directory in A
// and "x/a.txt" in B is only "x/a.txt" New.
//
// What is compared is what each registration read. Whether the disks still
// hold it is what FULL verification of each archive proves.
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace va {

// One entry that is not the same in both archives.
struct ArchiveDifference {
    std::string path;    // archive-relative path ("p"), the same in both archives
    std::string stream;  // "" for the file itself (alternate data streams arrive in V1.1)
    std::string status;  // Missing | New | Modified | TypeChanged | Unbaselined | MetadataMismatch
    // Missing, New: "". Modified: "size 10 -> 12", "content differs", "link
    // target differs". TypeChanged: "f -> d" (f file, d directory, l link).
    // Unbaselined: "unreadable in A (media)" (reason permission | media |
    // other), "unreadable in both archives", "inside a directory unreadable in
    // B". MetadataMismatch: "link-target-translated".
    std::string detail;
};

struct ArchiveComparison {
    Uuid archive_a;
    Uuid archive_b;
    std::string label_a;
    std::string label_b;
    std::uint32_t revision_a = 0;
    std::uint32_t revision_b = 0;
    std::string registered_a;  // when each compared revision was created (UTC timestamp)
    std::string registered_b;
    std::uint64_t entries_a = 0;
    std::uint64_t entries_b = 0;
    std::uint64_t same = 0;                        // in both, same type and content
    std::map<std::string, std::uint64_t> counts;   // per status of `differences`
    std::vector<ArchiveDifference> differences;    // in canonical order of path
    // IDENTICAL: nothing differs. IDENTICAL_WITH_WARNINGS: only Unbaselined or
    // MetadataMismatch entries (their content cannot be compared, so it is not
    // proven the same). DIFFERENT: anything Missing, New, Modified or TypeChanged.
    std::string result;
};

struct CompareFailure {
    // ManifestHashMismatch | ManifestRevisionRollback | RepositoryError | Cancelled
    std::string failure_class;
    std::string message;
};

// Both manifests are loaded through load_recorded_manifest, so a manifest whose
// bytes are not the recorded ones is refused (R-14). Comparing an archive with
// itself is allowed: IDENTICAL, or IDENTICAL_WITH_WARNINGS when it has
// known-unreadable entries. Metadata (mtime, mode, attributes) is not compared:
// copies made by ordinary tools need not keep it.
//
// Progress counts entries: items_done of items_total = entries_a + entries_b,
// no bytes. Loading the manifests comes first and reports no progress; the
// token is checked before each load and during the merge.
std::expected<ArchiveComparison, CompareFailure> compare_archives(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_a,
    const Uuid& archive_b,
    CancelToken& cancel,
    IProgressSink& progress);

} // namespace va
