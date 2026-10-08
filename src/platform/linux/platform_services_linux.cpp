#include "platform/platform_services.h"
#include "platform/linux/mountinfo.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
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

// Fails closed: a filesystem that cannot be identified is treated as remote, so
// SQLite is never silently placed on a network share.
bool network_filesystem(const std::filesystem::path& path)
{
    struct statfs information {};
    if (statfs(path.c_str(), &information) != 0) {
        return true;
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
    const auto mount =
        linux_mounts::mount_containing(linux_mounts::read_mountinfo(), error ? path : canonical);
    return !mount || linux_mounts::is_network_filesystem_type(mount->filesystem_type);
}

bool process_is_elevated()
{
    return geteuid() == 0;
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

} // namespace va::platform