#pragma once
// Parser for /proc/self/mountinfo. Mount boundaries come from here rather than
// from st_dev, because btrfs subvolumes have distinct st_dev on one volume (R-30).
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

} // namespace va::platform::linux_mounts
