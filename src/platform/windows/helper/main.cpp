// The device identity reader, veriarchive-device-identity.exe (decision
// 0047): the one program of VeriArchive that runs with administrator
// permission. Its manifest asks Windows for it (requireAdministrator), so
// Windows starts it only once the operator allows it; the window starts it on
// the operator's request (platform::make_device_identity_helper), never
// itself elevated (R-12). It reads a device's own identity where the
// device's enclosure reports its own serial in place of the device's: it
// sends the device ATA IDENTIFY DEVICE, which returns the device's
// identification and changes nothing, through ATA pass-through, and where
// that fails inside the SCSI command ATA PASS-THROUGH (16), as USB bridges
// take it. No other command, nothing read of the device's content, nothing
// written to the device, no repository, no window.
//
//   veriarchive-device-identity.exe <device number> <result file>
//
// <device number>: 0 to 63, the N of \\.\PhysicalDriveN, nothing else.
// <result file>: a full path of a .json file that is not there yet; the
// reader makes it new and writes its result there (device_identity_reader.h).
// Exit status: 0 the identity is written; 1 an error is written; 2 the
// arguments are refused, nothing is written and no device is opened; 3 the
// result file cannot be made (it is there, or it is not where its path says;
// no device is then opened), or cannot be written.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include "platform/windows/device_identity_reader.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace va::platform::windows;

constexpr int identity_written = 0;
constexpr int error_written = 1;
constexpr int arguments_refused = 2;
constexpr int result_refused = 3;

// The time each command may take, in seconds: a sleeping hard disk behind a
// bridge may first spin up. Both tries fit in the time that the program which
// started the reader waits for it (device_identity_helper_win.cpp, 60 s).
constexpr ULONG command_time_limit = 15;

class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_;
};

// The path by which the file system names the file `handle` is open on, in
// the \\?\ form with its drive letter.
std::optional<std::wstring> final_path_of(HANDLE handle)
{
    std::vector<wchar_t> buffer(512);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(length + 1);
    }
    return std::nullopt;
}

// Makes the result file new, and keeps it only where it is the file its path
// names: a link or junction on the way that led elsewhere has it removed
// again at once. Nothing where it cannot be made so.
std::optional<HANDLE> make_result_file(std::wstring_view path)
{
    const std::wstring name(path);
    const HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const std::wstring wanted = name.starts_with(L"\\\\?\\") ? name : L"\\\\?\\" + name;
    const auto final_path = final_path_of(file);
    if (!final_path ||
        CompareStringOrdinal(final_path->c_str(), static_cast<int>(final_path->size()), wanted.c_str(),
            static_cast<int>(wanted.size()), TRUE) != CSTR_EQUAL) {
        FILE_DISPOSITION_INFO remove{};
        remove.DeleteFile = TRUE;
        SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
        CloseHandle(file);
        return std::nullopt;
    }
    return file;
}

int write_result(HANDLE file, const std::string& result, int status)
{
    DWORD written = 0;
    const bool whole = WriteFile(file, result.data(), static_cast<DWORD>(result.size()), &written, nullptr) &&
                       written == result.size();
    CloseHandle(file);
    return whole ? status : result_refused;
}

// IDENTIFY DEVICE through ATA pass-through, as a SATA controller takes it.
// `error`: the Win32 error where the request failed, 0 where the device
// refused the command or answered with no identification.
std::optional<AtaIdentity> by_ata_pass_through(HANDLE device, DWORD& error)
{
    struct Request {
        ATA_PASS_THROUGH_EX header;
        alignas(16) unsigned char data[identify_data_size];
    };
    Request request;
    std::memset(&request, 0, sizeof(request));
    request.header.Length = sizeof(ATA_PASS_THROUGH_EX);
    request.header.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    request.header.DataTransferLength = identify_data_size;
    request.header.TimeOutValue = command_time_limit;
    request.header.DataBufferOffset = offsetof(Request, data);
    request.header.CurrentTaskFile[6] = identify_device_command;  // the command register
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_ATA_PASS_THROUGH, &request, sizeof(request), &request, sizeof(request),
            &returned, nullptr)) {
        error = GetLastError();
        return std::nullopt;
    }
    error = 0;
    // Back from the device, the command register holds its status: ERR
    // (bit 0) says it refused the command.
    if ((request.header.CurrentTaskFile[6] & 0x01) != 0) {
        return std::nullopt;
    }
    return decode_identify_data(std::as_bytes(std::span(request.data)));
}

// IDENTIFY DEVICE inside ATA PASS-THROUGH (16), as a USB bridge that
// follows SAT takes it. `error` as above; 0 also where the bridge answered
// with a SCSI status other than GOOD, as one that does not pass the command
// on does.
std::optional<AtaIdentity> by_sat(HANDLE device, DWORD& error)
{
    struct Request {
        SCSI_PASS_THROUGH header;
        alignas(16) unsigned char sense[32];
        alignas(16) unsigned char data[identify_data_size];
    };
    Request request;
    std::memset(&request, 0, sizeof(request));
    request.header.Length = sizeof(SCSI_PASS_THROUGH);
    request.header.CdbLength = 16;
    request.header.SenseInfoLength = sizeof(request.sense);
    request.header.DataIn = SCSI_IOCTL_DATA_IN;
    request.header.DataTransferLength = identify_data_size;
    request.header.TimeOutValue = command_time_limit;
    request.header.SenseInfoOffset = offsetof(Request, sense);
    request.header.DataBufferOffset = offsetof(Request, data);
    const auto cdb = identify_device_cdb();
    std::memcpy(request.header.Cdb, cdb.data(), cdb.size());
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_SCSI_PASS_THROUGH, &request, sizeof(request), &request, sizeof(request),
            &returned, nullptr)) {
        error = GetLastError();
        return std::nullopt;
    }
    error = 0;
    if (request.header.ScsiStatus != 0) {
        return std::nullopt;
    }
    return decode_identify_data(std::as_bytes(std::span(request.data)));
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3) {
        return arguments_refused;
    }
    const auto number = device_number_argument(argv[1]);
    if (!number || !output_path_argument(argv[2])) {
        return arguments_refused;
    }
    // The result file first: where it cannot be made, no device is opened.
    const auto file = make_result_file(argv[2]);
    if (!file) {
        return result_refused;
    }

    // Both pass-through requests take a handle with read and write access
    // (their control codes say so). Nothing of the device's content is read
    // or written through it: the one command sent is IDENTIFY DEVICE.
    const std::wstring name = L"\\\\.\\PhysicalDrive" + std::to_wstring(*number);
    const Handle device(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!device.valid()) {
        const DWORD code = GetLastError();
        return write_result(*file, error_result_json("open-failed", code), error_written);
    }
    DWORD error = 0;
    if (const auto identity = by_ata_pass_through(device.get(), error)) {
        return write_result(*file, identity_result_json(*identity, ReaderHow::Ata), identity_written);
    }
    if (const auto identity = by_sat(device.get(), error)) {
        return write_result(*file, identity_result_json(*identity, ReaderHow::Sat), identity_written);
    }
    return write_result(*file, error_result_json("not-passed", error), error_written);
}
