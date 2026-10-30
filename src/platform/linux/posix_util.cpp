#include "platform/linux/posix_util.h"

#include <cerrno>
#include <cstring>

namespace va::platform::posix {

IoError error_from_errno(int code, std::string operation)
{
    IoErrorKind kind = IoErrorKind::Other;
    switch (code) {
    case ENOENT:
    case ENOTDIR:
        kind = IoErrorKind::NotFound;
        break;
    case EACCES:
    case EPERM:
    case EROFS:
        kind = IoErrorKind::PermissionDenied;
        break;
    case EIO:
    case EBADMSG:
    case EUCLEAN:
        kind = IoErrorKind::MediaError;
        break;
    // The device or its filesystem is gone (decision 0034): ENOLINK when the
    // link to the device was cut (a USB disk pulled out), ENOTCONN when the
    // FUSE program that served the mount has ended (ntfs-3g after its device
    // went away). A pulled disk gives EIO as often: for that, and for "not
    // found", the engines look the volume up again.
    case ENODEV:
    case ENXIO:
    case ENOMEDIUM:
    case ESHUTDOWN:
    case ENOLINK:
    case ENOTCONN:
        kind = IoErrorKind::DeviceRemoved;
        break;
    // A request the device did not finish: no answer in time, or one that the
    // kernel cancelled (ECANCELED). This program never cancels its own
    // requests (its cancel is the CancelToken, IoErrorKind::Cancelled), so the
    // engines treat both as a device that did not answer (decision 0034).
    case ETIMEDOUT:
    case ECANCELED:
        kind = IoErrorKind::Timeout;
        break;
    case ENOTSUP:
    case ENOSYS:
        kind = IoErrorKind::NotSupported;
        break;
    default:
        break;
    }
    return {kind, code, operation + " failed: " + std::strerror(code)};
}

} // namespace va::platform::posix
