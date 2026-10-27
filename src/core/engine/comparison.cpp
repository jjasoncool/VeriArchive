#include "core/engine/comparison.h"

#include "core/engine/item_rules.h"
#include "core/hash/hash_pipeline.h"
#include "core/scan/scanner.h"

#include <algorithm>
#include <chrono>
#include <tuple>

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

class Classifier {
public:
    Classifier(ComparisonResult& result, const ComparisonInput& input)
        : result_(result), input_(input), same_os_(input.header->os == input.host_os)
    {
    }

    void add(const Entry& entry, const std::string& status, const std::string& detail, Severity severity)
    {
        ++result_.counts[status];
        if (severity == Severity::Failure) {
            result_.content_failure = true;
        } else if (severity == Severity::Warning) {
            result_.warning = true;
        }
        if (status != "Verified") {
            result_.items.push_back({"", entry.path, entry.stream, status, detail});
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
// recorded hashes (decision 0004).
Entry carried_entry(const Entry& recorded, const Entry& current)
{
    Entry entry = current;
    entry.b3 = recorded.b3;
    entry.sha256 = recorded.sha256;
    entry.size = recorded.size;
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
    scan_options.volume_attached = input.volume_attached;
    auto scanned = scan_tree(files, input.root, scan_options, cancel, progress);
    if (!scanned) {
        const auto kind = scanned.error().kind;
        if (kind == IoErrorKind::Cancelled || kind == IoErrorKind::DeviceRemoved) {
            result.incomplete = true;
            result.volume_gone = kind == IoErrorKind::DeviceRemoved;
        } else {
            result.scan_error = scanned.error().message;
        }
        return result;
    }
    Classifier classify(result, input);

    // Directory facts needed to explain missing and new entries.
    std::set<std::string> known_unreadable_dirs; // unreadable at registration
    std::set<std::string> recorded_dirs;         // every directory implied by the manifest
    for (const auto& entry : recorded) {
        if (entry.type == 'd' && !entry.unreadable.empty()) {
            known_unreadable_dirs.insert(entry.path.p);
        }
        for_each_ancestor(entry.path.p, [&](const std::string& dir) { recorded_dirs.insert(dir); });
    }
    std::map<std::string, std::string> unreadable_dirs_now;
    std::set<std::string> current_dirs;
    for (const auto& scanned_entry : scanned->entries) {
        const auto& entry = scanned_entry.entry;
        if (entry.type == 'd' && !entry.unreadable.empty()) {
            unreadable_dirs_now[entry.path.p] = entry.unreadable;
        }
        for_each_ancestor(entry.path.p, [&](const std::string& dir) { current_dirs.insert(dir); });
    }
    std::set<std::string> mount_points_now;
    for (const auto& mount_point : scanned->skipped_mount_points) {
        mount_points_now.insert(mount_point.p);
    }

    const auto missing = [&](const Entry& entry) {
        if (entry.type == 'd' && current_dirs.contains(entry.path.p)) {
            classify.add(entry, "Verified", "", Severity::None); // now holds new entries
        } else if (ancestor_in(entry.path.p, mount_points_now)) {
            classify.add(entry, "SkippedMountPoint", "a mount point now covers this path", Severity::Failure);
        } else if (const auto dir = ancestor_in(entry.path.p, unreadable_dirs_now)) {
            classify.add(entry, unreadable_status(unreadable_dirs_now.at(*dir)),
                "directory " + *dir + " cannot be listed", Severity::Failure);
        } else if ((input.host_os == "windows" && !representable_on_windows(entry.path.p)) ||
                   (!entry.path.enc.empty() && !archive_path_to_native(entry.path))) {
            classify.add(entry, "Unrepresentable", "name cannot exist on " + input.host_os,
                Severity::Warning); // R-44
        } else {
            classify.add(entry, "Missing", "", Severity::Failure);
        }
    };
    const auto added = [&](const Entry& entry) {
        if (entry.type == 'd' && recorded_dirs.contains(entry.path.p)) {
            return; // existed implicitly; its recorded contents are reported individually
        }
        if (ancestor_in(entry.path.p, known_unreadable_dirs)) {
            classify.add(entry, "Unbaselined", "inside a directory unreadable at registration",
                Severity::Warning);
        } else {
            classify.add(entry, "New", "", Severity::Failure);
        }
    };

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

    const auto hashed = hash_files(files, jobs, hash_options, cancel, progress, [&](HashResult&& hash) {
        const auto& was = *to_hash[hash.id].first;
        const auto& is = to_hash[hash.id].second->entry;
        const bool known = !was.unreadable.empty(); // decision 0010
        if (hash.error) {
            // Decision 0034: a read that failed because the device went away
            // says nothing of the entry. The volume is looked up again before
            // the entry is classified; gone, the comparison stops, resumable.
            if (hash.error->kind == IoErrorKind::DeviceRemoved ||
                (input.volume_attached && !input.volume_attached())) {
                result.incomplete = true;
                result.volume_gone = true;
                return false;
            }
            classify.add(was, unreadable_status(unreadable_reason(*hash.error)),
                known ? "known-since-registration" : hash.error->message,
                known ? Severity::Warning : Severity::Failure);
            if (known) {
                result.current_entries.push_back(was);
            }
            return true;
        }
        if (known) {
            classify.add(was, "Unbaselined", "readable now, unreadable at registration", Severity::Warning);
            result.current_entries.push_back(was);
            return true;
        }
        if (hash.b3 != was.b3 || hash.sha256 != was.sha256 || !was.size || hash.size != *was.size) {
            classify.add(was, "Modified",
                was.size && hash.size != *was.size ? "size differs" : "content differs", Severity::Failure);
            return true;
        }
        result.coverage_bytes += hash.size; // read and matched
        classify.verified_or_metadata(was, is);
        result.current_entries.push_back(carried_entry(was, is));
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
    return result;
}

} // namespace va
