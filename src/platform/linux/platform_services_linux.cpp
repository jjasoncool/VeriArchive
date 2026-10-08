#include "platform/platform_services.h"
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

bool network_filesystem(const std::filesystem::path& path)
{
    struct statfs information {};
    if (statfs(path.c_str(), &information) != 0) {
        return false;
    }

    constexpr decltype(information.f_type) nfs_magic = 0x6969;
    constexpr decltype(information.f_type) cifs_magic = 0xFF534D42;
    constexpr decltype(information.f_type) smb_magic = 0x517B;
    return information.f_type == nfs_magic || information.f_type == cifs_magic ||
           information.f_type == smb_magic;
}

bool process_is_elevated()
{
    return geteuid() == 0;
}

} // namespace va::platform