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

// Whether `path` lies on a network or remote filesystem: by the filesystem
// type statfs reports, then by the type of the mount that contains it in
// /proc/self/mountinfo. Nothing when neither tells (statfs fails, or no mount
// contains the path).
std::optional<bool> network_filesystem_of(const std::filesystem::path& path)
{
    struct statfs information {};
    if (statfs(path.c_str(), &information) != 0) {
        return std::nullopt;
    }

    using magic = decltype(information.f_type);
    constexpr magic remote_magics[] = {
        static_cast<magic>(0x6969),     // NFS
        static_cast<magic>(0xFF534D42), // CIFS
        static_cast<magic>(0xFE534D42), // SMB2/3
        static_cast<magic>(0x517B),     // SMB
        static_cast<magic>(0x5346414F), // AFS
        static_cast<magic>(0x73757245), // Coda
        static_cast<magic>(0x564C),     // NCP
        static_cast<magic>(0x01021997), // 9P
        static_cast<magic>(0x00C36400), // Ceph
    };
    for (const auto remote : remote_magics) {
        if (information.f_type == remote) {
            return true;
        }
    }

    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return linux_mounts::on_network_filesystem(linux_mounts::read_mountinfo(), error ? path : canonical);
}

} // namespace

// Fails closed: a filesystem that cannot be identified is treated as remote, so
// SQLite is never silently placed on a network share.
bool network_filesystem(const std::filesystem::path& path)
{
    return network_filesystem_of(path).value_or(true);
}

// Decision 0029: a root on a network filesystem is refused. One whose
// filesystem cannot be told is left to its volume lookup, which says why.
RootKind archive_root_kind(const std::filesystem::path& root)
{
    return network_filesystem_of(root).value_or(false) ? RootKind::NetworkShare : RootKind::Usable;
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