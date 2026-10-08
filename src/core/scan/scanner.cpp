#include "core/scan/scanner.h"

#include "common/unicode.h"
#include "common/utf8.h"
#include "core/hash/hashers.h"

#include <algorithm>
#include <map>

namespace va {
namespace {

// '*' matches any run of characters; everything else must match exactly.
bool matches_pattern(std::string_view pattern, std::string_view name)
{
    const auto star = pattern.find('*');
    if (star == std::string_view::npos) {
        return pattern == name;
    }
    const auto prefix = pattern.substr(0, star);
    const auto rest = pattern.substr(star + 1);
    if (!name.starts_with(prefix)) {
        return false;
    }
    for (std::size_t start = prefix.size(); start <= name.size(); ++start) {
        if (matches_pattern(rest, name.substr(start))) {
            return true;
        }
    }
    return false;
}

bool excluded(const std::vector<std::string>& patterns, std::string_view name, bool directory)
{
    for (std::string_view pattern : patterns) {
        const bool directory_only = pattern.ends_with('/');
        if (directory_only) {
            pattern.remove_suffix(1);
            if (!directory) {
                continue;
            }
        }
        if (matches_pattern(pattern, name)) {
            return true;
        }
    }
    return false;
}

// R-42: names in one directory that differ only by Unicode normalisation or by
// case. Returns, per child index, the warning to record (or "").
std::vector<std::string> collision_warnings(const std::vector<DirEntry>& children)
{
    std::map<std::string, std::vector<std::size_t>> by_folded_name;
    std::vector<std::string> names;
    for (std::size_t index = 0; index < children.size(); ++index) {
        names.push_back(path_to_utf8(children[index].native_name));
        by_folded_name[unicode_fold(names.back())].push_back(index);
    }
    std::vector<std::string> warnings(children.size());
    for (const auto& [folded, indexes] : by_folded_name) {
        if (indexes.size() < 2) {
            continue;
        }
        for (const auto index : indexes) {
            const bool normalisation_only = std::any_of(indexes.begin(), indexes.end(),
                [&](std::size_t other) {
                    return other != index && unicode_nfc(names[other]) == unicode_nfc(names[index]);
                });
            warnings[index] = normalisation_only ? "nfc-collision" : "case-collision";
        }
    }
    return warnings;
}

class Walker {
public:
    Walker(IFileSystemAccess& files, const std::filesystem::path& root, const ScanOptions& options,
        CancelToken& cancel, IProgressSink& progress)
        : files_(files), root_(root), options_(options), cancel_(cancel), progress_(progress)
    {
    }

    std::expected<ScanResult, IoError> run()
    {
        const auto listing = files_.list(root_);
        if (!listing) {
            return std::unexpected(listing.error());
        }
        if (auto walked = walk_children({}, *listing); !walked) {
            return std::unexpected(walked.error());
        }
        std::sort(result_.entries.begin(), result_.entries.end(),
            [](const ScannedEntry& left, const ScannedEntry& right) {
                return canonical_less(left.entry, right.entry);
            });
        std::sort(result_.skipped_mount_points.begin(), result_.skipped_mount_points.end(),
            [](const ArchivePath& left, const ArchivePath& right) { return left.p < right.p; });
        return std::move(result_);
    }

private:
    // Returns how many entries the subtree contributed, so an empty directory
    // can be recorded (R-49).
    std::expected<std::size_t, IoError> walk_children(
        const std::filesystem::path& relative, const std::vector<DirEntry>& children)
    {
        if (cancel_.cancelled()) {
            return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "scan cancelled"});
        }
        const auto warnings = collision_warnings(children);
        std::size_t contributed = 0;
        for (std::size_t index = 0; index < children.size(); ++index) {
            const auto& child = children[index];
            const auto child_relative =
                relative.empty() ? child.native_name : relative / child.native_name;
            const bool directory = child.type == EntryType::Directory;
            if (relative.empty() &&
                excluded(options_.exclusions, path_to_utf8(child.native_name), directory)) {
                continue; // R-56: neither hashed nor reported as New
            }

            auto archive_path = archive_path_from_native(child_relative);
            if (!archive_path) {
                archive_path = ArchivePath{path_to_utf8(child_relative), "", ""};
            }

            Entry entry;
            entry.path = std::move(*archive_path);
            if (!warnings[index].empty()) {
                entry.warn.push_back(warnings[index]);
            }

            if (directory) {
                if (child.is_mount_point) {
                    result_.skipped_mount_points.push_back(entry.path);
                    continue;
                }
                const auto count = walk_directory(child_relative, entry);
                if (!count) {
                    return std::unexpected(count.error());
                }
                contributed += *count;
                continue;
            }

            entry.mtime_ns = child.mtime_ns;
            entry.mode = child.mode;
            entry.attrs = child.attrs;
            if (child.type == EntryType::Link) {
                entry.type = 'l';
                record_link(child_relative, entry);
            } else {
                entry.type = 'f';
                entry.size = child.size;
                if (child.type == EntryType::Special) {
                    entry.unreadable = "other"; // decision 0014
                } else if (child.attrs && (*child.attrs & recall_attribute_mask) != 0) {
                    entry.unreadable = "other"; // decision 0009: not on this disk
                }
            }
            add(std::move(entry), root_ / child_relative);
            ++contributed;
        }
        return contributed;
    }

    std::expected<std::size_t, IoError> walk_directory(
        const std::filesystem::path& relative, Entry& directory_entry)
    {
        directory_entry.type = 'd';
        const auto listing = files_.list(root_ / relative);
        if (!listing) {
            if (listing.error().kind == IoErrorKind::DeviceRemoved) {
                return std::unexpected(listing.error());
            }
            directory_entry.unreadable = unreadable_reason(listing.error());
            add(directory_entry, root_ / relative);
            return 1;
        }
        const auto count = walk_children(relative, *listing);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            add(directory_entry, root_ / relative); // empty directory (R-49)
            return 1;
        }
        return *count;
    }

    void record_link(const std::filesystem::path& relative, Entry& entry)
    {
        const auto target = files_.read_link(root_ / relative);
        if (!target) {
            entry.unreadable = unreadable_reason(target.error());
            return;
        }
        entry.size = target->size();
        entry.b3 = blake3_hex(*target);
        entry.sha256 = sha256_hex(*target);
        if (is_absolute_link_target(*target)) {
            entry.warn.push_back("link-absolute");
        }
    }

    void add(Entry entry, std::filesystem::path native)
    {
        result_.entries.push_back({std::move(entry), std::move(native)});
        if (result_.entries.size() % 1000 == 0) {
            progress_.on_progress({0, 0, result_.entries.size(), 0, result_.entries.back().entry.path.p});
        }
    }

    IFileSystemAccess& files_;
    std::filesystem::path root_;
    const ScanOptions& options_;
    CancelToken& cancel_;
    IProgressSink& progress_;
    ScanResult result_;
};

} // namespace

const std::vector<std::string>& default_exclusions()
{
    static const std::vector<std::string> exclusions{
        ".veriarchive/", "System Volume Information/", "$RECYCLE.BIN/", "$Recycle.Bin/",
        ".Trash-*/", ".Trashes/", "lost+found/",
    };
    return exclusions;
}

std::string unreadable_reason(const IoError& error)
{
    switch (error.kind) {
    case IoErrorKind::PermissionDenied:
        return "permission";
    case IoErrorKind::MediaError:
        return "media";
    default:
        return "other";
    }
}

bool is_absolute_link_target(std::string_view target)
{
    return target.starts_with('/') ||
           (target.size() >= 3 && target[1] == ':' && target[2] == '/' &&
               ((target[0] >= 'A' && target[0] <= 'Z') || (target[0] >= 'a' && target[0] <= 'z')));
}

std::expected<ScanResult, IoError> scan_tree(
    IFileSystemAccess& files,
    const std::filesystem::path& root,
    const ScanOptions& options,
    CancelToken& cancel,
    IProgressSink& progress)
{
    return Walker(files, root, options, cancel, progress).run();
}

} // namespace va
