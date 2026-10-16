#include "platform/linux/mountinfo.h"

#include <exception>
#include <fstream>
#include <iterator>

namespace va::platform::linux_mounts {
namespace {

// mountinfo escapes space, tab, newline and backslash as \040 \011 \012 \134.
std::string unescape(std::string_view field)
{
    std::string result;
    for (std::size_t index = 0; index < field.size(); ++index) {
        if (field[index] == '\\' && index + 3 < field.size()) {
            const auto digits = field.substr(index + 1, 3);
            if (digits.size() == 3 && digits[0] >= '0' && digits[0] <= '3' &&
                digits[1] >= '0' && digits[1] <= '7' && digits[2] >= '0' && digits[2] <= '7') {
                result += static_cast<char>((digits[0] - '0') * 64 + (digits[1] - '0') * 8 +
                                            (digits[2] - '0'));
                index += 3;
                continue;
            }
        }
        result += field[index];
    }
    return result;
}

std::vector<std::string> split_spaces(std::string_view line)
{
    std::vector<std::string> fields;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ') {
            ++index;
        }
        const auto start = index;
        while (index < line.size() && line[index] != ' ') {
            ++index;
        }
        if (index > start) {
            fields.emplace_back(line.substr(start, index - start));
        }
    }
    return fields;
}

std::optional<Mount> parse_line(std::string_view line)
{
    const auto fields = split_spaces(line);
    // At least: id parent major:minor root mount-point options "-" type source super
    std::size_t separator = 0;
    for (std::size_t index = 6; index < fields.size(); ++index) {
        if (fields[index] == "-") {
            separator = index;
            break;
        }
    }
    if (separator == 0 || separator + 3 > fields.size()) {
        return std::nullopt;
    }

    Mount mount;
    try {
        mount.mount_id = std::stoi(fields[0]);
        mount.parent_id = std::stoi(fields[1]);
        const auto colon = fields[2].find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        mount.major = static_cast<unsigned>(std::stoul(fields[2].substr(0, colon)));
        mount.minor = static_cast<unsigned>(std::stoul(fields[2].substr(colon + 1)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    mount.root = unescape(fields[3]);
    mount.mount_point = std::filesystem::path(unescape(fields[4]));
    mount.options = fields[5];
    mount.filesystem_type = unescape(fields[separator + 1]);
    mount.source = unescape(fields[separator + 2]);
    if (separator + 3 < fields.size()) {
        mount.super_options = fields[separator + 3];
    }
    return mount;
}

bool is_prefix(const std::filesystem::path& prefix, const std::filesystem::path& path)
{
    auto prefix_it = prefix.begin();
    auto path_it = path.begin();
    for (; prefix_it != prefix.end(); ++prefix_it, ++path_it) {
        if (prefix_it->empty()) {
            continue; // trailing separator
        }
        if (path_it == path.end() || *prefix_it != *path_it) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<Mount> parse_mountinfo(std::string_view text)
{
    std::vector<Mount> mounts;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (auto mount = parse_line(text.substr(start, end - start))) {
            mounts.push_back(std::move(*mount));
        }
        start = end + 1;
    }
    return mounts;
}

std::vector<Mount> read_mountinfo()
{
    std::ifstream input("/proc/self/mountinfo", std::ios::binary);
    if (!input) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return parse_mountinfo(text);
}

std::optional<Mount> mount_containing(const std::vector<Mount>& mounts,
    const std::filesystem::path& path)
{
    const Mount* best = nullptr;
    std::size_t best_length = 0;
    for (const auto& mount : mounts) {
        const auto length = mount.mount_point.native().size();
        // Later entries in mountinfo shadow earlier ones on the same mount point.
        if (is_prefix(mount.mount_point, path) && (best == nullptr || length >= best_length)) {
            best = &mount;
            best_length = length;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return *best;
}

bool is_network_filesystem_type(std::string_view type)
{
    constexpr std::string_view network_types[] = {
        "nfs", "nfs4", "cifs", "smb3", "smbfs", "ncpfs", "9p", "afs", "ceph", "glusterfs",
        "lustre", "davfs", "coda", "orangefs", "beegfs", "gpfs", "ocfs2",
    };
    constexpr std::string_view network_fuse_subtypes[] = {
        "sshfs", "rclone", "s3fs", "gcsfuse", "davfs2", "curlftpfs", "glusterfs", "cephfs",
        "goofys", "blobfuse", "blobfuse2", "juicefs", "seaweedfs", "smbnetfs",
    };
    for (const auto candidate : network_types) {
        if (type == candidate) {
            return true;
        }
    }
    if (type.starts_with("fuse.")) {
        const auto subtype = type.substr(5);
        for (const auto candidate : network_fuse_subtypes) {
            if (subtype == candidate) {
                return true;
            }
        }
    }
    return false;
}

std::optional<bool> on_network_filesystem(const std::vector<Mount>& mounts, const std::filesystem::path& path)
{
    const auto mount = mount_containing(mounts, path);
    if (!mount) {
        return std::nullopt;
    }
    return is_network_filesystem_type(mount->filesystem_type);
}

} // namespace va::platform::linux_mounts
