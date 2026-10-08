// Windows durable writer: CREATE_NEW temporary file, WriteFile, FlushFileBuffers,
// then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) over the destination.
#include "platform/file_writer.h"
#include "platform/windows/win_util.h"

#include <atomic>

namespace va::platform {
namespace {

using windows::UniqueHandle;
using windows::error_from_win32;
using windows::long_path;

std::atomic<unsigned> temporary_counter{0};

class WindowsOutputFile final : public IOutputFile {
public:
    WindowsOutputFile(UniqueHandle handle, std::wstring temporary, std::wstring destination)
        : handle_(std::move(handle)),
          temporary_(std::move(temporary)),
          destination_(std::move(destination))
    {
    }

    ~WindowsOutputFile() override
    {
        if (!committed_) {
            handle_.reset();
            DeleteFileW(temporary_.c_str());
        }
    }

    std::expected<void, IoError> write(std::span<const std::byte> data) override
    {
        while (!data.empty()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
            DWORD written = 0;
            if (!WriteFile(handle_.get(), data.data(), chunk, &written, nullptr)) {
                return std::unexpected(error_from_win32(GetLastError(), "WriteFile"));
            }
            data = data.subspan(written);
        }
        return {};
    }

    std::expected<void, IoError> commit() override
    {
        if (!FlushFileBuffers(handle_.get())) {
            return std::unexpected(error_from_win32(GetLastError(), "FlushFileBuffers"));
        }
        handle_.reset();
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return std::unexpected(error_from_win32(GetLastError(), "MoveFileExW"));
        }
        committed_ = true;
        return {};
    }

private:
    UniqueHandle handle_;
    std::wstring temporary_;
    std::wstring destination_;
    bool committed_ = false;
};

class WindowsFileWriter final : public IFileWriter {
public:
    std::expected<std::unique_ptr<IOutputFile>, IoError> create(
        const std::filesystem::path& destination) override
    {
        const auto target = long_path(destination);
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto temporary = target + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                                   L"-" + std::to_wstring(temporary_counter++);
            UniqueHandle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (handle.valid()) {
                return std::make_unique<WindowsOutputFile>(std::move(handle), temporary, target);
            }
            if (GetLastError() != ERROR_FILE_EXISTS) {
                return std::unexpected(error_from_win32(GetLastError(), "CreateFileW"));
            }
        }
        return std::unexpected(IoError{IoErrorKind::Other, 0, "cannot create temporary file"});
    }

    std::expected<void, IoError> remove(const std::filesystem::path& file) override
    {
        if (!DeleteFileW(long_path(file).c_str())) {
            return std::unexpected(error_from_win32(GetLastError(), "DeleteFileW"));
        }
        return {};
    }

    std::expected<void, IoError> create_directories(const std::filesystem::path& directory) override
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return std::unexpected(error_from_win32(static_cast<DWORD>(error.value()), "create_directories"));
        }
        return {};
    }
};

} // namespace

std::unique_ptr<IFileWriter> make_file_writer()
{
    return std::make_unique<WindowsFileWriter>();
}

} // namespace va::platform
