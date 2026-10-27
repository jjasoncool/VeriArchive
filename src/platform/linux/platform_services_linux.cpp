#include "platform/platform_services.h"
#include "platform/linux/mountinfo.h"
#include <cstdlib>
#include <ctime>
#include <optional>
#include <sys/statfs.h>
#include <unistd.h>
#include <vector>

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path()
{
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::unexpected(IoError{IoErrorKind::Other, errno, "readlink failed"});
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::expected<std::filesystem::path, IoError> user_home()
{
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home);
    }
    return std::unexpected(IoError{IoErrorKind::NotFound, 0, "HOME is unavailable"});
}

std::expected<std::filesystem::path, IoError> user_config_dir()
{
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(config_home) / "veriarchive";
    }
    auto home = user_home();
    if (!home) {
        return std::unexpected(home.error());
    }
    return *home / ".config" / "veriarchive";
}

bool writable_directory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    auto pattern = (directory / ".veriarchive-probe-XXXXXX").native();
    const int descriptor = mkstemp(pattern.data());
    if (descriptor < 0) {
        return false;
    }
    const char byte = 0;
    const bool writable = write(descriptor, &byte, 1) == 1 && fsync(descriptor) == 0;
    close(descriptor);
    unlink(pattern.c_str());
    return writable;
}

namespace {

// The type of the filesystem that holds `path`, as statfs reports it;
// nothing when statfs fails.
std::optional<linux_mounts::FilesystemMagic> statfs_type(const std::filesystem::path& path)
{
    struct statfs information {};
    if (statfs(path.c_str(), &information) != 0) {
        return std::nullopt;
    }
    return static_cast<linux_mounts::FilesystemMagic>(information.f_type);
}

// `path` made canonical, or as it is where it cannot be made so (a path that
// does not exist).
std::filesystem::path canonical_or_as_is(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return error ? path : canonical;
}

} // namespace

// The two checks below only read (statfs, the canonical path, mountinfo);
// linux_mounts judges what that tells, in plain C++ tested on every OS.

// Fails closed: a filesystem that cannot be identified is treated as remote, so
// SQLite is never silently placed on a network share.
bool network_filesystem(const std::filesystem::path& path)
{
    const auto type = statfs_type(path);
    return linux_mounts::repository_on_network(type, linux_mounts::read_mountinfo(), canonical_or_as_is(path));
}

// Decision 0029: a root on a network filesystem is refused. One whose
// filesystem cannot be told is left to its volume lookup, which says why.
RootKind archive_root_kind(const std::filesystem::path& root)
{
    const auto type = statfs_type(root);
    return linux_mounts::archive_root_on_network(type, linux_mounts::read_mountinfo(), canonical_or_as_is(root))
               ? RootKind::NetworkShare
               : RootKind::Usable;
}

// Linux has no drive letters: a root is a folder, something else, missing,
// or not told (access denied, say), which the volume lookup then reports.
RootPresence root_presence(const std::filesystem::path& root)
{
    std::error_code error;
    const auto status = std::filesystem::status(root, error);
    switch (status.type()) {
    case std::filesystem::file_type::directory:
        return RootPresence::Folder;
    case std::filesystem::file_type::not_found:
        return RootPresence::Missing;
    case std::filesystem::file_type::none:
    case std::filesystem::file_type::unknown:
        return RootPresence::Unknown;
    default:
        return error ? RootPresence::Unknown : RootPresence::NotAFolder;
    }
}

bool is_directory(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

bool process_is_elevated()
{
    return geteuid() == 0;
}

// Linux has no drive letters.
std::filesystem::path resolve_subst_drive(const std::filesystem::path& path)
{
    return path;
}

// Nor GUID paths of volumes: a backslash is an ordinary character (R-40).
std::filesystem::path normal_volume_guid_path(const std::filesystem::path& path)
{
    return path;
}

std::optional<std::filesystem::path> final_path(const std::filesystem::path& path)
{
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        return std::nullopt;
    }
    return canonical;
}

std::string os_name()
{
    return "linux";
}

std::string host_name()
{
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1) != 0) {
        return {};
    }
    return name;
}

std::chrono::seconds utc_offset(std::chrono::sys_seconds time)
{
    // localtime_r follows TZ and /etc/localtime; tm_gmtoff is the offset east
    // of UTC at that moment, daylight saving time included.
    const auto seconds = static_cast<std::time_t>(time.time_since_epoch().count());
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return std::chrono::seconds{0};
    }
    return std::chrono::seconds(local.tm_gmtoff);
}

std::string ui_language()
{
    // gettext's rules: the locale is LC_ALL, then LC_MESSAGES, then LANG, and "C"
    // when none is set (an empty variable counts as unset). LANGUAGE, a
    // ':'-separated priority list such as "fr:zh_TW:en", overrides the locale,
    // except when the locale is C or POSIX: `LC_ALL=C command` is the usual way to
    // ask for untranslated (English) text, whatever LANGUAGE says.
    std::string locale = "C";
    for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            locale = value;
            break;
        }
    }
    if (locale == "C" || locale == "POSIX") {
        return locale;
    }
    if (const char* language = std::getenv("LANGUAGE"); language && *language) {
        return language; // may be a list; app::resolve_language takes its first known entry
    }
    return locale;
}

void prepare_console() {}

} // namespace va::platform