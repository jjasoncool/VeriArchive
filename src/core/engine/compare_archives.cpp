#include "core/engine/compare_archives.h"

#include "core/engine/item_rules.h"
#include "core/engine/recorded_manifest.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <tuple>

namespace va {
namespace {

std::unexpected<CompareFailure> fail(std::string failure_class, std::string message)
{
    return std::unexpected(CompareFailure{std::move(failure_class), std::move(message)});
}

// The identity of an entry in both archives: (path, raw, stream), the key the
// database uses (decision 0002). The type is left out on purpose, so a file
// that became a directory meets its old self and is reported once, as
// TypeChanged, instead of as Missing plus New. Manifests are in canonical
// order (R-43), and that order sorts these keys too: within one path the file,
// directory or link has stream "" and comes before its streams.
auto key_of(const Entry& entry)
{
    return std::tie(entry.path.p, entry.path.raw, entry.stream);
}

// Whether `entries` (canonical order) hold anything below `directory`: a
// directory with contents (item_rules.h).
bool holds_directory(const std::vector<Entry>& entries, const ArchivePath& directory)
{
    return va::holds_directory(entries, directory, [](const Entry& entry) -> const ArchivePath& { return entry.path; });
}

// Directories that could not be listed at registration (decision 0010), by
// path. Nothing is known about what they held.
class UnreadableDirectories {
public:
    explicit UnreadableDirectories(const std::vector<Entry>& entries)
    {
        for (const auto& entry : entries) {
            if (entry.type == 'd' && !entry.unreadable.empty()) {
                by_path_[entry.path.p].push_back(&entry);
            }
        }
    }

    // Whether one of `path`'s ancestors is such a directory.
    bool contain(const ArchivePath& path) const
    {
        if (by_path_.empty()) {
            return false; // the usual case: skip the ancestor walk
        }
        for (auto slash = path.p.find('/'); slash != std::string::npos; slash = path.p.find('/', slash + 1)) {
            const auto found = by_path_.find(path.p.substr(0, slash));
            if (found == by_path_.end()) {
                continue;
            }
            for (const Entry* directory : found->second) {
                if (is_below(path, directory->path)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::map<std::string, std::vector<const Entry*>> by_path_;
};

// Why an entry has no content hash, or nothing when it has one. Directories
// never have hashes; for them only "could not be listed" counts.
std::optional<std::string> missing_hash_reason(const Entry& entry)
{
    if (!entry.unreadable.empty()) {
        return entry.unreadable;
    }
    if (entry.type != 'd' && (entry.b3.empty() || entry.sha256.empty())) {
        return "other"; // a hand-made manifest; this application always names the reason
    }
    return std::nullopt;
}

// Details are fixed sentences ("unreadable in A (media)"), so the interface
// can translate each of them (decision 0017).
std::string unreadable_detail(char side, const std::string& reason)
{
    return std::string("unreadable in ") + side + " (" + reason + ")";
}

std::string type_change(char from, char to)
{
    return std::string(1, from) + " -> " + to;
}

bool is_absolute_link(const Entry& entry)
{
    return std::find(entry.warn.begin(), entry.warn.end(), "link-absolute") != entry.warn.end();
}

// Classifies entries and collects the differences in the order it is called,
// which is canonical order.
class Classifier {
public:
    Classifier(const RecordedManifest& a, const RecordedManifest& b, ArchiveComparison& result)
        : a_(a), b_(b), result_(result), unreadable_a_(a.entries), unreadable_b_(b.entries),
          same_os_(a.header.os == b.header.os)
    {
    }

    // The same key in both archives.
    void in_both(const Entry& a, const Entry& b)
    {
        if (a.type != b.type) {
            add(a, "TypeChanged", type_change(a.type, b.type));
            return;
        }
        const auto reason_a = missing_hash_reason(a);
        const auto reason_b = missing_hash_reason(b);
        if (reason_a && reason_b) {
            add(a, "Unbaselined", "unreadable in both archives");
        } else if (reason_a) {
            add(a, "Unbaselined", unreadable_detail('A', *reason_a));
        } else if (reason_b) {
            add(a, "Unbaselined", unreadable_detail('B', *reason_b));
        } else if (a.type == 'd' || (a.b3 == b.b3 && a.sha256 == b.sha256)) {
            // The whole-file hashes, never chunk hashes (decision 0032): an
            // archive registered by V1 has none, and the same content at
            // another chunk size has others.
            ++result_.same; // an empty directory has no content: being there is all
        } else if (a.type == 'l') {
            // Decision 0008: a driver may translate an absolute target when the
            // disk is read on another OS, so then it is only a warning.
            if (!same_os_ && (is_absolute_link(a) || is_absolute_link(b))) {
                add(a, "MetadataMismatch", "link-target-translated");
            } else {
                add(a, "Modified", "link target differs");
            }
        } else if (a.size && b.size && *a.size != *b.size) {
            add(a, "Modified", "size " + std::to_string(*a.size) + " -> " + std::to_string(*b.size));
        } else {
            add(a, "Modified", "content differs");
        }
    }

    void only_in_a(const Entry& a)
    {
        if (a.type != 's' && holds_directory(b_.entries, a.path)) {
            beside_directory(a, 'A', type_change(a.type, 'd'));
        } else if (unreadable_b_.contain(a.path)) {
            add(a, "Unbaselined", "inside a directory unreadable in B");
        } else {
            add(a, "Missing", "");
        }
    }

    void only_in_b(const Entry& b)
    {
        if (b.type != 's' && holds_directory(a_.entries, b.path)) {
            beside_directory(b, 'B', type_change('d', b.type));
        } else if (unreadable_a_.contain(b.path)) {
            add(b, "Unbaselined", "inside a directory unreadable in A");
        } else {
            add(b, "New", "");
        }
    }

private:
    // `entry` is recorded only in archive `side`, and the other archive holds a
    // directory with contents at that path. An empty directory recorded on one
    // side is then the same directory; anything else changed its type.
    void beside_directory(const Entry& entry, char side, const std::string& type_changed)
    {
        if (entry.type != 'd') {
            add(entry, "TypeChanged", type_changed);
        } else if (!entry.unreadable.empty()) {
            add(entry, "Unbaselined", unreadable_detail(side, entry.unreadable));
        } else {
            ++result_.same;
        }
    }

    void add(const Entry& entry, const std::string& status, std::string detail)
    {
        ++result_.counts[status];
        result_.differences.push_back({entry.path.p, entry.stream, status, std::move(detail)});
    }

    const RecordedManifest& a_;
    const RecordedManifest& b_;
    ArchiveComparison& result_;
    UnreadableDirectories unreadable_a_;
    UnreadableDirectories unreadable_b_;
    bool same_os_;
};

std::expected<ArchiveRecord, CompareFailure> find(ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto archive = store.find_archive(archive_uuid);
    if (!archive) {
        return fail("RepositoryError", archive.error().message);
    }
    if (!*archive) {
        return fail("RepositoryError", "no archive " + archive_uuid);
    }
    if ((*archive)->current_revision == 0) {
        // Its first registration never finished, so there is no manifest yet.
        return fail("RepositoryError", "archive " + archive_uuid + " has no revision yet");
    }
    return **archive;
}

std::expected<RecordedManifest, CompareFailure> load(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, const ArchiveRecord& archive)
{
    auto manifest = load_recorded_manifest(
        store, repository_files, layout, archive.archive_uuid, archive.current_revision);
    if (!manifest) {
        return fail(manifest.error().failure_class,
            "cannot read the manifest of " + archive.label + " revision " +
                std::to_string(archive.current_revision) + ": " + manifest.error().message);
    }
    return std::move(*manifest);
}

} // namespace

std::expected<ArchiveComparison, CompareFailure> compare_archives(
    ArchiveStore& store,
    IFileSystemAccess& repository_files,
    const RepositoryLayout& layout,
    const Uuid& archive_a,
    const Uuid& archive_b,
    CancelToken& cancel,
    IProgressSink& progress)
{
    const auto cancelled = [] { return fail("Cancelled", "the comparison was cancelled"); };
    const auto record_a = find(store, archive_a);
    if (!record_a) {
        return std::unexpected(record_a.error());
    }
    const auto record_b = find(store, archive_b);
    if (!record_b) {
        return std::unexpected(record_b.error());
    }

    // Loading is the slow part: each manifest is read, hashed and parsed whole.
    if (cancel.cancelled()) {
        return cancelled();
    }
    const auto manifest_a = load(store, repository_files, layout, *record_a);
    if (!manifest_a) {
        return std::unexpected(manifest_a.error());
    }
    if (cancel.cancelled()) {
        return cancelled();
    }
    // An archive compared with itself is loaded once.
    std::optional<RecordedManifest> loaded_b;
    if (archive_b != archive_a) {
        auto manifest_b = load(store, repository_files, layout, *record_b);
        if (!manifest_b) {
            return std::unexpected(manifest_b.error());
        }
        loaded_b = std::move(*manifest_b);
    }
    const RecordedManifest& a = *manifest_a;
    const RecordedManifest& b = loaded_b ? *loaded_b : *manifest_a;

    ArchiveComparison result;
    result.archive_a = archive_a;
    result.archive_b = archive_b;
    result.label_a = record_a->label;
    result.label_b = record_b->label;
    result.revision_a = record_a->current_revision;
    result.revision_b = record_b->current_revision;
    result.created_a = a.header.created;
    result.created_b = b.header.created;
    result.entries_a = a.entries.size();
    result.entries_b = b.entries.size();

    // Merge the two sorted lists by key: linear in the number of entries.
    Classifier classify(a, b, result);
    const std::uint64_t total = result.entries_a + result.entries_b;
    progress.on_progress({0, 0, 0, total, ""});
    auto last_report = std::chrono::steady_clock::now();
    std::size_t i = 0;
    std::size_t j = 0;
    std::uint64_t steps = 0;
    while (i < a.entries.size() || j < b.entries.size()) {
        // Look at the clock and the token only every few thousand entries:
        // a step is much cheaper than either.
        if (++steps % 4096 == 0) {
            if (cancel.cancelled()) {
                return cancelled();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(250)) {
                const auto& current = i < a.entries.size() ? a.entries[i] : b.entries[j];
                progress.on_progress({0, 0, i + j, total, current.path.p});
                last_report = now;
            }
        }
        if (j == b.entries.size() || (i < a.entries.size() && key_of(a.entries[i]) < key_of(b.entries[j]))) {
            classify.only_in_a(a.entries[i++]);
        } else if (i == a.entries.size() || key_of(b.entries[j]) < key_of(a.entries[i])) {
            classify.only_in_b(b.entries[j++]);
        } else {
            classify.in_both(a.entries[i++], b.entries[j++]);
        }
    }
    progress.on_progress({0, 0, total, total, ""});

    // Unbaselined and a translated link target leave the content unproven, not
    // different; everything else is a real difference.
    bool different = false;
    for (const auto& [status, count] : result.counts) {
        if (status != "Unbaselined" && status != "MetadataMismatch") {
            different = true;
        }
    }
    if (different) {
        result.result = "DIFFERENT";
    } else {
        result.result = result.differences.empty() ? "IDENTICAL" : "IDENTICAL_WITH_WARNINGS";
    }
    return result;
}

} // namespace va
