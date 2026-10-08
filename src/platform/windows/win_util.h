#pragma once
// Windows helpers shared by the platform implementation files.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common/error.h"

#include <filesystem>
#include <string>

namespace va::platform::windows {

// Maps a Win32 error code to the platform-neutral error taxonomy (§B9).
IoError error_from_win32(DWORD code, std::string operation);

// Absolute paths get the \\?\ (or \\?\UNC\) prefix so they may exceed MAX_PATH (R-23).
std::wstring long_path(const std::filesystem::path& path);

// Owns a HANDLE and closes it when it goes out of scope (RAII).
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    HANDLE release() noexcept
    {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace va::platform::windows
