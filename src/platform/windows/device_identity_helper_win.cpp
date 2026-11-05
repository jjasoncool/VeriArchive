// The device identity reader on Windows (decision 0047). The program never
// elevates itself (R-12): it asks Windows to start its own small reader,
// veriarchive-device-identity.exe from the program's folder, with
// administrator permission (ShellExecuteExW, verb "runas"), which Windows
// asks the operator for. The reader gets two arguments only: the number of
// the device (the N of \\.\PhysicalDriveN; never a path, which an elevated
// program would open whatever it named) and the file to write its result to,
// in a folder made new for this read under this user's temporary folder. The
// program waits for the reader, reads its small JSON result, and removes the
// file and the folder. The device must be described alike before Windows asks
// and once the reader has ended, so that a device that took the number while
// the operator answered is not read in its place.
#include "platform/device_identity_helper.h"
#include "platform/platform_services.h"
#include "platform/windows/device_identity_reader.h"
#include "platform/windows/device_identity_start.h"

#include "common/time.h"
#include "common/utf8.h"
#include "common/uuid.h"

#include <objbase.h>
#include <shellapi.h>

#include <chrono>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace va::platform {
namespace windows {
namespace {

// How long the reader may take once it runs: it sends one command to the
// device, twice at most, each with a time limit of 15 s (helper/main.cpp).
constexpr auto reader_time_limit = std::chrono::seconds(60);
// How often the wait looks whether it is cancelled.
constexpr DWORD wait_step_ms = 100;
// The folder of one read under the temporary folder, and its result file.
constexpr std::wstring_view result_folder_prefix = L"VeriArchive-device-identity-";
constexpr std::wstring_view result_file_name = L"device-identity.json";

// An error of the platform, as none of the kinds that the operator's answer,
// the wait and the reader alone give (IDeviceIdentityHelper).
IoError unreadable(DWORD code, std::string operation)
{
    auto error = error_from_win32(code, std::move(operation));
    if (error.kind == IoErrorKind::PermissionDenied || error.kind == IoErrorKind::Cancelled ||
        error.kind == IoErrorKind::NotSupported || error.kind == IoErrorKind::CommandNotPassed) {
        error.kind = IoErrorKind::Other;
    }
    return error;
}

std::wstring wide(std::string_view text)
{
    const auto units = utf8_to_utf16(text);
    return std::wstring(units.begin(), units.end());
}

// COM on this thread, as ShellExecuteExW asks for: the shell may hand the
// start to an extension that it reaches through COM.
class ComOnThisThread {
public:
    ComOnThisThread()
        : started_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
    }
    ~ComOnThisThread()
    {
        if (started_) {
            CoUninitialize();
        }
    }
    ComOnThisThread(const ComOnThisThread&) = delete;
    ComOnThisThread& operator=(const ComOnThisThread&) = delete;

private:
    bool started_;
};

// The path by which the file system names the folder that `handle` is open
// on, with its drive letter ("\\?\C:\Users\...\Temp\..."): short (8.3) names
// and links resolved, as the reader checks the file it makes against it.
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

// Removes the folders that earlier reads left under `temporary`, with their
// result: a reader that this program could not end (it runs with
// administrator permission) may write its result after its read has gone.
// Passed over: a folder that a read still uses, which that read holds open
// (ResultFolder), so that it cannot be opened alone; a link; and a folder
// that holds anything but a result.
void remove_left_folders(const std::filesystem::path& temporary)
{
    std::error_code error;
    for (std::filesystem::directory_iterator entry(temporary, error), end; !error && entry != end;
         entry.increment(error)) {
        if (!entry->path().filename().native().starts_with(result_folder_prefix)) {
            continue;
        }
        const UniqueHandle folder(CreateFileW(entry->path().c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!folder.valid() || !GetFileInformationByHandle(folder.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        DeleteFileW((entry->path() / result_file_name).c_str());
        FILE_DISPOSITION_INFO remove{};
        remove.DeleteFile = TRUE;
        SetFileInformationByHandle(folder.get(), FileDispositionInfo, &remove, sizeof(remove));
    }
}

// A folder made new for one read under this user's temporary folder, and the
// result file in it, which the reader makes. Held open while the read lasts,
// so that no other read takes it for one left behind; removed with the file
// when it goes. A reader that still runs (it was cancelled, or took too long,
// and Windows did not let it be ended) may keep it there; the next read
// removes it (remove_left_folders).
class ResultFolder {
public:
    static std::expected<ResultFolder, IoError> make()
    {
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error) {
            return std::unexpected(
                IoError{IoErrorKind::Other, error.value(), "no temporary folder: " + error.message()});
        }
        remove_left_folders(temporary);
        ResultFolder made;
        const auto folder = temporary / (std::wstring(result_folder_prefix) + wide(generate_uuid()));
        if (!CreateDirectoryW(folder.c_str(), nullptr)) {
            return std::unexpected(unreadable(GetLastError(), "CreateDirectoryW"));
        }
        made.folder_ = folder;
        // Listing access, shared for reading and writing but not for
        // deleting: remove_left_folders cannot open it alone.
        made.held_ = UniqueHandle(CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        const auto final_folder = made.held_.valid() ? final_path_of(made.held_.get()) : std::nullopt;
        if (!final_folder) {
            return std::unexpected(unreadable(GetLastError(), "GetFinalPathNameByHandleW"));
        }
        made.result_ = std::filesystem::path(*final_folder) / result_file_name;
        return made;
    }

    ResultFolder(ResultFolder&& other) noexcept
        : folder_(std::exchange(other.folder_, {})), result_(std::move(other.result_)),
          held_(std::move(other.held_))
    {
    }
    ResultFolder& operator=(ResultFolder&&) = delete;
    ResultFolder(const ResultFolder&) = delete;
    ResultFolder& operator=(const ResultFolder&) = delete;
    ~ResultFolder()
    {
        if (!folder_.empty()) {
            if (!result_.empty()) {
                DeleteFileW(result_.c_str());
            }
            held_.reset();
            RemoveDirectoryW(folder_.c_str());
        }
    }

    // The result file, as the reader is given it: by its final path.
    const std::filesystem::path& result() const { return result_; }

private:
    ResultFolder() = default;

    std::filesystem::path folder_;
    std::filesystem::path result_;
    UniqueHandle held_;
};

// The result file's text, up to one byte more than the reader ever writes;
// nothing where there is no file.
std::optional<std::string> read_result(const std::filesystem::path& file)
{
    const UniqueHandle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return std::nullopt;
    }
    std::string text(largest_reader_result + 1, '\0');
    DWORD total = 0;
    while (total < text.size()) {
        DWORD got = 0;
        if (!ReadFile(handle.get(), text.data() + total, static_cast<DWORD>(text.size() - total), &got, nullptr) ||
            got == 0) {
            break;
        }
        total += got;
    }
    text.resize(total);
    return text;
}

// Whether `reader` is a file (not a folder) that is there.
bool reader_present(const std::filesystem::path& reader)
{
    const DWORD attributes = GetFileAttributesW(reader.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Whether two descriptions of the device that has one number are of one
// device: as this program tells it without elevation.
bool same_description(const PhysicalDeviceInfo& before, const PhysicalDeviceInfo& after)
{
    return before.model == after.model && before.serial == after.serial && before.wwn == after.wwn &&
           before.bus == after.bus && before.capacity_bytes == after.capacity_bytes;
}

class WindowsDeviceIdentityHelper final : public IDeviceIdentityHelper {
public:
    WindowsDeviceIdentityHelper(std::optional<std::filesystem::path> reader, ReaderStart start,
        DescribeDevice describe)
        : reader_(std::move(reader)), start_(std::move(start)), describe_(std::move(describe))
    {
    }

    bool available() const override
    {
        const auto reader = reader_path();
        return reader && reader_present(*reader);
    }

    std::expected<DeviceIdentityRead, IoError> read_device_identity(
        const std::string& device_ref, CancelToken& cancel) override
    {
        if (cancel.cancelled()) {
            return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "cancelled"});
        }
        const auto number = physical_drive_number(device_ref);
        if (!number) {
            return std::unexpected(
                IoError{IoErrorKind::Other, 0, "no device of this computer by its number: " + device_ref});
        }
        const auto reader = reader_path();
        if (!reader) {
            return std::unexpected(reader.error());
        }
        if (!reader_present(*reader)) {
            return std::unexpected(IoError{IoErrorKind::NotFound, 0,
                "the device identity reader is missing: " + path_to_utf8(*reader)});
        }
        // The device as this program tells it before Windows asks the
        // operator, which may take minutes.
        const auto before = describe_(*number);
        if (!before) {
            return std::unexpected(
                IoError{IoErrorKind::NotFound, 0, "the device is no longer attached: " + device_ref});
        }
        auto folder = ResultFolder::make();
        if (!folder) {
            return std::unexpected(folder.error());
        }
        auto process = start_(*reader, reader_parameters(*number, folder->result().native()));
        if (!process) {
            return std::unexpected(process.error());
        }

        const auto until = std::chrono::steady_clock::now() + reader_time_limit;
        for (;;) {
            const DWORD waited = WaitForSingleObject(process->get(), wait_step_ms);
            if (waited == WAIT_OBJECT_0) {
                break;
            }
            if (waited == WAIT_FAILED) {
                return std::unexpected(unreadable(GetLastError(), "WaitForSingleObject"));
            }
            const bool cancelled = cancel.cancelled();
            if (cancelled || std::chrono::steady_clock::now() >= until) {
                // Windows may refuse this program to end the elevated reader;
                // it then ends on its own, and its folder may stay until the
                // next read removes it.
                if (TerminateProcess(process->get(), 1)) {
                    WaitForSingleObject(process->get(), 2000);
                }
                if (cancelled) {
                    return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "cancelled"});
                }
                return std::unexpected(
                    IoError{IoErrorKind::Timeout, 0, "the device identity reader did not finish in time"});
            }
        }
        // The same device still has the number: else the reader read another
        // one, which took the number while the operator answered.
        const auto after = describe_(*number);
        if (!after) {
            return std::unexpected(
                IoError{IoErrorKind::NotFound, 0, "the device is no longer attached: " + device_ref});
        }
        if (!same_description(*before, *after)) {
            return std::unexpected(IoError{IoErrorKind::NotFound, 0,
                "the device changed while it was read: another device has its number now (" + device_ref + ")"});
        }
        DWORD exit_code = 0;
        GetExitCodeProcess(process->get(), &exit_code);
        const auto text = read_result(folder->result());
        if (!text) {
            return std::unexpected(IoError{IoErrorKind::Other, static_cast<int>(exit_code),
                "the device identity reader wrote no result (exit " + std::to_string(exit_code) + ")"});
        }
        return reader_result(*text, device_ref, current_timestamp());
    }

private:
    // The reader given, or veriarchive-device-identity.exe in the program's
    // folder.
    std::expected<std::filesystem::path, IoError> reader_path() const
    {
        if (reader_) {
            return *reader_;
        }
        const auto program = executable_path();
        if (!program) {
            return std::unexpected(IoError{IoErrorKind::Other, program.error().native_code, program.error().message});
        }
        return program->parent_path() / device_identity_reader_name;
    }

    std::optional<std::filesystem::path> reader_;
    ReaderStart start_;
    DescribeDevice describe_;
};

} // namespace

std::expected<UniqueHandle, IoError> start_with_administrator_permission(
    const std::filesystem::path& reader, const std::wstring& parameters)
{
    const ComOnThisThread com;
    SHELLEXECUTEINFOW start{};
    start.cbSize = sizeof(start);
    start.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    start.lpVerb = L"runas";
    start.lpFile = reader.c_str();
    start.lpParameters = parameters.c_str();
    start.nShow = SW_HIDE;
    // Windows brings its question to the front only for a request from the
    // window in front, and this runs off the window's thread: that window is
    // named as the request's where it is this program's.
    if (const HWND front = GetForegroundWindow()) {
        DWORD front_process = 0;
        GetWindowThreadProcessId(front, &front_process);
        if (front_process == GetCurrentProcessId()) {
            start.hwnd = front;
        }
    }
    // Windows asks the operator here, and returns once they answered.
    if (!ShellExecuteExW(&start)) {
        const DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            return std::unexpected(IoError{IoErrorKind::PermissionDenied, static_cast<int>(code),
                "administrator permission for the device identity reader was declined"});
        }
        return std::unexpected(unreadable(code, "ShellExecuteExW"));
    }
    UniqueHandle process(start.hProcess);
    if (!process.valid()) {
        return std::unexpected(
            IoError{IoErrorKind::Other, 0, "Windows gave no handle of the device identity reader it started"});
    }
    return process;
}

std::unique_ptr<IDeviceIdentityHelper> make_device_identity_helper(
    std::optional<std::filesystem::path> reader, ReaderStart start, DescribeDevice describe)
{
    return std::make_unique<WindowsDeviceIdentityHelper>(std::move(reader), std::move(start), std::move(describe));
}

} // namespace windows

std::unique_ptr<IDeviceIdentityHelper> make_device_identity_helper()
{
    return windows::make_device_identity_helper(
        std::nullopt, windows::start_with_administrator_permission, windows::describe_physical_device);
}

} // namespace va::platform
