#pragma once
// How the window's side starts the device identity reader on Windows
// (decision 0047; platform/device_identity_helper.h): with administrator
// permission, which Windows asks the operator for (ShellExecuteExW, verb
// "runas"), from the program's own folder. The tests start it otherwise,
// without that permission, and describe sample devices, so that everything
// after the start is tested without a question to anyone.
#include "platform/windows/win_util.h"

#include "common/types.h"
#include "core/interfaces.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace va::platform::windows {

// Starts `reader` with the command line `parameters` (reader_parameters);
// gives a handle to wait on until it has ended, or why it did not start
// (PermissionDenied only where the operator declined administrator
// permission).
using ReaderStart = std::function<std::expected<UniqueHandle, IoError>(
    const std::filesystem::path& reader, const std::wstring& parameters)>;

// The start with administrator permission: ShellExecuteExW "runas", which
// returns once the operator answered Windows' question; ERROR_CANCELLED (the
// operator declined) is PermissionDenied.
std::expected<UniqueHandle, IoError> start_with_administrator_permission(
    const std::filesystem::path& reader, const std::wstring& parameters);

// What this program tells, without elevation, of the device whose number is
// `number` (the N of \\.\PhysicalDriveN); nothing where no device has it.
// The read is bound to the device by it: taken before Windows asks the
// operator and again once the reader has ended, it must not change, or
// another device took the number while the operator answered.
using DescribeDevice = std::function<std::optional<PhysicalDeviceInfo>(unsigned number)>;

// The media enumerator's description of that device (media_enumerator_win.cpp):
// model, serial, WWN, bus and capacity, as it lists the device.
std::optional<PhysicalDeviceInfo> describe_physical_device(unsigned number);

// The device identity reader `reader` (nothing: veriarchive-device-identity.exe
// in the program's folder), started by `start`, its device described by
// `describe`.
std::unique_ptr<IDeviceIdentityHelper> make_device_identity_helper(
    std::optional<std::filesystem::path> reader, ReaderStart start, DescribeDevice describe);

} // namespace va::platform::windows
