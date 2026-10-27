#pragma once
#include <chrono>
#include <filesystem>
#include <expected>
#include <string>
#include "common/error.h"

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path();
std::expected<std::filesystem::path, IoError> user_config_dir();
std::expected<std::filesystem::path, IoError> user_home();
bool writable_directory(const std::filesystem::path& directory);
bool network_filesystem(const std::filesystem::path& path);
bool process_is_elevated();

// `path` through a drive letter made with subst (`subst S: D:\Data`), with
// the letter replaced by the folder it stands for: "S:\Photos" becomes
// "D:\Data\Photos". Windows reports no volume for such a letter, so the user
// interfaces resolve an archive root this way before its volume is looked up,
// and the root is recorded by its real path (decision 0027). Any other path
// comes back unchanged, and on Linux every path.
std::filesystem::path resolve_subst_drive(const std::filesystem::path& path);

// `path`, when it is a volume's GUID path in the \\?\ form (Windows,
// "\\?\Volume{...}\Photos", decision 0029, point 3), with what follows its
// GUID made normal as the C++ library makes any other path the operator types
// (app::typed_path): every run of separators one '\', "." dropped and ".."
// dropping the name before it, never the volume's root folder, and its prefix
// written with '\'. Windows reads a path in that form exactly as given, while
// a root's place on its volume is told from the path made normal, so made so,
// the root read is the root identified. Any other path comes back unchanged,
// and on Linux every path.
std::filesystem::path normal_volume_guid_path(const std::filesystem::path& path);

// What keeps a folder the operator gave from being taken as an archive root
// before anything of it is read (decision 0029). `root` is a root as the user
// interfaces make it (app::typed_root).
enum class RootKind {
    Usable,        // nothing here: its volume is looked up as for any root
    NetworkShare,  // on a network share, which is no volume of this computer: on Windows a network path
                   // ("\\server\share\..."), or a drive letter that Windows reports as a network drive; on
                   // Linux a network filesystem (NFS, SMB, sshfs, ...)
    DevicePath,    // written in the \\?\ or \\.\ form (Windows only): "\\?\D:\Photos", which the volume lookup
                   // and the C++ library misread. Not a volume's GUID path in the \\?\ form
                   // ("\\?\Volume{...}\Photos"), the only name of a volume that has no drive letter or
                   // folder path, which they read as a root on its volume: it is Usable (decision 0029, point 3)
};
// Judged on Windows from the root's text and the type of its drive letter (a
// network path from its text alone); on Linux from the filesystem type that
// statfs and /proc/self/mountinfo give for it. A root whose filesystem cannot
// be told is Usable: its volume lookup then says what is wrong.
RootKind archive_root_kind(const std::filesystem::path& root);

// Whether a root the operator gave is there (decision 0029, point 7), asked
// before anything of it is read: a folder; something that is no folder; not
// found; on Windows, not found as nothing is attached as its drive letter
// (or the drive has no medium in it); or not told (access denied, say),
// which the volume lookup then reports.
enum class RootPresence { Folder, NotAFolder, Missing, NotAttached, Unknown };
RootPresence root_presence(const std::filesystem::path& root);

// Whether `path` is a folder, as the root of a registration or relocation
// must be (app::root_not_taken). On Windows asked of GetFileAttributesW in
// the \\?\ form (R-23), which takes a volume's GUID path ("\\?\Volume{...}\")
// as the volume's root folder, where std::filesystem::is_directory of the C++
// library fails; such a path written without its separator names the volume
// itself, and is asked again with it. On Linux std::filesystem::is_directory.
bool is_directory(const std::filesystem::path& path);

// Recorded in manifests and history rows (created_by).
std::string os_name();   // "windows" or "linux"
std::string host_name();

// The operating system's interface language, e.g. "zh-TW" or "en_US.UTF-8";
// empty when unknown (decision 0017). On Linux it may be a ':'-separated
// priority list taken from LANGUAGE, such as "fr:zh_TW:en", which
// app::resolve_language splits.
std::string ui_language();
// How far the system's local time is ahead of UTC at `time`, with daylight
// saving time as it was then (Taiwan: +8 h). Zero when it cannot be determined.
std::chrono::seconds utc_offset(std::chrono::sys_seconds time);
// Makes std::cout and std::cerr show UTF-8 text correctly in a Windows console
// window, whatever its code page (often a legacy one such as Big5): text for a
// console is written as UTF-16 through WriteConsoleW. The console's code page is
// not changed, so there is nothing to restore at exit. Redirected output (a pipe
// or a file) stays UTF-8 bytes. Does nothing on Linux, where UTF-8 is the norm.
void prepare_console();

} // namespace va::platform