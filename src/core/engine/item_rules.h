#pragma once
// Small, independently testable rules used when comparing a manifest entry with
// what is on the media now.
#include <cstdint>
#include <string_view>

namespace va {

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
