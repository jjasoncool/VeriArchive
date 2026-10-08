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
    case ENODEV:
    case ENXIO:
    case ENOMEDIUM:
    case ESHUTDOWN:
        kind = IoErrorKind::DeviceRemoved;
        break;
    case ETIMEDOUT:
        kind = IoErrorKind::Timeout;
        break;
    case ENOTSUP:
    case ENOSYS:
        kind = IoErrorKind::NotSupported;
        break;
    case ECANCELED:
        kind = IoErrorKind::Cancelled;
        break;
    default:
        break;
    }
    return {kind, code, operation + " failed: " + std::strerror(code)};
}

} // namespace va::platform::posix
