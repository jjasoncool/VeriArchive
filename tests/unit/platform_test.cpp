#include "test_support.h"

#include "app/typed_path.h"
#include "common/utf8.h"
#include "core/engine/registration_attempt.h"
#include "platform/linux/mountinfo.h"
#include "platform/media_enumerator.h"
#include "platform/platform_services.h"
#include "platform/process_table.h"
#include "platform/windows/long_path.h"
#include "platform/windows/root_kind.h"
#include "platform/windows/volume_list.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
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
    CHECK(kind(LR"(\\?\UNCLE\SAMPLE)") == RootKind::DevicePath); // a name that only starts with UNC
}

// Decision 0029, point 3: a volume's GUID path in the \\?\ form, as mountvol
// lists it, the only name of a volume that has no drive letter or folder
// path, is taken, also for a folder below it, "Volume" and the GUID in any
// case, and with either separator; nothing is asked about a drive for it.
// The \\.\ form of it, a GUID cut short or longer, and a name that only
// starts like one stay refused for their form, as the \\?\ form of a drive
// letter does. (Sample GUIDs.) Plain C++, so it is tested on every OS
// although only Windows uses it.
void windows_volume_guid_paths_are_taken()
{
    using va::platform::RootKind;
    std::vector<std::wstring> asked;
    const auto kind = [&asked](std::wstring_view root) {
        return va::platform::windows::root_kind(root, [&asked](std::wstring_view drive) {
            asked.emplace_back(drive);
            return drive == L"Z:";
        });
    };
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)") == RootKind::Usable);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE\photos)") == RootKind::Usable);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001})") == RootKind::Usable);
    CHECK(kind(LR"(\\?\volume{C0FFEE00-0000-4000-8000-0000000000AB}\SAMPLE)") == RootKind::Usable);
    CHECK(kind(L"//?/Volume{c0ffee00-0000-4000-8000-000000000001}/SAMPLE") == RootKind::Usable);
    CHECK(asked.empty());
    CHECK(kind(LR"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-00000000001}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-0000000000012}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00x0000-4000-8000-000000000001}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-00000000000g}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volumes{c0ffee00-0000-4000-8000-000000000001}\SAMPLE)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001)") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume{})") == RootKind::DevicePath);
    CHECK(kind(LR"(\\?\Volume)") == RootKind::DevicePath);
    // A share behind the form stays a share, and so does a network path whose
    // server is named like a volume.
    CHECK(kind(LR"(\\?\UNC\Volume{c0ffee00-0000-4000-8000-000000000001}\share)") == RootKind::NetworkShare);
    CHECK(kind(LR"(\\Volume{c0ffee00-0000-4000-8000-000000000001}\share)") == RootKind::NetworkShare);
}

// Decision 0029, point 3: every volume that Windows names is listed, one with
// a drive letter or folder path under the first of them, one with neither
// under its GUID path, its only name, so that verification by archive uuid
// finds an archive there too. (Sample names.) Plain C++, so it is tested on
// every OS although only Windows uses it.
void volumes_without_a_mount_point_are_listed_by_their_guid_path()
{
    using va::platform::windows::volumes_to_list;
    const std::wstring lettered = LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)";
    const std::wstring folder = LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000002}\)";
    const std::wstring unnamed = LR"(\\?\Volume{c0ffee00-0000-4000-8000-000000000003}\)";
    const auto listed = volumes_to_list({{lettered, {LR"(K:\)", LR"(C:\mnt\SAMPLE\)"}},
        {folder, {LR"(C:\mnt\SAMPLE-2\)"}}, {unnamed, {}}});
    using Listed = std::pair<std::wstring, std::wstring>;
    CHECK((listed == std::vector<Listed>{{lettered, LR"(K:\)"}, {folder, LR"(C:\mnt\SAMPLE-2\)"}, {unnamed, unnamed}}));
    CHECK(volumes_to_list({}).empty());
}

// Decision 0029, point 3, with this computer's own volumes: the volume that
// holds the temporary folder, named by its GUID path, which Windows gives
// for it (a folder below it, its root with or without its separator), is a
// folder, where the C++ library cannot tell it; a file and a missing folder
// below it are not. A folder through the volume's own mount point is one too.
// Only reads. (Sample names, in a folder of their own in the temporary folder.)
void volume_guid_paths_name_folders()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder = std::filesystem::temp_directory_path() / ("va-SAMPLE-guid-" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "SAMPLE.txt") << "sample data\n";
    CHECK(va::platform::is_directory(folder));
    CHECK(!va::platform::is_directory(folder / "SAMPLE.txt"));
    CHECK(!va::platform::is_directory(folder / "SAMPLE-missing"));
    const auto volume = va::platform::make_media_enumerator()->volume_for_path(folder);
    CHECK(volume.has_value());
    if (std::filesystem::path::preferred_separator == '\\' && volume && !volume->volume_guid.empty()) {
        // "C:\" and the folder below it, as "\\?\Volume{...}\" and the same folder below that.
        const auto below = va::path_to_utf8(folder.lexically_relative(volume->mount_point));
        const auto guid_root = R"(\\?\Volume)" + volume->volume_guid + R"(\)";
        CHECK(va::app::root_refusal(va::path_from_utf8(guid_root + below)) == std::nullopt);
        // Taken as the root of a registration or relocation: the volume's root
        // folder, as typed with its separator or without, and the folder below it.
        CHECK(va::app::root_not_taken(va::app::typed_root(guid_root), guid_root) == std::nullopt);
        CHECK(va::app::root_not_taken(va::app::typed_root(guid_root.substr(0, guid_root.size() - 1)), "") ==
              std::nullopt);
        CHECK(va::app::root_not_taken(va::app::typed_root(guid_root + below), "") == std::nullopt);
        CHECK(va::platform::is_directory(va::path_from_utf8(guid_root)));
        CHECK(va::platform::is_directory(va::path_from_utf8(guid_root.substr(0, guid_root.size() - 1))));
        CHECK(va::platform::is_directory(va::path_from_utf8(guid_root + below)));
        CHECK(!va::platform::is_directory(va::path_from_utf8(guid_root + below + R"(\SAMPLE.txt)")));
        CHECK(!va::platform::is_directory(va::path_from_utf8(guid_root + below + R"(\SAMPLE-missing)")));
        // The volume itself, by that path.
        const auto through_guid = va::platform::make_media_enumerator()->volume_for_path(
            va::path_from_utf8(guid_root + below));
        CHECK(through_guid && through_guid->filesystem_uuid == volume->filesystem_uuid);
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

// Decision 0029: on Linux a root on a network filesystem, told by the type of
// the mount that contains it (sample mountinfo: NFS, SMB and sshfs mounts, a
// local volume mounted in a folder of the NFS share, a local volume through
// ntfs-3g, and the FUSE mounts through which GNOME (GVFS) and KDE (kio-fuse)
// give the shares they opened, below the user's runtime folder, a tmpfs).
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
        "45 22 8:33 / /media/sample/SAMPLE rw,nosuid - fuseblk /dev/sdc1 rw\n"
        "46 22 0:40 / /run/user/1000 rw,nosuid,nodev,relatime shared:30 - tmpfs tmpfs rw,mode=700,uid=1000\n"
        "47 46 0:53 / /run/user/1000/gvfs rw,nosuid,nodev,relatime shared:31 - fuse.gvfsd-fuse gvfsd-fuse "
        "rw,user_id=1000,group_id=1000\n"
        "48 46 0:54 / /run/user/1000/kio-fuse-SAMPLE rw,nosuid,nodev,relatime shared:32 - fuse.kio-fuse "
        "kio-fuse rw,user_id=1000,group_id=1000\n");
    CHECK(mounts.size() == 9);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas/photos") == true);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas") == true);
    CHECK(on_network_filesystem(mounts, "/media/sample-share/2026") == true);
    CHECK(on_network_filesystem(mounts, "/home/sample/remote/photos") == true);
    CHECK(on_network_filesystem(mounts, "/run/user/1000/gvfs/smb-share:server=sample-server,share=photos/2026") ==
          true);
    CHECK(on_network_filesystem(mounts, "/run/user/1000/gvfs/sftp:host=sample-server/home/sample/photos") == true);
    CHECK(on_network_filesystem(mounts, "/run/user/1000/kio-fuse-SAMPLE/smb/sample-server/photos") == true);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas/usb/photos") == false);
    CHECK(on_network_filesystem(mounts, "/media/sample/SAMPLE/photos") == false);
    CHECK(on_network_filesystem(mounts, "/home/sample/photos") == false);
    CHECK(on_network_filesystem(mounts, "/mnt/sample-nas-2") == false);
    CHECK(on_network_filesystem(mounts, "/run/user/1000/SAMPLE") == false);
    CHECK(!on_network_filesystem({}, "/home/sample"));
}

// Decision 0029 and R-83 step 4 on Linux: whether a path lies on a network
// filesystem, told by the type that statfs reports (sample magic numbers),
// then by the mount that contains it (sample mountinfo: the local root, an
// sshfs mount, a local volume through ntfs-3g). Where neither tells, statfs
// having failed or no mount containing the path, the repository's check fails
// closed and an archive root is not refused. Plain C++, so it is tested on
// every OS although only Linux uses it.
void linux_network_filesystems_are_told_by_statfs_and_mounts()
{
    using va::platform::linux_mounts::archive_root_on_network;
    using va::platform::linux_mounts::FilesystemMagic;
    using va::platform::linux_mounts::repository_on_network;
    constexpr FilesystemMagic ext4 = 0xEF53, fuse = 0x65735546;
    // NFS, CIFS, SMB2/3, SMB, AFS, Coda, NCP, 9P and Ceph.
    constexpr FilesystemMagic network[] = {
        0x6969, 0xFF534D42, 0xFE534D42, 0x517B, 0x5346414F, 0x73757245, 0x564C, 0x01021997, 0x00C36400};
    const auto mounts = va::platform::linux_mounts::parse_mountinfo(
        "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
        "43 22 0:52 / /home/sample/remote rw,nosuid,nodev - fuse.sshfs sample@sample-server:/data rw\n"
        "45 22 8:33 / /media/sample/SAMPLE rw,nosuid - fuseblk /dev/sdc1 rw\n");
    CHECK(mounts.size() == 3);
    // A network filesystem by the type statfs reports, whatever the mounts
    // say: where they show the local root there, and where none could be read.
    for (const auto type : network) {
        CHECK(archive_root_on_network(type, mounts, "/mnt/sample-nas/photos"));
        CHECK(repository_on_network(type, mounts, "/mnt/sample-nas/photos"));
        CHECK(archive_root_on_network(type, {}, "/mnt/sample-nas/photos"));
        CHECK(repository_on_network(type, {}, "/mnt/sample-nas/photos"));
    }
    // sshfs by its mount: statfs reports FUSE, as it does for ntfs-3g on a
    // local disk, and ext4 for a folder on the local root.
    CHECK(archive_root_on_network(fuse, mounts, "/home/sample/remote/photos"));
    CHECK(repository_on_network(fuse, mounts, "/home/sample/remote/photos"));
    CHECK(!archive_root_on_network(fuse, mounts, "/media/sample/SAMPLE/photos"));
    CHECK(!repository_on_network(fuse, mounts, "/media/sample/SAMPLE/photos"));
    CHECK(!archive_root_on_network(ext4, mounts, "/home/sample/photos"));
    CHECK(!repository_on_network(ext4, mounts, "/home/sample/photos"));
    // Neither tells: statfs failed (also below the sshfs mount), or no mount
    // contains the path.
    for (const auto* path : {"/home/sample/photos", "/home/sample/remote/photos"}) {
        CHECK(!archive_root_on_network(std::nullopt, mounts, path));
        CHECK(repository_on_network(std::nullopt, mounts, path));
    }
    CHECK(!archive_root_on_network(ext4, {}, "/home/sample/photos"));
    CHECK(repository_on_network(ext4, {}, "/home/sample/photos"));
}

void network_types_are_recognised()
{
    using va::platform::linux_mounts::is_network_filesystem_type;
    CHECK(is_network_filesystem_type("nfs4"));
    CHECK(is_network_filesystem_type("cifs"));
    CHECK(is_network_filesystem_type("fuse.sshfs"));
    CHECK(is_network_filesystem_type("fuse.gvfsd-fuse")); // GNOME's shares (GVFS)
    CHECK(is_network_filesystem_type("fuse.kio-fuse"));   // KDE's shares
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
    windows_volume_guid_paths_are_taken();
    volumes_without_a_mount_point_are_listed_by_their_guid_path();
    volume_guid_paths_name_folders();
    linux_roots_on_network_filesystems_are_told();
    linux_network_filesystems_are_told_by_statfs_and_mounts();
    network_types_are_recognised();
    network_detection_fails_closed();
}
