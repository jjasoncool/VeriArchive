#pragma once
#include <string>

namespace va {

enum class IoErrorKind {
    NotFound,
    PermissionDenied,
    DeviceRemoved,
    MediaError,
    Timeout,
    NotSupported,
    Cancelled,
    Other,
};

struct IoError {
    IoErrorKind kind;
    int native_code = 0;
    std::string message;
};

} // namespace va