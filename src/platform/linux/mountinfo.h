#pragma once
// Parser for /proc/self/mountinfo. Mount boundaries come from here rather than
// from st_dev, because btrfs subvolumes have distinct st_dev on one volume (R-30).
// Also whether a path lies on a network filesystem, from the mounts and the
// type that statfs reports: plain C++, so it is tested on every OS.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va::platform::linux_mounts {

struct Mount {
    int mount_id = 0;
    int parent_id = 0;
    unsigned major = 0;
    unsigned minor = 0;
    std::string root;                    // path inside the filesystem that is mounted
    std::filesystem::path mount_point;   // where it is mounted
    std::string options;                 // per-mount options, e.g. "ro,noatime"
    std::string filesystem_type;         // e.g. "ext4", "fuseblk", "fuse.sshfs"
    std::string source;                  // e.g. "/dev/sdb1"
    std::string super_options;
};

// Parses mountinfo text; malformed lines are skipped.
std::vector<Mount> parse_mountinfo(std::string_view text);

// Reads /proc/self/mountinfo; empty when it cannot be read.
std::vector<Mount> read_mountinfo();

// The mount whose mount point is the longest prefix of `path` (the mount that
// contains it). `path` should be absolute and canonical.
std::optional<Mount> mount_containing(const std::vector<Mount>& mounts,
    const std::filesystem::path& path);

// True for network or remote filesystem types (nfs, cifs, fuse.sshfs, ...).
bool is_network_filesystem_type(std::string_view type);

// Whether `path` (absolute and canonical) lies on a network or remote
// filesystem: the type of the mount that contains it (mount_containing), or
// nothing when no mount contains it.
std::optional<bool> on_network_filesystem(const std::vector<Mount>& mounts, const std::filesystem::path& path);

// The filesystem type that statfs reports (f_type, a magic number such as
// 0x6969 for NFS), as its 32 bits: every Linux magic number fits in them,
// while the C library's own type for it differs (long in glibc, unsigned long
// in musl, 32 bits wide on 32-bit systems).
using FilesystemMagic = std::uint32_t;

// Whether a path lies on a network or remote filesystem: told by the type that
// statfs reported for it (`statfs_type`: NFS, SMB, AFS, ... by their magic
// numbers), then by the type of the mount in `mounts` that contains it
// (on_network_filesystem). `path` should be absolute and canonical. Neither
// tells when statfs failed (no `statfs_type`) or no mount contains the path,
// and then each check decides on its own:
//  - the repository's (R-83 step 4) fails closed: the path counts as on a
//    network filesystem, so SQLite is never placed on a share unawares;
//  - an archive root's (decision 0029) does not refuse the root: its volume
//    lookup then says what is wrong.
bool repository_on_network(std::optional<FilesystemMagic> statfs_type, const std::vector<Mount>& mounts,
    const std::filesystem::path& path);
bool archive_root_on_network(std::optional<FilesystemMagic> statfs_type, const std::vector<Mount>& mounts,
    const std::filesystem::path& path);

} // namespace va::platform::linux_mounts
