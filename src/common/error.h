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
    // The repository's database is damaged or is no database (SQLite's
    // SQLITE_CORRUPT and SQLITE_NOTADB), wherever SQLite finds it (decision
    // 0036): `repo rebuild-index` makes it again.
    DatabaseDamaged,
    Other,
};

struct IoError {
    IoErrorKind kind;
    int native_code = 0;
    std::string message;
};

} // namespace va