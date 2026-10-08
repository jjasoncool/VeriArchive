#include "test_support.h"

#include "platform/linux/mountinfo.h"
#include "platform/platform_services.h"

#include <chrono>

namespace {

// The parser is plain C++, so it is tested on every OS even though only the
// Linux build uses it.
void mountinfo_is_parsed()
{
    using va::platform::linux_mounts::mount_containing;
    using va::platform::linux_mounts::parse_mountinfo;

    const auto mounts = parse_mountinfo(
        "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
        "40 22 8:17 / /media/user/My\\040Disk rw,nosuid master:2 - fuseblk /dev/sdb1 rw\n"
        "41 22 0:50 /exports /mnt/share rw - nfs4 server:/exports rw\n"
        "garbage line without separator\n");

    CHECK(mounts.size() == 3);
    if (mounts.size() != 3) {
        return;
    }
    CHECK(mounts[0].filesystem_type == "ext4" && mounts[0].source == "/dev/sda2");
    CHECK(mounts[1].mount_point == std::filesystem::path("/media/user/My Disk"));
    CHECK(mounts[1].major == 8 && mounts[1].minor == 17);
    CHECK(mounts[2].root == "/exports");

    const auto disk = mount_containing(mounts, "/media/user/My Disk/Backups/2026");
    CHECK(disk && disk->mount_id == 40);
    const auto root = mount_containing(mounts, "/media/user/My Diskette");
    CHECK(root && root->mount_id == 22);
}

void network_types_are_recognised()
{
    using va::platform::linux_mounts::is_network_filesystem_type;
    CHECK(is_network_filesystem_type("nfs4"));
    CHECK(is_network_filesystem_type("cifs"));
    CHECK(is_network_filesystem_type("fuse.sshfs"));
    CHECK(!is_network_filesystem_type("fuseblk"));     // ntfs-3g on a local disk
    CHECK(!is_network_filesystem_type("fuse.portal")); // unknown FUSE is not assumed remote here
    CHECK(!is_network_filesystem_type("ext4"));
}

void network_detection_fails_closed()
{
    const auto local = std::filesystem::temp_directory_path();
    CHECK(!va::platform::network_filesystem(local));

    // A location whose volume cannot be identified counts as remote.
    std::filesystem::path unidentifiable = "/nonexistent-veriarchive-volume/x";
    if (std::filesystem::path::preferred_separator == '\\') {
        for (char letter = 'Q'; letter <= 'Z'; ++letter) {
            const std::filesystem::path drive = std::string(1, letter) + ":\\";
            if (!std::filesystem::exists(drive)) {
                unidentifiable = drive / "x";
                break;
            }
        }
    }
    CHECK(va::platform::network_filesystem(unidentifiable));
}

} // namespace

void platform_tests()
{
    mountinfo_is_parsed();
    network_types_are_recognised();
    network_detection_fails_closed();
}
