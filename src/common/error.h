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
    // The enclosure (the USB bridge or dock between a device and the
    // computer) passes no ATA command on to the device, so the device's own
    // identity cannot be read through it (decision 0047).
    CommandNotPassed,
    // Something of that name is there already: a file, a folder or a link
    // (Windows ERROR_FILE_EXISTS and ERROR_ALREADY_EXISTS, Linux EEXIST);
    // the refusal of IOutputFile::commit_new (decision 0048).
    AlreadyExists,
    Other,
};

struct IoError {
    IoErrorKind kind;
    int native_code = 0;
    std::string message;
};

} // namespace va