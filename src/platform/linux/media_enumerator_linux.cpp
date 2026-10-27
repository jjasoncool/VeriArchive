// Linux media enumeration with unprivileged reads only (R-09):
//  - /proc/self/mountinfo for mount points (mount boundaries, R-30)
//  - the udev property database (/run/udev/data/b<major>:<minor>) for
//    filesystem type, UUID and label, and for device model/serial/WWN
//  - sysfs (/sys/dev/block/...) to walk from a volume to its physical disks,
//    through partitions, device-mapper (LUKS, LVM) and md RAID.
// The udev database files are read directly (the same data libudev returns).
// Containers and minimal systems have no udev database; decision 0021 then
// takes the kernel filesystem id (statfs) and, for SATA devices, the SCSI
// serial page (sysfs).
#include "platform/media_enumerator.h"
#include "platform/linux/mountinfo.h"
#include "platform/linux/posix_util.h"
#include "platform/linux/vpd.h"

#include "core/identity/filesystem_id.h"
#include "core/identity/identity_text.h"

#include <algorithm>
#include <compare> // IWYU pragma: keep (std::strong_ordering of the defaulted operator<=>)
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
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

// The bytes of a file, unchanged; empty when it cannot be read.
std::string read_binary_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::filesystem::path& path)
{
    return trim(read_binary_file(path));
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
        // increment(error), never operator++, which throws on a read error:
        // no exception leaves the platform layer (CLAUDE.md rule 6).
        for (std::filesystem::directory_iterator slave(slaves, error), end; !error && slave != end;
             slave.increment(error)) {
            std::error_code canonical_error;
            collect_disks(std::filesystem::canonical(slave->path(), canonical_error), disks, container,
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
        device.serial = read_text_file(disk / "device" / "serial"); // e.g. NVMe
    }
    // Decision 0021: without the udev database, a SATA device behind the Linux
    // ATA layer still reports its serial in the SCSI Unit Serial Number page,
    // which sysfs shows raw; it is the serial udev reports for that device.
    // Other SCSI devices (SAS, virtio-scsi, USB bridges) are left without one:
    // udev takes theirs from another source (page 0x83, the USB serial), so a
    // page 0x80 serial would not match the one recorded where udev runs.
    if (device.serial.empty() && properties.empty() &&
        linux_vpd::is_ata_vendor(read_text_file(disk / "device" / "vendor"))) {
        device.serial = linux_vpd::unit_serial(read_binary_file(disk / "device" / "vpd_pg80"));
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
    // increment(error), never operator++, which throws on a read error.
    for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code link_error;
        if (std::filesystem::canonical(entry->path(), link_error) == target && !link_error) {
            return entry->path().filename().native();
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

struct FilesystemStatus {
    std::uint64_t capacity_bytes = 0;
    std::uint64_t kernel_id = 0; // f_fsid, see combine_kernel_filesystem_id
};

// The volume size and the kernel filesystem id of the filesystem mounted at
// `mount_point`. statfs() rather than statvfs(): statvfs() has the C library
// narrow f_fsid to one unsigned long, and only 64-bit glibc keeps both of its
// words; musl (Alpine containers) and 32-bit glibc keep the first one only.
std::optional<FilesystemStatus> filesystem_status(const std::filesystem::path& mount_point)
{
    // `struct statfs` names the type: a function of the same name exists.
    struct statfs information {};
    if (::statfs(mount_point.c_str(), &information) != 0) {
        return std::nullopt;
    }
    // The size the way statvfs() gives it: blocks of f_frsize bytes, or of
    // f_bsize bytes where the kernel leaves f_frsize zero.
    const auto block_size = information.f_frsize != 0 ? information.f_frsize : information.f_bsize;
    FilesystemStatus status;
    status.capacity_bytes =
        static_cast<std::uint64_t>(information.f_blocks) * static_cast<std::uint64_t>(block_size);
    // glibc and musl both name the two words of f_fsid __val.
    status.kernel_id =
        combine_kernel_filesystem_id(information.f_fsid.__val[0], information.f_fsid.__val[1]);
    return status;
}

// The first sector of the block device `device_node`, as blkid reads it for
// udev; empty where this process may not read the node (R-09: libblkid only
// on device nodes the process can already read) or the read fails.
std::string first_sector(const std::string& device_node)
{
    if (!device_node.starts_with("/dev/")) {
        return {};
    }
    const posix::UniqueFd descriptor(::open(device_node.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!descriptor.valid()) {
        return {};
    }
    std::string sector(512, '\0');
    const auto count = ::pread(descriptor.get(), sector.data(), sector.size(), 0);
    if (count <= 0) {
        return {};
    }
    sector.resize(static_cast<std::size_t>(count));
    return sector;
}

VolumeInfo describe_mount(const linux_mounts::Mount& mount, DeviceNumber number)
{
    const auto properties = udev_properties(number);
    VolumeInfo volume;
    volume.mount_point = mount.mount_point;

    auto type = property(properties, "ID_FS_TYPE");
    if (type.empty()) {
        // Without the udev database the mount's type stands in, but that of a
        // FUSE mount ("fuseblk", as ntfs-3g mounts) names the program, never
        // the format (decision 0006): the format is then read from the
        // device's first sector, as blkid reads it for udev, where this
        // process may read the device, and is left unknown otherwise.
        type = mount.filesystem_type;
        if (is_fuse_mount_type(normalise_filesystem_type(type))) {
            type = filesystem_format_from_boot_sector(first_sector(mount.source));
        }
    }
    volume.filesystem_type = normalise_filesystem_type(type);
    // Decision 0006: a FUSE program that reads the format instead of the
    // kernel's driver (ntfs-3g instead of ntfs3) is noted in the report of a
    // verification, never in identity or the result.
    volume.driver = fuse_driver(volume.filesystem_type, mount.filesystem_type);

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

    if (const auto status = filesystem_status(mount.mount_point)) {
        volume.capacity_bytes = status->capacity_bytes;
        // Decision 0021: neither udev nor /dev/disk/by-uuid gave the UUID (a
        // container, a minimal system). For ext2/3/4 and btrfs the kernel's
        // filesystem id is computed from the UUID, so it names the same
        // filesystem on every machine and stands in for it (unless it is zero).
        if (raw_uuid.empty()) {
            if (auto kernel_id = kernel_filesystem_id_for(volume.filesystem_type, status->kernel_id)) {
                volume.filesystem_uuid = std::move(*kernel_id);
            }
        }
    }

    std::vector<std::filesystem::path> disks;
    collect_disks(sysfs_path(number), disks, volume.container, 0);
    for (const auto& disk : disks) {
        volume.devices.push_back(describe_disk(disk));
    }
    // Decision 0035: udev passes a label or a model on as raw bytes (its
    // "_ENC" escapes), and sysfs shows a device's model as the device reports
    // it: neither need be text.
    return with_valid_identity_text(std::move(volume));
}

// Filesystems without a block device (ZFS datasets, overlayfs, tmpfs): the
// kernel's filesystem id (statfs f_fsid) is the only identifier. No devices
// are reported, so matching is at best MATCH_WEAK. A container that does not
// show a volume's device node also lands here, whatever the filesystem; for
// xfs and others the id then follows the device number (decision 0021).
// FUSE and NFS report zero, so all of them read as one id. The format is the
// one the mount's type names, and unknown (empty) for a FUSE type, which
// names a program, as a mount of ntfs-3g whose device node the container
// hides is "fuseblk" (decision 0006).
VolumeInfo describe_virtual_mount(const linux_mounts::Mount& mount)
{
    VolumeInfo volume;
    volume.mount_point = mount.mount_point;
    volume.filesystem_type = filesystem_format_of_mount_type(mount.filesystem_type);
    volume.driver = fuse_driver(volume.filesystem_type, mount.filesystem_type); // decision 0006
    volume.read_only = has_option(mount.options, "ro");
    if (const auto status = filesystem_status(mount.mount_point)) {
        volume.capacity_bytes = status->capacity_bytes;
        volume.filesystem_uuid = kernel_filesystem_id(status->kernel_id);
    }
    return with_valid_identity_text(std::move(volume)); // decision 0035
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
