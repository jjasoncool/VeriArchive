#include "test_support.h"

#include "core/engine/registration_attempt.h"
#include "platform/linux/mountinfo.h"
#include "platform/media_enumerator.h"
#include "platform/platform_services.h"
#include "platform/process_table.h"
#include "platform/windows/long_path.h"
#include "platform/windows/root_kind.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>


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

// R-23: the \\?\ form in which Windows opens a path of any length names what
// the path names, also a network path and a path given in that form already,
// both of which the C++ library reads as a path on the current drive. Plain
// C++, so it is tested on every OS although only Windows uses it.
void long_paths_keep_their_drive_or_share()
{
    using va::platform::windows::long_path;
    // From the root of a drive letter: '/' is '\', "." and ".." are resolved,
    // never above the root, and a separator at the end stays.
    CHECK(long_path(LR"(C:\x)") == LR"(\\?\C:\x)");
    CHECK(long_path(L"C:/Data/./a/../b") == LR"(\\?\C:\Data\b)");
    CHECK(long_path(LR"(C:\Data\\b)") == LR"(\\?\C:\Data\b)");
    CHECK(long_path(LR"(C:\相片\2026)") == LR"(\\?\C:\相片\2026)");
    CHECK(long_path(LR"(C:\..\x)") == LR"(\\?\C:\x)");
    CHECK(long_path(LR"(C:\Data\..)") == LR"(\\?\C:\)");
    CHECK(long_path(LR"(C:\Data\a\..)") == LR"(\\?\C:\Data\)");
    CHECK(long_path(LR"(C:\)") == LR"(\\?\C:\)");
    CHECK(long_path(LR"(C:\mnt\vol\)") == LR"(\\?\C:\mnt\vol\)");
    // A network path keeps its server and share.
    CHECK(long_path(LR"(\\server\share\a\..\b)") == LR"(\\?\UNC\server\share\b)");
    CHECK(long_path(L"//server/share/dir/") == LR"(\\?\UNC\server\share\dir\)");
    CHECK(long_path(LR"(\\server\share)") == LR"(\\?\UNC\server\share)");
    CHECK(long_path(LR"(\\server\share\..\..\x)") == LR"(\\?\UNC\server\share\x)");
    // A path in the \\?\ or \\.\ form already is taken as it is, but for '/'.
    CHECK(long_path(LR"(\\?\C:\x)") == LR"(\\?\C:\x)");
    CHECK(long_path(LR"(\\?\C:\a\..\x)") == LR"(\\?\C:\a\..\x)");
    CHECK(long_path(L"//?/C:/x") == LR"(\\?\C:\x)");
    CHECK(long_path(LR"(\\?\UNC\server\share\x)") == LR"(\\?\UNC\server\share\x)");
    CHECK(long_path(LR"(\\.\C:\x)") == LR"(\\.\C:\x)");
    // Windows reads any other path as usual.
    CHECK(long_path(L"relative/a/../b") == LR"(relative\a\..\b)");
    CHECK(long_path(LR"(\Data)") == LR"(\Data)");
    CHECK(long_path(L"C:Data") == L"C:Data");
    CHECK(long_path(LR"(\\server)") == LR"(\\server)");
    CHECK(long_path(LR"(\\server\)") == LR"(\\server\)");
    CHECK(long_path(L"").empty());
}

// Decision 0029: the archive roots that Windows refuses before anything of
// them is read, told from the text and the type of the drive letter (sample
// data: K: is a local volume, Z: a drive letter mapped to a network share;
// the server and share are sample names). Plain C++, so it is tested on every
// OS although only Windows uses it.
void windows_roots_on_shares_are_told()
{
    using va::platform::RootKind;
    std::vector<std::wstring> asked;
    const auto kind = [&asked](std::wstring_view root) {
        return va::platform::windows::root_kind(root, [&asked](std::wstring_view drive) {
            asked.emplace_back(drive);
            return drive == L"Z:";
        });
    };
    // A network path is a share, told without asking about any drive.
    CHECK(kind(LR"(\\sample-server\sample-share\photos)") == RootKind::NetworkShare);
    CHECK(kind(L"//sample-server/sample-share") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\sample-server)") == RootKind::NetworkShare);
    CHECK(asked.empty());
    // A drive letter mapped to a share, written in either case.
    CHECK(kind(LR"(Z:\SAMPLE\photos)") == RootKind::NetworkShare);
    CHECK(kind(L"z:/SAMPLE") == RootKind::NetworkShare);
    CHECK(kind(LR"(Z:\)") == RootKind::NetworkShare);
    CHECK((asked == std::vector<std::wstring>{L"Z:", L"Z:", L"Z:"}));
    // A local volume's letter, and a path that names no drive, are usable.
    CHECK(kind(LR"(K:\SAMPLE\photos)") == RootKind::Usable);
    CHECK(kind(LR"(K:\)") == RootKind::Usable);
    CHECK(kind(L"SAMPLE") == RootKind::Usable);
    CHECK(kind(LR"(\SAMPLE)") == RootKind::Usable);
    CHECK(kind(L"K:SAMPLE") == RootKind::Usable);
    CHECK(kind(L"") == RootKind::Usable);
    // The \\?\ and \\.\ forms: a share behind them is a share, as in any
    // form; anything else is refused for its form.
    CHECK(kind(LR"(\\?\UNC\sample-server\sample-share\photos)") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\?\unc\sample-server\sample-share)") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\.\UNC\sample-server\sample-share)") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\?\Z:\SAMPLE)") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\?\K:\SAMPLE\photos)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\K:\)") == RootKind::DevicePath);
    CHECK(kind(L"//?/K:/SAMPLE") == RootKind::DevicePath);
    CHECK(kind(LR"(\\.\K:\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\UNCLE\SAMPLE)") == RootKind::DevicePath); // a name that only starts with UNC
}

// Decision 0029: on Linux a root on a network filesystem, told by the type of
// the mount that contains it (sample mountinfo: NFS, SMB and sshfs mounts, a
// local volume mounted in a folder of the NFS share, and a local volume
// through ntfs-3g).
// Plain C++, so it is tested on every OS although only Linux uses it.
void linux_roots_on_network_filesystems_are_told()
{
    using va::platform::linux_mounts::on_network_filesystem;
    const auto mounts = va::platform::linux_mounts::parse_mountinfo(
        "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
        "41 22 0:50 / /mnt/sample-nas rw,relatime shared:20 - nfs4 sample-server:/exports rw,vers=4.2\n"
        "42 22 0:51 / /media/sample-share rw,relatime shared:21 - cifs //sample-server/share rw\n"
        "43 22 0:52 / /home/sample/remote rw,nosuid,nodev - fuse.sshfs sample@sample-server:/data rw\n"
        "44 41 8:17 / /mnt/sample-nas/usb rw,relatime shared:23 - ext4 /dev/sdb1 rw\n"
        "45 22 8:33 / /media/sample/SAMPLE rw,nosuid - fuseblk /dev/sdc1 rw\n");
    CHECK(mounts.size() == 6);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas/photos") == true);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas") == true);
    CHECK(on_network_filesystem(mounts, "/media/sample-share/2026") == true);
    CHECK(on_network_filesystem(mounts, "/home/sample/remote/photos") == true);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas/usb/photos") == false);
    CHECK(on_network_filesystem(mounts, "/media/sample/SAMPLE/photos") == false);
    CHECK(on_network_filesystem(mounts, "/home/sample/photos") == false);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas-2") == false);
    CHECK(!on_network_filesystem({}, "/home/sample"));
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
    long_paths_keep_their_drive_or_share();
    windows_roots_on_shares_are_told();
    linux_roots_on_network_filesystems_are_told();
    network_types_are_recognised();
    network_detection_fails_closed();
}
