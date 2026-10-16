#include "core/engine/comparison.h"

#include "core/engine/item_rules.h"
#include "core/hash/hash_pipeline.h"
#include "core/scan/scanner.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace va {
namespace {

// How an item status affects the run result (§A6).
enum class Severity { None, Warning, Failure };

// Ancestor directories of an ArchivePath string: "a/b/c" -> "a", "a/b".
template <typename Visitor>
void for_each_ancestor(const std::string& path, Visitor visit)
{
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        visit(path.substr(0, slash));
    }
}

template <typename Container>
std::optional<std::string> ancestor_in(const std::string& path, const Container& paths)
{
    std::optional<std::string> found;
    for_each_ancestor(path, [&](const std::string& ancestor) {
        if (!found && paths.contains(ancestor)) {
            found = ancestor;
        }
    });
    return found;
}

std::string unreadable_status(const std::string& reason)
{
    return "Unreadable(" + reason + ")";
}

VerifyItemRecord item_of(const Entry& entry, const std::string& status, const std::string& detail)
{
    VerifyItemRecord item;
    item.path = entry.path;
    item.stream = entry.stream;
    item.status = status;
    item.detail = detail;
    return item;
}

// The idx of each chunk of a file with chunk lines whose recorded hash
// differs from the one just read, ascending (decision 0032). Empty for a file
// without chunk lines, and for one not read by chunk: its read gave no chunk
// hashes.
std::vector<std::uint64_t> differing_chunks(const Entry& recorded, const HashResult& read)
{
    std::vector<std::uint64_t> differing;
    if (recorded.chunks.empty() || read.chunks.size() != recorded.chunks.size()) {
        return differing;
    }
    for (std::size_t idx = 0; idx < recorded.chunks.size(); ++idx) {
        if (read.chunks[idx] != recorded.chunks[idx]) {
            differing.push_back(idx);
        }
    }
    return differing;
}

// The chunks `idx` (ascending) as an item's detail names them: "3", "3 and
// 17", "3, 5 and 17", a run of consecutive chunks as "3-5"; after the tenth
// run, "and N more" counts the chunks left.
std::string chunk_list(const std::vector<std::uint64_t>& idx)
{
    constexpr std::size_t named_runs = 10;
    std::vector<std::string> runs;
    std::uint64_t unnamed = 0;
    for (std::size_t start = 0; start < idx.size();) {
        auto end = start;
        while (end + 1 < idx.size() && idx[end + 1] == idx[end] + 1) {
            ++end;
        }
        if (runs.size() < named_runs) {
            runs.push_back(std::to_string(idx[start]) + (end > start ? "-" + std::to_string(idx[end]) : ""));
        } else {
            unnamed += end - start + 1;
        }
        start = end + 1;
    }
    std::string text;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index > 0) {
            text += index + 1 == runs.size() && unnamed == 0 ? " and " : ", ";
        }
        text += runs[index];
    }
    return unnamed > 0 ? text + " and " + std::to_string(unnamed) + " more" : text;
}

// A byte offset as people read it: "201,326,592".
std::string grouped(std::uint64_t value)
{
    auto digits = std::to_string(value);
    for (auto at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

// A file whose content differs, at its recorded size. A file with chunk lines
// read by chunk names the chunks that differ and where the first begins
// (decision 0032): "content differs: chunks 3 and 17 of 160 differ, the first
// from byte 201,326,592". Any other file, or one whose chunks all match
// though a file hash does not, says only that its content differs.
VerifyItemRecord content_differs(const Entry& recorded, std::vector<std::uint64_t> differing, std::uint64_t chunk_size)
{
    auto item = item_of(recorded, "Modified", "content differs");
    if (differing.empty()) {
        return item;
    }
    item.chunks = recorded.chunks.size();
    item.first_differing_offset = differing.front() * chunk_size;
    const auto of = " of " + std::to_string(item.chunks);
    item.detail = differing.size() == 1
                      ? "content differs: chunk " + std::to_string(differing.front()) + of + " differs, from byte " +
                            grouped(item.first_differing_offset)
                      : "content differs: chunks " + chunk_list(differing) + of + " differ, the first from byte " +
                            grouped(item.first_differing_offset);
    item.differing_chunks = std::move(differing);
    return item;
}

// A file whose content matches both its file hashes while chunks `differing`
// do not match their recorded hashes (decision 0032): the file is fine, and
// its manifest was wrong when it was written. Its chunk hashes cannot be
// relied on to say where it differs, or to sample it; a new revision, which
// reads it again, records them right.
std::string unreliable_chunk_hashes(const Entry& recorded, const std::vector<std::uint64_t>& differing)
{
    const auto of = " of " + std::to_string(recorded.chunks.size());
    return "the chunk hashes of " + recorded.path.p + " cannot be relied on: " +
           (differing.size() == 1 ? "chunk " + std::to_string(differing.front()) + of + " does not match"
                                  : "chunks " + chunk_list(differing) + of + " do not match") +
           ", while the whole file matches its hashes; the file is fine, and a new revision fixes them";
}

// The other half (decision 0032): a file with chunk lines whose every chunk,
// read by chunk, matches its recorded hash while its file hash, BLAKE3 or
// SHA-256, or both, does not. The file is fine, as its chunk hashes, BLAKE3
// hashes of all of its bytes, show: its manifest was wrong when it was
// written. The file hash recorded cannot be relied on; a new revision, which
// reads the file again, records it right.
std::string unreliable_file_hashes(const Entry& recorded, const HashResult& read)
{
    const auto chunks = ", while each of the file's " + std::to_string(recorded.chunks.size()) +
                        " chunks matches its chunk hash; the file is fine, and a new revision fixes ";
    if (read.b3 != recorded.b3 && read.sha256 != recorded.sha256) {
        return "the whole-file BLAKE3 and SHA-256 hashes of " + recorded.path.p +
               " cannot be relied on: they do not match" + chunks + "them";
    }
    return "the whole-file " + std::string(read.b3 != recorded.b3 ? "BLAKE3" : "SHA-256") + " hash of " +
           recorded.path.p + " cannot be relied on: it does not match" + chunks + "it";
}

class Classifier {
public:
    Classifier(ComparisonResult& result, const ComparisonInput& input)
        : result_(result), input_(input), same_os_(input.header->os == input.host_os)
    {
    }

    void add(const Entry& entry, const std::string& status, const std::string& detail, Severity severity)
    {
        add(item_of(entry, status, detail), severity);
    }

    void add(VerifyItemRecord item, Severity severity)
    {
        ++result_.counts[item.status];
        if (severity == Severity::Failure) {
            result_.content_failure = true;
        } else if (severity == Severity::Warning) {
            result_.warning = true;
        }
        if (item.status != "Verified") {
            result_.items.push_back(std::move(item));
            if (input_.on_item) {
                input_.on_item(result_.items.back(), severity == Severity::Failure);
            }
        }
    }

    // Metadata differences are warnings unless --strict-metadata (R-51).
    void verified_or_metadata(const Entry& recorded, const Entry& current)
    {
        std::string detail;
        if (recorded.mtime_ns && current.mtime_ns &&
            !mtime_matches(input_.filesystem_type, *recorded.mtime_ns, *current.mtime_ns)) {
            detail = "mtime";
        }
        if (input_.strict_metadata) {
            if (recorded.mode && current.mode && *recorded.mode != *current.mode) {
                detail += detail.empty() ? "mode" : ",mode";
            }
            if (recorded.attrs && current.attrs && *recorded.attrs != *current.attrs) {
                detail += detail.empty() ? "attrs" : ",attrs";
            }
        }
        if (detail.empty()) {
            add(recorded, "Verified", "", Severity::None);
        } else {
            add(recorded, "MetadataMismatch", detail,
                input_.strict_metadata ? Severity::Failure : Severity::Warning);
        }
    }

    // Decision 0008: on the registering OS a link target must match exactly;
    // across OSes an absolute target may be translated by the driver.
    bool compare_link(const Entry& recorded, const Entry& current)
    {
        if (recorded.b3 == current.b3 && recorded.sha256 == current.sha256) {
            verified_or_metadata(recorded, current);
            return true;
        }
        const auto absolute = [](const Entry& entry) {
            return std::find(entry.warn.begin(), entry.warn.end(), "link-absolute") != entry.warn.end();
        };
        if (!same_os_ && (absolute(recorded) || absolute(current))) {
            add(recorded, "MetadataMismatch", "link-target-translated",
                input_.strict_metadata ? Severity::Failure : Severity::Warning);
            return true;
        }
        add(recorded, "Modified", "link target differs", Severity::Failure);
        return false;
    }

private:
    ComparisonResult& result_;
    const ComparisonInput& input_;
    bool same_os_;
};

// The new carrier's entry for matched content: its own name and metadata, the
// recorded hashes (decision 0004), its chunk hashes included (decision 0032).
Entry carried_entry(const Entry& recorded, const Entry& current)
{
    Entry entry = current;
    entry.b3 = recorded.b3;
    entry.sha256 = recorded.sha256;
    entry.size = recorded.size;
    entry.chunks = recorded.chunks;
    return entry;
}

} // namespace

ComparisonResult compare_with_manifest(
    IFileSystemAccess& files,
    const ComparisonInput& input,
    CancelToken& cancel,
    IProgressSink& progress)
{
    ComparisonResult result;
    const auto& recorded = *input.recorded;
    ScanOptions scan_options;
    scan_options.exclusions = input.header->exclusions;
    if (input.watch) {
        input.watch->watch_scan(scan_options);
    }
    // Why a read that named a removed device, or that the watch stopped on, stops the comparison.
    const auto device_stop = [&] {
        return input.watch && input.watch->stop() != DeviceStop::None ? input.watch->stop() : DeviceStop::WentAway;
    };
    auto scanned = scan_tree(files, input.root, scan_options, cancel, progress);
    if (!scanned) {
        const auto kind = scanned.error().kind;
        if (kind == IoErrorKind::Cancelled || kind == IoErrorKind::DeviceRemoved) {
            result.incomplete = true;
            if (kind == IoErrorKind::DeviceRemoved) {
                result.device_stop = device_stop();
            }
        } else {
            result.scan_error = scanned.error().message;
        }
        return result;
    }
    Classifier classify(result, input);

    // Directory facts needed to explain missing and new entries.
    std::set<std::string> known_unreadable_dirs; // unreadable at registration
    for (const auto& entry : recorded) {
        if (entry.type == 'd' && !entry.unreadable.empty()) {
            known_unreadable_dirs.insert(entry.path.p);
        }
    }
    std::map<std::string, std::string> unreadable_dirs_now;
    for (const auto& scanned_entry : scanned->entries) {
        const auto& entry = scanned_entry.entry;
        if (entry.type == 'd' && !entry.unreadable.empty()) {
            unreadable_dirs_now[entry.path.p] = entry.unreadable;
        }
    }
    std::set<std::string> mount_points_now;
    for (const auto& mount_point : scanned->skipped_mount_points) {
        mount_points_now.insert(mount_point.p);
    }
    // The revision was registered on the system that verifies it (created_by.os).
    const bool registered_here = input.header->os == input.host_os;
    // A directory with contents is no entry (R-49): it shows only through its
    // descendants, in the manifest and in the scan alike (item_rules.h).
    const auto recorded_folder = [&](const ArchivePath& path) {
        return holds_directory(recorded, path, [](const Entry& entry) -> const ArchivePath& { return entry.path; });
    };
    const auto folder_now = [&](const ArchivePath& path) {
        return holds_directory(scanned->entries, path,
            [](const ScannedEntry& scanned_entry) -> const ArchivePath& { return scanned_entry.entry.path; });
    };

    const auto missing = [&](const Entry& entry) {
        if (entry.type != 's' && folder_now(entry.path)) {
            if (entry.type == 'd') {
                classify.add(entry, "Verified", "", Severity::None); // now holds new entries
            } else {
                // R-50: a file or link that became a folder with contents, as
                // archive comparison tells it (decision 0020); its contents
                // are New.
                classify.add(entry, "TypeChanged", std::string(1, entry.type) + " -> d", Severity::Failure);
            }
        } else if (mount_points_now.contains(entry.path.p) || ancestor_in(entry.path.p, mount_points_now)) {
            classify.add(entry, "SkippedMountPoint", "a mount point now covers this path", Severity::Failure);
        } else if (const auto dir = ancestor_in(entry.path.p, unreadable_dirs_now)) {
            classify.add(entry, unreadable_status(unreadable_dirs_now.at(*dir)),
                "directory " + *dir + " cannot be listed", Severity::Failure);
        } else if (!registered_here &&
                   ((input.host_os == "windows" && !representable_on_windows(entry.path.p)) ||
                       (!entry.path.enc.empty() && !archive_path_to_native(entry.path)))) {
            // R-44, only where another system registered the revision: on the
            // one that did, the name existed there (Windows reads names from
            // NTFS that it lets no program create), so it is Missing.
            classify.add(entry, "Unrepresentable", "name cannot exist on " + input.host_os,
                Severity::Warning);
        } else {
            classify.add(entry, "Missing", "", Severity::Failure);
        }
    };
    const auto added = [&](const Entry& entry) {
        if (entry.type != 's' && recorded_folder(entry.path)) {
            if (entry.type == 'd') {
                return; // existed implicitly; its recorded contents are reported individually
            }
            // R-50: a folder with contents that became a file or a link, as
            // archive comparison tells it (decision 0020); its recorded
            // contents are Missing.
            classify.add(entry, "TypeChanged", std::string("d -> ") + entry.type, Severity::Failure);
            return;
        }
        if (ancestor_in(entry.path.p, known_unreadable_dirs)) {
            classify.add(entry, "Unbaselined", "inside a directory unreadable at registration",
                Severity::Warning);
        } else {
            classify.add(entry, "New", "", Severity::Failure);
        }
    };

    // R-30, §A6: every mount point below the root is reported, never
    // descended. One that covers recorded entries fails the run, through each
    // of them (missing, above). One where the revision records nothing at or
    // below it (it was there at registration, which skipped it, or it was
    // mounted where nothing was recorded) holds no part of the archive: its
    // item changes no result, as every registered byte is still read.
    for (const auto& mount_point : scanned->skipped_mount_points) {
        bool recorded_there = recorded_folder(mount_point);
        for (auto at = std::lower_bound(recorded.begin(), recorded.end(), mount_point.p,
                 [](const Entry& entry, const std::string& value) { return entry.path.p < value; });
             !recorded_there && at != recorded.end() && at->path.p == mount_point.p; ++at) {
            recorded_there = at->path.raw == mount_point.raw;
        }
        if (!recorded_there) {
            Entry skipped;
            skipped.type = 'd';
            skipped.path = mount_point;
            classify.add(skipped, "SkippedMountPoint", "another volume is mounted here; it is not part of the archive",
                Severity::None);
        }
    }

    // Merge the two sorted lists and collect the files that must be re-hashed.
    std::vector<std::pair<const Entry*, const ScannedEntry*>> to_hash;
    std::size_t m = 0;
    std::size_t s = 0;
    const auto& current = scanned->entries;
    while (m < recorded.size() || s < current.size()) {
        if (s == current.size() || (m < recorded.size() && canonical_less(recorded[m], current[s].entry))) {
            missing(recorded[m++]);
            continue;
        }
        if (m == recorded.size() || canonical_less(current[s].entry, recorded[m])) {
            added(current[s++].entry);
            continue;
        }
        const auto& was = recorded[m++];
        const auto& now = current[s++];
        const auto& is = now.entry;
        if (was.type != is.type) {
            classify.add(was, "TypeChanged", std::string(1, was.type) + " -> " + is.type,
                Severity::Failure); // R-50
            continue;
        }
        const bool known = !was.unreadable.empty();
        if (!is.unreadable.empty()) {
            classify.add(was, unreadable_status(is.unreadable), known ? "known-since-registration" : "",
                known ? Severity::Warning : Severity::Failure); // decision 0010
            if (known) {
                result.current_entries.push_back(was);
            }
            continue;
        }
        if (known && was.type != 'f') {
            classify.add(was, "Unbaselined", "readable now, unreadable at registration", Severity::Warning);
            result.current_entries.push_back(was);
            continue;
        }
        if (was.type == 'd') {
            classify.add(was, "Verified", "", Severity::None);
            result.current_entries.push_back(is);
        } else if (was.type == 'l') {
            if (classify.compare_link(was, is)) {
                result.current_entries.push_back(is);
            }
        } else if (!known && input.content_verified.contains(entry_key(was)) && is.size == was.size) {
            // R-104: content matched in the run being resumed; only metadata is rechecked.
            result.coverage_bytes += was.size.value_or(0);
            classify.verified_or_metadata(was, is);
            result.current_entries.push_back(carried_entry(was, is));
        } else {
            to_hash.emplace_back(&was, &now);
        }
    }

    // Re-read and re-hash every recorded file (R-100, R-101).
    std::vector<HashJob> jobs;
    jobs.reserve(to_hash.size());
    for (std::size_t index = 0; index < to_hash.size(); ++index) {
        // The size just scanned, of the file about to be read, lets progress
        // count bytes (share done, time left), as registration does.
        const auto& scanned_file = *to_hash[index].second;
        jobs.push_back({index, scanned_file.native, scanned_file.entry.size.value_or(0)});
    }
    HashPipelineOptions hash_options;
    hash_options.readers = default_reader_count(input.rotational);
    // Compared by chunk, a file's chunk hashes come from the read that hashes
    // it whole (R-101), at the revision's chunk size: there is no other read.
    // A revision without a chunk size gives no chunk hashes (decision 0032),
    // unless they are added, at the chunk size to add (decision 0033).
    const auto adding = input.header->chunk_size == 0 ? input.adding_chunk_size : 0;
    const auto chunk_size = adding > 0 ? adding : input.compare_chunks ? input.header->chunk_size : 0;
    hash_options.chunk_size = chunk_size;

    // The files whose chunk hashes cannot be relied on, with their warning.
    std::vector<std::pair<const Entry*, std::string>> unreliable;

    std::vector<std::string> pending_checkpoint;
    auto last_checkpoint = std::chrono::steady_clock::now();
    // False when the caller stops the comparison.
    const auto flush_checkpoint = [&] {
        bool go_on = true;
        if (input.checkpoint && !pending_checkpoint.empty()) {
            go_on = input.checkpoint(pending_checkpoint);
        }
        pending_checkpoint.clear();
        last_checkpoint = std::chrono::steady_clock::now();
        return go_on;
    };

    // A file that could not be read, as the entry's own (decision 0010).
    const auto unreadable = [&](const HashResult& hash) {
        const auto& was = *to_hash[hash.id].first;
        const bool known = !was.unreadable.empty();
        classify.add(was, unreadable_status(unreadable_reason(*hash.error)),
            known ? "known-since-registration" : hash.error->message, known ? Severity::Warning : Severity::Failure);
        if (known) {
            result.current_entries.push_back(was);
        }
    };
    // Decision 0034: files whose reads failed with I/O errors since the last
    // read that succeeded, while the volume stayed attached. They are the
    // entries' own once the device answers again, or once every file was
    // read; as many in a row as io_errors_that_stop are a device that stopped
    // answering, and none of them is classified.
    std::vector<HashResult> held;
    const auto classify_held = [&] {
        for (const auto& hash : held) {
            unreadable(hash);
        }
        held.clear();
    };

    const auto hashed = hash_files(files, jobs, hash_options, cancel, progress, [&](HashResult&& hash) {
        const auto& was = *to_hash[hash.id].first;
        const auto& is = to_hash[hash.id].second->entry;
        const bool known = !was.unreadable.empty(); // decision 0010
        if (hash.error) {
            // Decision 0034: a read that failed because the device went away,
            // or stopped answering, says nothing of the entry. The volume is
            // looked up again before the entry is classified; gone, or no
            // longer answering, the comparison stops, resumable.
            const auto failure = input.watch ? input.watch->failed(*hash.error)
                                 : hash.error->kind == IoErrorKind::DeviceRemoved ? ReadFailure::Stop
                                                                                  : ReadFailure::Entry;
            if (failure == ReadFailure::Stop) {
                result.incomplete = true;
                result.device_stop = device_stop();
                return false;
            }
            if (failure == ReadFailure::Held) {
                held.push_back(std::move(hash));
            } else {
                unreadable(hash);
            }
            return true;
        }
        if (input.watch) {
            input.watch->answered();
        }
        classify_held();
        if (known) {
            classify.add(was, "Unbaselined", "readable now, unreadable at registration", Severity::Warning);
            result.current_entries.push_back(was);
            return true;
        }
        if (was.size && hash.size != *was.size) {
            // A size change stays one, whatever its chunks.
            classify.add(was, "Modified", "size differs", Severity::Failure);
            return true;
        }
        const auto differing = differing_chunks(was, hash);
        const bool file_hashes_match = hash.b3 == was.b3 && hash.sha256 == was.sha256;
        // Read by chunk, every chunk of a file with chunk lines matches.
        const bool chunks_match = !was.chunks.empty() && hash.chunks.size() == was.chunks.size() && differing.empty();
        if (!was.size || (!file_hashes_match && !chunks_match)) {
            classify.add(content_differs(was, differing, chunk_size), Severity::Failure);
            return true;
        }
        result.coverage_bytes += hash.size; // read and matched
        classify.verified_or_metadata(was, is);
        result.current_entries.push_back(carried_entry(was, is));
        // The content matches, by both file hashes or by every chunk: a hash
        // recorded that does not proves the manifest wrong, not the file
        // (decision 0032). Not checkpointed: a resume reads it again, and
        // warns again (R-104).
        if (!file_hashes_match) {
            unreliable.emplace_back(&was, unreliable_file_hashes(was, hash));
            return true;
        }
        if (!differing.empty()) {
            unreliable.emplace_back(&was, unreliable_chunk_hashes(was, differing));
            return true;
        }
        // The chunk hashes to add, of content that matched (decision 0033),
        // before the file joins a checkpoint.
        if (adding > 0 && !hash.chunks.empty() && input.chunks_read) {
            input.chunks_read(was, std::move(hash.chunks));
        }
        // R-104: checkpoint every 1000 items or 60 seconds.
        pending_checkpoint.push_back(entry_key(was));
        if (pending_checkpoint.size() >= 1000 ||
            std::chrono::steady_clock::now() - last_checkpoint > std::chrono::seconds(60)) {
            return flush_checkpoint(); // false stops hashing
        }
        return true;
    });
    // The last matches, also of a run that stops: a resume skips them.
    if (!flush_checkpoint() || !hashed) {
        result.incomplete = true;
    }
    // Every file was read, and the last ones could not be: theirs. A
    // comparison that stopped leaves them to its resume, which reads them
    // again.
    if (!result.incomplete) {
        classify_held();
    }
    std::sort(result.current_entries.begin(), result.current_entries.end(), canonical_less);
    // Report items in canonical order (R-43). They were added in two passes
    // (the merge above, then the hash callback in whatever order the readers
    // finished), so without this the same media could list them differently
    // on every run. There is one item per (path, raw, stream); a file's stream
    // is "", which sorts before any stream name, just like canonical_less.
    std::sort(result.items.begin(), result.items.end(),
        [](const VerifyItemRecord& left, const VerifyItemRecord& right) {
            return std::tie(left.path.p, left.path.raw, left.stream) <
                   std::tie(right.path.p, right.path.raw, right.stream);
        });
    // The warnings likewise, by the canonical order of their files.
    std::sort(unreliable.begin(), unreliable.end(),
        [](const auto& left, const auto& right) { return canonical_less(*left.first, *right.first); });
    for (auto& file : unreliable) {
        result.warnings.push_back(std::move(file.second));
    }
    return result;
}

bool records_name_collisions(const std::vector<Entry>& entries)
{
    return std::ranges::any_of(entries, [](const Entry& entry) {
        return std::ranges::any_of(entry.warn, [](const std::string& warning) {
            return warning == "nfc-collision" || warning == "case-collision";
        });
    });
}

} // namespace va
