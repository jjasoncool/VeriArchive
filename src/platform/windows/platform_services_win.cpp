#include "platform/platform_services.h"
#include "common/utf8.h"
#include "platform/windows/long_path.h"
#include "platform/windows/root_kind.h"
#include "platform/windows/subst_drive.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace va::platform {

std::expected<std::filesystem::path, IoError> executable_path()
{
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::unexpected(IoError{IoErrorKind::Other,
                static_cast<int>(GetLastError()), "GetModuleFileNameW failed"});
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::expected<std::filesystem::path, IoError> user_home()
{
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) {
        return std::filesystem::path(profile);
    }
    return std::unexpected(IoError{IoErrorKind::NotFound, 0, "USERPROFILE is unavailable"});
}

std::expected<std::filesystem::path, IoError> user_config_dir()
{
    if (const wchar_t* app_data = _wgetenv(L"APPDATA")) {
        return std::filesystem::path(app_data) / L"VeriArchive";
    }
    auto home = user_home();
    if (!home) {
        return std::unexpected(home.error());
    }
    return *home / L"AppData" / L"Roaming" / L"VeriArchive";
}

bool writable_directory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    // CREATE_NEW never truncates a pre-existing file, even with concurrent starts.
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        const auto name = L".veriarchive-probe-" + std::to_wstring(GetCurrentProcessId()) +
                          L"-" + std::to_wstring(GetTickCount64()) +
                          L"-" + std::to_wstring(attempt);
        const auto probe = directory / name;
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) {
                continue;
            }
            return false;
        }
        DWORD written = 0;
        const char byte = 0;
        const bool writable = WriteFile(file, &byte, 1, &written, nullptr) &&
                              written == 1 && FlushFileBuffers(file);
        CloseHandle(file);
        return writable;
    }
    return false;
}

// Fails closed: a volume that cannot be identified is treated as remote, so
// SQLite is never silently placed on a network share.
bool network_filesystem(const std::filesystem::path& path)
{
    // The volume that actually holds the path (it may be a volume mounted into
    // a folder, not the drive letter at the start of the path).
    std::vector<wchar_t> volume(32768);
    if (!GetVolumePathNameW(path.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) {
        return true;
    }
    const std::wstring volume_path(volume.data());
    const UINT type = GetDriveTypeW(volume_path.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) {
        return true;
    }

    if (volume_path.size() >= 2 && volume_path[1] == L':') {
        wchar_t remote_name[MAX_PATH]{};
        DWORD length = MAX_PATH;
        const auto drive = volume_path.substr(0, 2);
        return WNetGetConnectionW(drive.c_str(), remote_name, &length) == NO_ERROR;
    }
    return false;
}

std::filesystem::path resolve_subst_drive(const std::filesystem::path& path)
{
    // QueryDosDeviceW reads the drive letters of this logon session, which is
    // where subst makes them; it needs no privilege (R-09).
    const auto query = [](std::wstring_view drive) -> std::optional<std::wstring> {
        const std::wstring name(drive);
        std::vector<wchar_t> targets(1024);
        while (QueryDosDeviceW(name.c_str(), targets.data(), static_cast<DWORD>(targets.size())) == 0) {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || targets.size() >= 65536) {
                return std::nullopt; // not defined, or no answer: the letter is left as it is
            }
            targets.resize(targets.size() * 2);
        }
        return std::wstring(targets.data()); // the first target of the NUL-separated list
    };
    return std::filesystem::path(windows::resolve_subst(path.native(), query));
}

std::filesystem::path normal_volume_guid_path(const std::filesystem::path& path)
{
    const auto normal = windows::normal_volume_guid_path(path.native());
    return normal ? std::filesystem::path(*normal) : path;
}

RootKind archive_root_kind(const std::filesystem::path& root)
{
    // GetDriveTypeW tells a drive letter mapped to a network share
    // (DRIVE_REMOTE), unprivileged (R-09).
    return windows::root_kind(root.native(), [](std::wstring_view drive) {
        const std::wstring drive_root = std::wstring(drive) + L"\\";
        return GetDriveTypeW(drive_root.c_str()) == DRIVE_REMOTE;
    });
}

bool is_directory(const std::filesystem::path& path)
{
    auto name = windows::long_path(path.native());
    auto attributes = GetFileAttributesW(name.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && !name.empty() && name.back() != L'\\') {
        // "\\?\Volume{...}" names the volume; its root folder is "\\?\Volume{...}\".
        name += L'\\';
        attributes = GetFileAttributesW(name.c_str());
    }
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool process_is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(
                              token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                          elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

std::string os_name()
{
    return "windows";
}

std::string host_name()
{
    wchar_t name[256]{};
    DWORD length = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &length)) {
        return {};
    }
    return utf16_to_utf8(std::u16string(name, name + length));
}

std::chrono::seconds utc_offset(std::chrono::sys_seconds time)
{
    // Ask Windows for its own time zone rather than the C library's localtime,
    // which follows a TZ environment variable when one is set (some shells set
    // it, in a form the Windows C library misreads).
    // FILETIME counts 100 ns intervals since 1601-01-01; Unix time starts 1970.
    constexpr long long seconds_1601_to_1970 = 11'644'473'600LL;
    constexpr long long ticks_per_second = 10'000'000LL;
    const long long ticks = (time.time_since_epoch().count() + seconds_1601_to_1970) * ticks_per_second;
    if (ticks < 0) {
        return std::chrono::seconds{0};
    }
    const FILETIME utc_file{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    FILETIME local_file{};
    if (!FileTimeToSystemTime(&utc_file, &utc) ||
        GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID ||
        !SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local) ||
        !SystemTimeToFileTime(&local, &local_file)) {
        return std::chrono::seconds{0};
    }
    const long long local_ticks =
        (static_cast<long long>(local_file.dwHighDateTime) << 32) | local_file.dwLowDateTime;
    return std::chrono::seconds((local_ticks - ticks) / ticks_per_second);
}

std::string ui_language()
{
    // The display language, not the regional format: an English Windows set to
    // Taiwan's date format keeps an English interface.
    ULONG count = 0;
    ULONG size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) {
        return {};
    }
    std::wstring names(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size) || count == 0) {
        return {};
    }
    const auto first = std::wstring(names.c_str()); // the list is NUL-separated
    return utf16_to_utf8(std::u16string(first.begin(), first.end()));
}

namespace {

// Shows UTF-8 text in a console window. The console's code page is left alone:
// it belongs to the window, not to this process, and a changed code page would
// stay behind after Ctrl+C or a crash and garble later programs in the user's
// shell. WriteConsoleW takes UTF-16, which the console shows correctly whatever
// its code page, so each write is converted from UTF-8 first.
//
// The buffer writes at once and holds back only the first bytes of a character
// whose remaining bytes have not arrived yet (one character may be split across
// two writes). Output keeps its order with the other stream, and nothing is left
// to flush at exit.
class ConsoleBuffer final : public std::streambuf {
public:
    explicit ConsoleBuffer(HANDLE console) : console_(console) {}

protected:
    // Called by the stream for every piece of text (`std::cout << "..."`).
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        // The standard streams may be written from several threads (the C library's
        // buffers are locked too), and `pending_` must not be changed by two at once.
        const std::lock_guard lock(mutex_);
        pending_.append(data, static_cast<std::size_t>(count));
        const auto complete = complete_length(pending_);
        const bool written = write(std::string_view(pending_).substr(0, complete));
        pending_.erase(0, complete);
        return written ? count : 0; // 0 sets the stream's badbit, as a failed fwrite would
    }

    // Called for a single character; this buffer has no put area, so that is
    // every `std::cout << 'x'`.
    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character); // a flush request: nothing is buffered
        }
        const char byte = traits_type::to_char_type(character);
        return xsputn(&byte, 1) == 1 ? character : traits_type::eof();
    }

private:
    // The length of `bytes` without an unfinished UTF-8 character at its end.
    // Malformed bytes count as finished: they are shown as U+FFFD, never held.
    static std::size_t complete_length(std::string_view bytes)
    {
        const std::size_t size = bytes.size();
        for (std::size_t back = 1; back <= std::min<std::size_t>(3, size); ++back) {
            const auto byte = static_cast<unsigned char>(bytes[size - back]);
            if ((byte & 0xC0) == 0x80) {
                continue; // a continuation byte: look further back for the first byte
            }
            // `byte` starts the last character; the first byte tells its length.
            std::size_t length = 1; // ASCII, or a byte that never starts a character
            if (byte >= 0xC0 && byte < 0xE0) {
                length = 2;
            } else if (byte >= 0xE0 && byte < 0xF0) {
                length = 3;
            } else if (byte >= 0xF0 && byte < 0xF8) {
                length = 4;
            }
            return length > back ? size - back : size;
        }
        return size;
    }

    bool write(std::string_view utf8) const
    {
        const auto units = utf8_to_utf16(utf8); // malformed bytes become U+FFFD
        std::size_t done = 0;
        while (done < units.size()) {
            // Moderate pieces: older consoles refuse very large writes. A piece
            // never ends between the two halves of a surrogate pair.
            std::size_t piece = std::min<std::size_t>(units.size() - done, 8192);
            if (done + piece < units.size() && units[done + piece - 1] >= 0xD800 &&
                units[done + piece - 1] <= 0xDBFF) {
                --piece;
            }
            DWORD written = 0;
            if (!WriteConsoleW(console_, units.data() + done, static_cast<DWORD>(piece), &written,
                    nullptr) ||
                written == 0) {
                return false;
            }
            done += written;
        }
        return true;
    }

    HANDLE console_;
    std::mutex mutex_;
    std::string pending_; // the start of an unfinished character, at most 3 bytes between writes
};

// True when the handle is a console window, not a pipe or a file.
bool is_console(HANDLE handle)
{
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetConsoleMode(handle, &mode) != 0;
}

} // namespace

void prepare_console()
{
    // Once per process. The buffers are never deleted on purpose: the standard
    // streams are flushed at exit after static objects are destroyed, so they
    // must outlive everything.
    static const bool prepared = [] {
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (is_console(output)) {
            std::cout.rdbuf(new ConsoleBuffer(output));
        }
        // A redirected stream keeps its own buffer and gets the UTF-8 bytes unchanged.
        const HANDLE errors = GetStdHandle(STD_ERROR_HANDLE);
        if (is_console(errors)) {
            auto* buffer = new ConsoleBuffer(errors);
            std::cerr.rdbuf(buffer);
            std::clog.rdbuf(buffer);
        }
        return true;
    }();
    (void)prepared;
}

} // namespace va::platform