// Linux media enumeration with unprivileged reads only (R-09):
//  - /proc/self/mountinfo for mount points (mount boundaries, R-30)
//  - the udev property database (/run/udev/data/b<major>:<minor>) for
//    filesystem type, UUID and label, and for device model/serial/WWN
//  - sysfs (/sys/dev/block/...) to walk from a volume to its physical disks,
//    through partitions, device-mapper (LUKS, LVM) and md RAID.
// The udev database files are read directly (the same data libudev returns).
#include "platform/media_enumerator.h"
#include "platform/linux/mountinfo.h"
#include "platform/linux/posix_util.h"

#include "core/identity/filesystem_id.h"

#include <algorithm>
#include <compare>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <utility>

namespace va::platform {
namespace {

using Properties = std::map<std::string, std::string>;

struct DeviceNumber {
    unsigned major = 0;
    unsigned minor = 0;
    auto operator<=>(const DeviceNumber&) const = default;
};

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return trim(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

// udev "_ENC" values escape bytes as \xHH (e.g. a space is \x20).
std::string decode_udev(std::string_view value)
{
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 3 < value.size() && value[index + 1] == 'x') {
            const auto hex = std::string(value.substr(index + 2, 2));
            try {
                result += static_cast<char>(std::stoi(hex, nullptr, 16));
                index += 3;
                continue;
            } catch (const std::exception&) {
            }
        }
        result += value[index];
    }
    return result;
}

Properties udev_properties(DeviceNumber number)
{
    Properties properties;
    std::ifstream input("/run/udev/data/b" + std::to_string(number.major) + ":" +
                        std::to_string(number.minor));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("E:")) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals != std::string::npos) {
            properties[line.substr(2, equals - 2)] = line.substr(equals + 1);
        }
    }
    return properties;
}

std::string property(const Properties& properties, const std::string& key)
{
    const auto found = properties.find(key);
    return found == properties.end() ? std::string{} : found->second;
}

// Prefers the escaped "_ENC" variant, which keeps spaces that the plain variant
// replaces with underscores.
std::string encoded_property(const Properties& properties, const std::string& key)
{
    const auto encoded = property(properties, key + "_ENC");
    return trim(encoded.empty() ? property(properties, key) : decode_udev(encoded));
}

std::optional<DeviceNumber> device_number_of(const std::string& device_node)
{
    struct stat information {};
    if (::stat(device_node.c_str(), &information) != 0 || !S_ISBLK(information.st_mode)) {
        return std::nullopt;
    }
    return DeviceNumber{major(information.st_rdev), minor(information.st_rdev)};
}

std::optional<DeviceNumber> parse_device_number(const std::string& text)
{
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    try {
        return DeviceNumber{static_cast<unsigned>(std::stoul(text.substr(0, colon))),
            static_cast<unsigned>(std::stoul(text.substr(colon + 1)))};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::filesystem::path sysfs_path(DeviceNumber number)
{
    std::error_code error;
    const auto path = std::filesystem::canonical(
        "/sys/dev/block/" + std::to_string(number.major) + ":" + std::to_string(number.minor), error);
    return error ? std::filesystem::path{} : path;
}

// Walks from a block device to the physical disks under it. Partitions lead to
// their parent disk; device-mapper and md devices lead through "slaves".
void collect_disks(const std::filesystem::path& block, std::vector<std::filesystem::path>& disks,
    std::string& container, int depth)
{
    std::error_code error;
    if (block.empty() || depth > 8) {
        return;
    }
    const auto slaves = block / "slaves";
    if (std::filesystem::is_directory(slaves, error) && !std::filesystem::is_empty(slaves, error)) {
        if (container.empty()) {
            const auto dm_uuid = read_text_file(block / "dm" / "uuid");
            if (dm_uuid.starts_with("CRYPT-")) {
                container = "luks";
            } else if (dm_uuid.starts_with("LVM-")) {
                container = "lvm";
            } else if (std::filesystem::exists(block / "md", error)) {
                container = "md";
            }
        }
        for (const auto& slave : std::filesystem::directory_iterator(slaves, error)) {
            collect_disks(std::filesystem::canonical(slave.path(), error), disks, container,
                depth + 1);
        }
        return;
    }
    const auto disk = std::filesystem::exists(block / "partition", error) ? block.parent_path()
                                                                           : block;
    if (std::find(disks.begin(), disks.end(), disk) == disks.end()) {
        disks.push_back(disk);
    }
}

std::string bus_name(const std::string& udev_bus, const std::string& disk_name)
{
    if (udev_bus == "ata") {
        return "sata";
    }
    if (udev_bus == "usb" || udev_bus == "scsi" || udev_bus == "nvme") {
        return udev_bus;
    }
    return disk_name.starts_with("nvme") ? "nvme" : "";
}

PhysicalDeviceInfo describe_disk(const std::filesystem::path& disk)
{
    PhysicalDeviceInfo device;
    const auto name = disk.filename().native();
    device.device_ref = "/dev/" + name;

    Properties properties;
    if (const auto number = parse_device_number(read_text_file(disk / "dev"))) {
        properties = udev_properties(*number);
    }
    device.model = encoded_property(properties, "ID_MODEL");
    if (device.model.empty()) {
        device.model = read_text_file(disk / "device" / "model");
    }
    device.serial = property(properties, "ID_SERIAL_SHORT");
    if (device.serial.empty()) {
        device.serial = read_text_file(disk / "device" / "serial");
    }
    // NVMe namespaces expose their EUI-64/NGUID as "wwid" ("eui.<hex>"); Windows
    // reports the same identifier (see nvme_wwn_from_windows_serial).
    device.wwn = read_text_file(disk / "wwid");
    if (device.wwn.empty()) {
        device.wwn = property(properties, "ID_WWN_WITH_EXTENSION");
    }
    if (device.wwn.empty()) {
        device.wwn = property(properties, "ID_WWN");
    }
    device.bus = bus_name(property(properties, "ID_BUS"), name);

    try {
        device.capacity_bytes = std::stoull(read_text_file(disk / "size")) * 512;
    } catch (const std::exception&) {
    }
    const auto rotational = read_text_file(disk / "queue" / "rotational");
    if (rotational == "1" || rotational == "0") {
        device.rotational = rotational == "1";
    }
    return device;
}

// Fallback when udev data is unavailable: /dev/disk/by-uuid and by-label link to
// the device node.
std::string find_link_name(const std::filesystem::path& directory, const std::string& source)
{
    std::error_code error;
    const auto target = std::filesystem::canonical(source, error);
    if (error) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::error_code link_error;
        if (std::filesystem::canonical(entry.path(), link_error) == target && !link_error) {
            return entry.path().filename().native();
        }
    }
    return {};
}

bool has_option(const std::string& options, std::string_view wanted)
{
    std::size_t start = 0;
    while (start <= options.size()) {
        auto end = options.find(',', start);
        if (end == std::string::npos) {
            end = options.size();
        }
        if (std::string_view(options).substr(start, end - start) == wanted) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

VolumeInfo describe_mount(const linux_mounts::Mount& mount, DeviceNumber number)
{
    const auto properties = udev_properties(number);
    VolumeInfo volume;
    volume.mount_point = mount.mount_point;

    auto type = property(properties, "ID_FS_TYPE");
    if (type.empty()) {
        type = mount.filesystem_type;
    }
    volume.filesystem_type = normalise_filesystem_type(type);

    auto raw_uuid = property(properties, "ID_FS_UUID");
    if (raw_uuid.empty()) {
        raw_uuid = find_link_name("/dev/disk/by-uuid", mount.source);
    }
    const auto id = normalise_filesystem_id(volume.filesystem_type, raw_uuid);
    volume.filesystem_uuid = id.uuid;
    volume.filesystem_uuid_short = id.uuid_short;

    volume.volume_label = encoded_property(properties, "ID_FS_LABEL");
    if (volume.volume_label.empty() && !properties.contains("ID_FS_LABEL")) {
        volume.volume_label = decode_udev(find_link_name("/dev/disk/by-label", mount.source));
    }
    volume.read_only = has_option(mount.options, "ro");

    struct statvfs information {};
    if (::statvfs(mount.mount_point.c_str(), &information) == 0) {
        volume.capacity_bytes =
            static_cast<std::uint64_t>(information.f_blocks) * information.f_frsize;
    }

    std::vector<std::filesystem::path> disks;
    collect_disks(sysfs_path(number), disks, volume.container, 0);
    for (const auto& disk : disks) {
        volume.devices.push_back(describe_disk(disk));
    }
    return volume;
}

// Filesystems without a block device (ZFS datasets, overlayfs, tmpfs): the
// kernel's filesystem id (statvfs f_fsid) is the only identifier. No devices
// are reported, so matching is at best MATCH_WEAK.
VolumeInfo describe_virtual_mount(const linux_mounts::Mount& mount)
{
    VolumeInfo volume;
    volume.mount_point = mount.mount_point;
    volume.filesystem_type = normalise_filesystem_type(mount.filesystem_type);
    volume.read_only = has_option(mount.options, "ro");
    struct statvfs information {};
    if (::statvfs(mount.mount_point.c_str(), &information) == 0) {
        volume.capacity_bytes = static_cast<std::uint64_t>(information.f_blocks) * information.f_frsize;
        char text[32];
        std::snprintf(text, sizeof(text), "fsid-%016llx",
            static_cast<unsigned long long>(information.f_fsid));
        volume.filesystem_uuid = text;
    }
    return volume;
}

bool pseudo_filesystem(const std::string& type)
{
    static const std::set<std::string> pseudo{"proc", "sysfs", "devtmpfs", "devpts", "cgroup",
        "cgroup2", "mqueue", "securityfs", "debugfs", "tracefs", "pstore", "bpf", "autofs",
        "hugetlbfs", "configfs", "fusectl", "binfmt_misc", "efivarfs", "nsfs", "rpc_pipefs",
        "selinuxfs", "ramfs", "squashfs"};
    return pseudo.contains(type);
}

// Block-device-backed mounts only. btrfs reports an anonymous major 0 in
// mountinfo, so the device number comes from the source node instead.
std::optional<DeviceNumber> block_device_of(const linux_mounts::Mount& mount)
{
    if (!mount.source.starts_with("/dev/") || mount.filesystem_type == "squashfs") {
        return std::nullopt;
    }
    return device_number_of(mount.source);
}

class LinuxMediaEnumerator final : public IMediaEnumerator {
public:
    std::expected<std::vector<VolumeInfo>, IoError> volumes() override
    {
        const auto mounts = linux_mounts::read_mountinfo();
        if (mounts.empty()) {
            return std::unexpected(IoError{IoErrorKind::Other, 0, "cannot read /proc/self/mountinfo"});
        }
        std::vector<VolumeInfo> result;
        std::set<DeviceNumber> seen;
        // A volume mounted several times (bind mounts, subvolumes) is listed once:
        // first at a mount of its filesystem root, otherwise at any of its mounts
        // (containers often see disks only through bind mounts).
        for (const bool root_only : {true, false}) {
            for (const auto& mount : mounts) {
                const auto number = block_device_of(mount);
                if (!number || (root_only && mount.root != "/") || !seen.insert(*number).second) {
                    continue;
                }
                result.push_back(describe_mount(mount, *number));
            }
        }
        // Filesystems without a block device (ZFS, overlayfs, tmpfs), once per
        // filesystem id; kernel pseudo filesystems cannot hold archives.
        std::set<std::string> seen_ids;
        for (const auto& mount : mounts) {
            if (block_device_of(mount) || mount.root != "/" || pseudo_filesystem(mount.filesystem_type)) {
                continue;
            }
            auto volume = describe_virtual_mount(mount);
            if (!volume.filesystem_uuid.empty() && seen_ids.insert(volume.filesystem_uuid).second) {
                result.push_back(std::move(volume));
            }
        }
        return result;
    }

    // A path below a bind mount of a subdirectory (mountinfo root other than
    // "/") is described by that mount; its root_subpath is then relative to the
    // bind mount, not to the filesystem root.
    std::expected<VolumeInfo, IoError> volume_for_path(const std::filesystem::path& path) override
    {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        if (error) {
            return std::unexpected(posix::error_from_errno(error.value(), "canonical"));
        }
        const auto mount = linux_mounts::mount_containing(linux_mounts::read_mountinfo(), canonical);
        if (!mount) {
            return std::unexpected(IoError{IoErrorKind::NotFound, 0, "no mount contains the path"});
        }
        const auto number = block_device_of(*mount);
        if (!number) {
            return describe_virtual_mount(*mount);
        }
        return describe_mount(*mount, *number);
    }
};

} // namespace

std::unique_ptr<IMediaEnumerator> make_media_enumerator()
{
    return std::make_unique<LinuxMediaEnumerator>();
}

} // namespace va::platform
