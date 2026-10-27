#pragma once
// Small, independently testable rules used when comparing a manifest entry with
// what is on the media now.
#include "common/archive_path.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace va {

// Whether `child` lies somewhere below the directory `directory`. For a name
// that is not valid Unicode, `p` is only a display form, so the raw native
// path must continue with a '/' as well (hex "2f", or "2f00" in UTF-16LE).
bool is_below(const ArchivePath& child, const ArchivePath& directory);

// Whether `entries`, in canonical order (R-43), hold anything below the
// directory `directory`; `path_of` gives an element's ArchivePath. A manifest
// records a directory only when it is empty or cannot be listed (R-49), and a
// scan the same way: a directory with contents shows only through its
// descendants (R-50 in a comparison, decision 0020, and in a verification).
// Binary search: the entries below "x" are the run of paths that start with
// "x/".
template <typename Entries, typename PathOf>
bool holds_directory(const Entries& entries, const ArchivePath& directory, PathOf path_of)
{
    const auto prefix = directory.p + '/';
    auto found = std::lower_bound(entries.begin(), entries.end(), prefix,
        [&](const auto& element, const std::string& value) { return path_of(element).p < value; });
    for (; found != entries.end() && path_of(*found).p.starts_with(prefix); ++found) {
        if (is_below(path_of(*found), directory)) {
            return true;
        }
    }
    return false;
}

// R-52: is the recorded mtime equal to the current one within the tolerance of
// the *verifying* filesystem?
//  - vfat (FAT32): 2 s resolution and local time: informational only (always true)
//  - exfat: within 10 ms, or off by whole quarter hours up to 14 h (a missing
//    UTC-offset field makes times shift by the time zone)
//  - everything else (ntfs, ext4, xfs, btrfs, ...): within 1 s
bool mtime_matches(std::string_view filesystem_type, std::int64_t recorded_ns, std::int64_t current_ns);

// R-44: can this manifest path exist on Windows? False for reserved names
// (CON, NUL, COM1, ...), the characters < > : " | ? * \ and control
// characters, a trailing space or dot, or a component over 255 UTF-16 units.
bool representable_on_windows(std::string_view archive_path);

} // namespace va
