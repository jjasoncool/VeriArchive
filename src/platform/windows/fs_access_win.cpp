// Windows file access for scanning and hashing.
//  - Listing uses FindFirstFileExW; a reparse point is a Link only when its tag is
//    a name surrogate (decision 0009); a mount point whose target is a volume is
//    reported as is_mount_point (R-30); every other reparse point is a file.
//  - Reads bypass the cache with FILE_FLAG_NO_BUFFERING (R-100), which requires
//    sector-aligned offsets, lengths and buffers: alignment() tells the caller.
//  - Link targets are returned in the canonical form of decision 0008.
#include "platform/file_system.h"
#include "platform/windows/win_util.h"

#include "common/utf8.h"

#include <algorithm>
#include <cstring>
#include <winioctl.h>

namespace va::platform {
namespace {

using windows::UniqueHandle;
using windows::error_from_win32;
using windows::long_path;

// REPARSE_DATA_BUFFER is a kernel-mode structure; user-mode headers do not
// declare it, so its documented layout is reproduced here.
struct ReparseHeader {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
};
struct LinkBuffer {
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
};

#ifndef IO_REPARSE_TAG_LX_SYMLINK
#define IO_REPARSE_TAG_LX_SYMLINK 0xA000001DL
#endif

constexpr std::int64_t filetime_unix_epoch = 116444736000000000LL; // 1601 -> 1970 in 100 ns

std::int64_t filetime_to_unix_ns(const FILETIME& time)
{
    const auto ticks = (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return (ticks - filetime_unix_epoch) * 100;
}

std::expected<std::vector<BYTE>, IoError> read_reparse_data(const std::filesystem::path& path)
{
    UniqueHandle handle(CreateFileW(long_path(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) {
        return std::unexpected(error_from_win32(GetLastError(), "CreateFileW (reparse point)"));
    }
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
            static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        return std::unexpected(error_from_win32(GetLastError(), "FSCTL_GET_REPARSE_POINT"));
    }
    buffer.resize(returned);
    return buffer;
}

std::wstring name_at(const std::vector<BYTE>& buffer, std::size_t path_buffer_offset,
    USHORT offset, USHORT length)
{
    const auto start = path_buffer_offset + offset;
    if (start + length > buffer.size()) {
        return {};
    }
    std::wstring name(length / sizeof(wchar_t), L'\0');
    std::memcpy(name.data(), buffer.data() + start, length);
    return name;
}

// Decision 0008: PrintName (or SubstituteName without \??\) with '\' -> '/'.
std::expected<std::string, IoError> canonical_link_target(const std::vector<BYTE>& buffer)
{
    if (buffer.size() < sizeof(ReparseHeader)) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, "short reparse buffer"});
    }
    ReparseHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));

    if (header.tag == IO_REPARSE_TAG_LX_SYMLINK) {
        // WSL symlink: 4-byte version, then the UTF-8 target.
        const auto start = sizeof(ReparseHeader) + 4;
        if (buffer.size() < start) {
            return std::unexpected(IoError{IoErrorKind::Other, 0, "short WSL symlink"});
        }
        return std::string(reinterpret_cast<const char*>(buffer.data() + start),
            buffer.size() - start);
    }

    std::size_t path_buffer = sizeof(ReparseHeader) + sizeof(LinkBuffer);
    if (header.tag == IO_REPARSE_TAG_SYMLINK) {
        path_buffer += sizeof(ULONG); // Flags
    } else if (header.tag != IO_REPARSE_TAG_MOUNT_POINT) {
        return std::unexpected(IoError{IoErrorKind::NotSupported, 0, "unsupported link type"});
    }
    if (buffer.size() < path_buffer) {
        return std::unexpected(IoError{IoErrorKind::Other, 0, "short reparse buffer"});
    }
    LinkBuffer link{};
    std::memcpy(&link, buffer.data() + sizeof(ReparseHeader), sizeof(link));

    auto target = name_at(buffer, path_buffer, link.print_offset, link.print_length);
    if (target.empty()) {
        target = name_at(buffer, path_buffer, link.substitute_offset, link.substitute_length);
        if (target.starts_with(L"\\??\\")) {
            target.erase(0, 4);
        }
    }
    std::replace(target.begin(), target.end(), L'\\', L'/');
    return utf16_to_utf8(std::u16string(target.begin(), target.end()));
}

bool is_volume_mount_point(const std::vector<BYTE>& buffer)
{
    if (buffer.size() < sizeof(ReparseHeader) + sizeof(LinkBuffer)) {
        return false;
    }
    LinkBuffer link{};
    std::memcpy(&link, buffer.data() + sizeof(ReparseHeader), sizeof(link));
    const auto substitute = name_at(
        buffer, sizeof(ReparseHeader) + sizeof(LinkBuffer), link.substitute_offset,
        link.substitute_length);
    return substitute.starts_with(L"\\??\\Volume{");
}

DWORD alignment_of(HANDLE file)
{
    FILE_STORAGE_INFO storage{};
    if (GetFileInformationByHandleEx(file, FileStorageInfo, &storage, sizeof(storage))) {
        return std::max<DWORD>(4096, storage.LogicalBytesPerSector);
    }
    return 4096;
}

class WindowsByteSource final : public IByteSource {
public:
    WindowsByteSource(UniqueHandle handle, std::uint64_t size, std::size_t alignment)
        : handle_(std::move(handle)), size_(size), alignment_(alignment)
    {
    }

    std::expected<std::size_t, IoError> read(std::uint64_t offset, std::span<std::byte> output) override
    {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD transferred = 0;
        const auto request = static_cast<DWORD>(std::min<std::size_t>(output.size(), 1u << 30));
        if (!ReadFile(handle_.get(), output.data(), request, &transferred, &position)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return 0;
            }
            return std::unexpected(error_from_win32(error, "ReadFile"));
        }
        return transferred;
    }

    std::uint64_t size() const override { return size_; }
    std::size_t alignment() const override { return alignment_; }

private:
    UniqueHandle handle_;
    std::uint64_t size_;
    std::size_t alignment_;
};

class WindowsFileSystemAccess final : public IFileSystemAccess {
public:
    std::expected<std::vector<DirEntry>, IoError> list(const std::filesystem::path& directory) override
    {
        WIN32_FIND_DATAW data{};
        const auto pattern = long_path(directory) + L"\\*";
        const HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (search == INVALID_HANDLE_VALUE) {
            return std::unexpected(error_from_win32(GetLastError(), "FindFirstFileExW"));
        }

        std::vector<DirEntry> entries;
        do {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            DirEntry entry{};
            entry.native_name = std::filesystem::path(name);
            entry.attrs = data.dwFileAttributes;
            entry.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            entry.mtime_ns = filetime_to_unix_ns(data.ftLastWriteTime);
            entry.is_mount_point = false;
            const bool directory_attribute = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.type = directory_attribute ? EntryType::Directory : EntryType::File;

            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                IsReparseTagNameSurrogate(data.dwReserved0)) {
                entry.type = EntryType::Link;
                if (data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT) {
                    const auto reparse = read_reparse_data(directory / name);
                    if (reparse && is_volume_mount_point(*reparse)) {
                        entry.type = EntryType::Directory;
                        entry.is_mount_point = true;
                    }
                }
            }
            if (entry.type != EntryType::File) {
                entry.size = 0;
            }
            entries.push_back(std::move(entry));
        } while (FindNextFileW(search, &data));

        const DWORD last_error = GetLastError();
        FindClose(search);
        if (last_error != ERROR_NO_MORE_FILES) {
            return std::unexpected(error_from_win32(last_error, "FindNextFileW"));
        }
        return entries;
    }

    std::expected<std::unique_ptr<IByteSource>, IoError> open(
        const std::filesystem::path& file, ReadOptions options) override
    {
        DWORD flags = 0;
        if (options.sequential) {
            flags |= FILE_FLAG_SEQUENTIAL_SCAN;
        }
        if (options.uncached) {
            flags |= FILE_FLAG_NO_BUFFERING;
        }
        UniqueHandle handle(CreateFileW(long_path(file).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags,
            nullptr));
        if (!handle.valid()) {
            return std::unexpected(error_from_win32(GetLastError(), "CreateFileW"));
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle.get(), &size)) {
            return std::unexpected(error_from_win32(GetLastError(), "GetFileSizeEx"));
        }
        const std::size_t alignment = options.uncached ? alignment_of(handle.get()) : 1;
        return std::make_unique<WindowsByteSource>(
            std::move(handle), static_cast<std::uint64_t>(size.QuadPart), alignment);
    }

    std::expected<std::string, IoError> read_link(const std::filesystem::path& link) override
    {
        const auto reparse = read_reparse_data(link);
        if (!reparse) {
            return std::unexpected(reparse.error());
        }
        return canonical_link_target(*reparse);
    }

    // Uncached reads leave nothing in the cache to drop.
    std::expected<void, IoError> drop_cache(const std::filesystem::path&) override { return {}; }
};

} // namespace

std::unique_ptr<IFileSystemAccess> make_file_system_access()
{
    return std::make_unique<WindowsFileSystemAccess>();
}

} // namespace va::platform
