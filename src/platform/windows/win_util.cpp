#include "platform/windows/win_util.h"

#include <string_view>

namespace va::platform::windows {
namespace {

// What went wrong, in plain words, for the errors people meet (decision 0017,
// amended 2026-10-27); nothing for others, which keep Windows' number alone.
std::string_view plain_words(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return "not found";
    case ERROR_ACCESS_DENIED:
        return "access denied";
    case ERROR_INVALID_DRIVE:
        return "no such drive";
    case ERROR_WRITE_PROTECT:
        return "the media is write-protected";
    case ERROR_NOT_READY:
        return "the device is not ready";
    case ERROR_CRC:
    case ERROR_SECTOR_NOT_FOUND:
    case ERROR_READ_FAULT:
    case ERROR_WRITE_FAULT:
    case ERROR_GEN_FAILURE:
    case ERROR_IO_DEVICE:
    case ERROR_SEEK:
    case ERROR_BAD_UNIT:
    case ERROR_DEVICE_HARDWARE_ERROR:
        return "the device reported an error";
    case ERROR_DISK_CORRUPT:
    case ERROR_FILE_CORRUPT:
        return "damaged and unreadable";
    case ERROR_SHARING_VIOLATION:
        return "in use by another program";
    case ERROR_LOCK_VIOLATION:
        return "locked by another program";
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL:
        return "the volume is full";
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return "network path not found";
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return "already exists";
    case ERROR_INVALID_NAME:
        return "the name is not valid";
    case ERROR_FILENAME_EXCED_RANGE:
        return "the name is too long";
    case ERROR_DIRECTORY:
        return "not a folder";
    case ERROR_DIR_NOT_EMPTY:
        return "the folder is not empty";
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED:
    case ERROR_NO_SUCH_DEVICE:
        return "the device is not attached";
    case ERROR_PRIVILEGE_NOT_HELD:
        return "not permitted for this user";
    case ERROR_SEM_TIMEOUT:
    case WAIT_TIMEOUT:
        return "no answer in time";
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return "not supported";
    case ERROR_OPERATION_ABORTED:
        return "cancelled";
    default:
        return {};
    }
}

} // namespace

IoError error_from_win32(DWORD code, std::string operation)
{
    IoErrorKind kind = IoErrorKind::Other;
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
        kind = IoErrorKind::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
        kind = IoErrorKind::PermissionDenied;
        break;
    case ERROR_CRC:
    case ERROR_SECTOR_NOT_FOUND:
    case ERROR_READ_FAULT:
    case ERROR_WRITE_FAULT:
    case ERROR_GEN_FAILURE:
    case ERROR_IO_DEVICE:
    case ERROR_SEEK:
    case ERROR_BAD_UNIT:
    case ERROR_DISK_CORRUPT:
    case ERROR_FILE_CORRUPT:
    case ERROR_DEVICE_HARDWARE_ERROR:
        kind = IoErrorKind::MediaError;
        break;
    // The device or its volume is gone (decision 0034): removed, switched
    // off, or dismounted under the open file (ERROR_FILE_INVALID), or its
    // medium taken out or changed. A removal also shows as an I/O error or
    // as "not found"; for those the engines look the volume up again.
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED:
    case ERROR_NO_SUCH_DEVICE:
    case ERROR_MEDIA_CHANGED:
    case ERROR_NO_MEDIA_IN_DRIVE:
    case ERROR_UNRECOGNIZED_VOLUME:
    case ERROR_FILE_INVALID:
        kind = IoErrorKind::DeviceRemoved;
        break;
    case ERROR_SEM_TIMEOUT:
    case WAIT_TIMEOUT:
        kind = IoErrorKind::Timeout;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        kind = IoErrorKind::NotSupported;
        break;
    case ERROR_OPERATION_ABORTED:
        kind = IoErrorKind::Cancelled;
        break;
    default:
        break;
    }
    // What went wrong first, then the call and Windows' number for whoever
    // looks further: "access denied (CreateFileW, Windows error 5)"; where no
    // plain words are known, "Windows error 1234 (CreateFileW)".
    const auto words = plain_words(code);
    const auto number = std::to_string(code);
    return {kind, static_cast<int>(code),
        words.empty() ? "Windows error " + number + " (" + operation + ")"
                      : std::string(words) + " (" + operation + ", Windows error " + number + ")"};
}

} // namespace va::platform::windows
