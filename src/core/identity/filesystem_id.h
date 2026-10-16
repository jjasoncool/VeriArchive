#pragma once
// Filesystem identifiers normalised so the same volume reads the same on
// Windows and Linux (HANDOFF §B1.3, decisions 0006 and 0021).
#include <cstdint>
#include <optional>
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

// Decision 0021: on Linux without the udev database the UUID of an ext2/3/4 or
// btrfs volume cannot be read, but the kernel's filesystem id (statfs f_fsid)
// is computed from it. That id is recorded in filesystem_uuid as "fsid-" and 16
// lowercase hex digits, the form also used for volumes without a block device
// (ZFS datasets, overlayfs); it is not a UUID, so it is compared only with
// another kernel filesystem id.
std::string kernel_filesystem_id(std::uint64_t fsid);
bool is_kernel_filesystem_id(std::string_view filesystem_uuid);

// The kernel reports f_fsid as two 32-bit words. They are combined the way
// 64-bit glibc's statvfs() does (first word low, second word high), so ids
// recorded before decision 0021 keep their value. musl and 32-bit glibc keep
// only the first word in statvfs(), which is why the words are read with
// statfs() and combined here, the same way under every C library.
std::uint64_t combine_kernel_filesystem_id(std::int32_t first, std::int32_t second);

// Decision 0021: the id that stands in for the UUID of a volume on a block
// device whose UUID cannot be read, or nullopt when there is none:
//  - only ext2, ext3, ext4 and btrfs compute their kernel id from the UUID, so
//    it is the same on every machine. Others, such as xfs, derive it from the
//    device number, which changes when the device is attached elsewhere.
//  - zero is no id: ext2/3/4 report it for a filesystem whose UUID is all
//    zeros, which udev does not report either, so every such volume would
//    read the same.
// `filesystem_type` must be normalised.
std::optional<std::string> kernel_filesystem_id_for(std::string_view filesystem_type,
    std::uint64_t fsid);

// Windows' storage driver reports an NVMe namespace's EUI-64/NGUID in the serial
// field, formatted like "1234_5678_9ABC_DEF0_0011_2233_4455_6677.". Linux shows the
// same identifier as the WWN "eui.123456789abcdef00011223344556677". Returns that
// WWN form, or nullopt when `serial` is not in this format.
std::optional<std::string> nvme_wwn_from_windows_serial(std::string_view serial);

} // namespace va
