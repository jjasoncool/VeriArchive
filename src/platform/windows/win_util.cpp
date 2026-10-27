#include "platform/windows/win_util.h"

namespace va::platform::windows {

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
    return {kind, static_cast<int>(code), operation + " failed (Win32 error " +
                                              std::to_string(code) + ")"};
}

} // namespace va::platform::windows
