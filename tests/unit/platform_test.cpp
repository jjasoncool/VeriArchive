#include "test_support.h"

#include "core/engine/registration_attempt.h"
#include "platform/linux/mountinfo.h"
#include "platform/media_enumerator.h"
#include "platform/platform_services.h"
#include "platform/process_table.h"

#include <nlohmann/json.hpp>


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

// M2: the volume holding the temporary directory can be identified without
// elevation. Devices may legitimately be missing (virtual disks), so only the
// filesystem identity is required.
void media_enumeration_identifies_local_volume()
{
    const auto enumerator = va::platform::make_media_enumerator();
    const auto volume = enumerator->volume_for_path(std::filesystem::temp_directory_path());
    CHECK(volume.has_value());
    if (volume) {
        CHECK(!volume->mount_point.empty());
        CHECK(!volume->filesystem_type.empty());
        CHECK(!volume->filesystem_uuid.empty() || !volume->filesystem_uuid_short.empty());
        CHECK(volume->capacity_bytes > 0);
    }
    const auto all = enumerator->volumes();
    CHECK(all && !all->empty());
}

// Decision 0024, with this computer's processes: this process is found, with
// the start it had a moment ago and the program it runs; an id that no
// process has reads as ended. Judged, a plan naming this process runs (when
// this process may run a registration), and one naming its id with another
// start is a later process given the same id: the attempt has ended.
void process_table_tells_running_from_ended()
{
    const auto processes = va::platform::make_process_table();
    const auto self = processes->current();
    CHECK(self && self->id != 0 && !self->start.empty() && !self->program.empty());
    if (!self) {
        std::cerr << "  " << self.error().message << '\n';
        return;
    }
    const auto again = processes->current();
    CHECK(again && again->id == self->id && again->start == self->start);
    const auto found = processes->find(self->id);
    CHECK(found && *found && (*found)->id == self->id && (*found)->start == self->start);
    // Beyond every process id either system gives out (Windows: multiples of
    // 4 well below this; Linux: at most 2^22).
    for (const std::uint64_t unused : {std::uint64_t{0x7FFFFFF0}, std::uint64_t{0}}) {
        const auto none = processes->find(unused);
        CHECK(none && !*none);
    }

    const auto naming = [](const va::ProcessIdentity& process) {
        nlohmann::json plan{{"archive_uuid", "c0ffee00-0000-4000-8000-000000000001"}, {"revision", 1}};
        va::record_attempt(plan, {process, "this-computer"});
        return plan;
    };
    // `runs_an_operation`: whether this process runs an operation now.
    const auto judged = [&](const va::ProcessIdentity& process, bool runs_an_operation) {
        return va::judge_attempt(naming(process), *processes, "this-computer", [runs_an_operation] {
            return runs_an_operation;
        }).state;
    };
    CHECK(judged(*self, true) == va::AttemptState::Running);
    CHECK(judged(*self, false) == va::AttemptState::Ended);
    CHECK(judged({self->id, self->start + "0", self->program}, true) == va::AttemptState::Ended);
    CHECK(judged({0x7FFFFFF0, "1", "gone"}, true) == va::AttemptState::Ended);
}

} // namespace

void platform_tests()
{
    process_table_tells_running_from_ended();
    media_enumeration_identifies_local_volume();
    mountinfo_is_parsed();
    network_types_are_recognised();
    network_detection_fails_closed();
}
