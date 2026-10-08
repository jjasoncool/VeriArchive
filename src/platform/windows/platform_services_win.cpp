#include "platform/platform_services.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path()
{
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::unexpected(IoError{IoErrorKind::Other,
                static_cast<int>(GetLastError()), "GetModuleFileNameW failed"});
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::expected<std::filesystem::path, IoError> user_home()
{
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) {
        return std::filesystem::path(profile);
    }
    return std::unexpected(IoError{IoErrorKind::NotFound, 0, "USERPROFILE is unavailable"});
}

std::expected<std::filesystem::path, IoError> user_config_dir()
{
    if (const wchar_t* app_data = _wgetenv(L"APPDATA")) {
        return std::filesystem::path(app_data) / L"VeriArchive";
    }
    auto home = user_home();
    if (!home) {
        return std::unexpected(home.error());
    }
    return *home / L"AppData" / L"Roaming" / L"VeriArchive";
}

bool writable_directory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    // CREATE_NEW never truncates a pre-existing file, even with concurrent starts.
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        const auto name = L".veriarchive-probe-" + std::to_wstring(GetCurrentProcessId()) +
                          L"-" + std::to_wstring(GetTickCount64()) +
                          L"-" + std::to_wstring(attempt);
        const auto probe = directory / name;
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) {
                continue;
            }
            return false;
        }
        DWORD written = 0;
        const char byte = 0;
        const bool writable = WriteFile(file, &byte, 1, &written, nullptr) &&
                              written == 1 && FlushFileBuffers(file);
        CloseHandle(file);
        return writable;
    }
    return false;
}

bool network_filesystem(const std::filesystem::path& path)
{
    const auto root = path.root_path();
    if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
        return true;
    }

    wchar_t remote_name[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const auto drive = root.native().substr(0, 2);
    return WNetGetConnectionW(drive.c_str(), remote_name, &length) == NO_ERROR;
}

bool process_is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(
                              token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                          elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

} // namespace va::platform