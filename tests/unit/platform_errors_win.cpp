// How the Windows platform layer names a failed call's Win32 error (§B9).
// Built on Windows only; the Linux build has platform_errors_linux.cpp.
#include "test_support.h"

#include "platform/windows/win_util.h"

void platform_error_tests()
{
    using va::IoErrorKind;
    using va::platform::windows::error_from_win32;
    // Decision 0034: the errors of a device or a volume that is gone (removed,
    // switched off, dismounted under the open file, or its medium taken out
    // or changed) are DeviceRemoved, which interrupts the operation.
    for (const DWORD code : {ERROR_NOT_READY, ERROR_DEVICE_NOT_CONNECTED, ERROR_DEV_NOT_EXIST, ERROR_DEVICE_REMOVED,
             ERROR_NO_SUCH_DEVICE, ERROR_MEDIA_CHANGED, ERROR_NO_MEDIA_IN_DRIVE, ERROR_UNRECOGNIZED_VOLUME,
             ERROR_FILE_INVALID}) {
        const auto error = error_from_win32(code, "ReadFile");
        const bool removed = error.kind == IoErrorKind::DeviceRemoved && error.native_code == static_cast<int>(code);
        CHECK(removed);
        if (!removed) {
            std::cerr << "  Win32 error " << code << " is not DeviceRemoved\n";
        }
    }
    // A read error says nothing of a removal by itself: the engines look the
    // volume up again before they take it for the entry's.
    for (const DWORD code : {ERROR_CRC, ERROR_IO_DEVICE, ERROR_GEN_FAILURE}) {
        CHECK(error_from_win32(code, "ReadFile").kind == IoErrorKind::MediaError);
    }
    // A request that Windows aborted is the device's, as one it did not
    // answer in time: the program's own cancel is never a Win32 error.
    for (const DWORD code : {ERROR_SEM_TIMEOUT, ERROR_OPERATION_ABORTED}) {
        const auto error = error_from_win32(code, "ReadFile");
        CHECK(error.kind == IoErrorKind::Timeout && error.native_code == static_cast<int>(code));
    }
    CHECK(error_from_win32(ERROR_PATH_NOT_FOUND, "CreateFileW").kind == IoErrorKind::NotFound);
    CHECK(error_from_win32(ERROR_ACCESS_DENIED, "CreateFileW").kind == IoErrorKind::PermissionDenied);
}
