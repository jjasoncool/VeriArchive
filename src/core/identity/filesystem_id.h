#pragma once
// Filesystem identifiers normalised so the same volume reads the same on
// Windows and Linux (HANDOFF §B1.3, decision 0006).
#include <string>
#include <string_view>

namespace va {

// On-disk format name: "ntfs", "exfat", "vfat", "ext4", ... never a driver name
// such as "ntfs3" or "ntfs-3g" (decision 0006). Unknown names are lower-cased.
std::string normalise_filesystem_type(std::string_view raw);

struct FilesystemId {
    std::string uuid;        // VolumeInfo::filesystem_uuid
    std::string uuid_short;  // VolumeInfo::filesystem_uuid_short (32-bit serial, 8 hex)
};

// `filesystem_type` must already be normalised. Rules (§B1.3):
//  - ntfs: 64-bit serial as 16 lowercase hex; the short form is its low 32 bits,
//    which is what Windows' GetVolumeInformationW reports. A 32-bit-only
//    reading yields an empty uuid and the short form only.
//  - exfat, vfat: 32-bit serial as 8 lowercase hex without the dash, in both fields.
//  - everything else (ext4, xfs, btrfs, ...): the lowercase UUID, no short form.
FilesystemId normalise_filesystem_id(std::string_view filesystem_type, std::string_view raw);

} // namespace va
