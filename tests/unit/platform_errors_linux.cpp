// How the Linux platform layer names a failed call's errno (§B9). Built on
// Linux only; the Windows build has platform_errors_win.cpp.
#include "test_support.h"

#include "platform/linux/posix_util.h"

#include <cerrno>

void platform_error_tests()
{
    using va::IoErrorKind;
    using va::platform::posix::error_from_errno;
    // Decision 0034: the errors of a device or a filesystem that is gone
    // (ENOLINK: the link to the device was cut; ENOTCONN: the FUSE program
    // that served the mount has ended) are DeviceRemoved, which interrupts
    // the operation.
    for (const int code : {ENODEV, ENXIO, ENOMEDIUM, ESHUTDOWN, ENOLINK, ENOTCONN}) {
        const auto error = error_from_errno(code, "pread");
        const bool removed = error.kind == IoErrorKind::DeviceRemoved && error.native_code == code;
        CHECK(removed);
        if (!removed) {
            std::cerr << "  errno " << code << " is not DeviceRemoved\n";
        }
    }
    // A USB disk pulled out gives EIO as often as a bad sector does: it stays a
    // media error, and the engines look the volume up again before they take
    // it for the entry's.
    CHECK(error_from_errno(EIO, "pread").kind == IoErrorKind::MediaError);
    CHECK(error_from_errno(ENOENT, "open").kind == IoErrorKind::NotFound);
    CHECK(error_from_errno(EACCES, "open").kind == IoErrorKind::PermissionDenied);
}
