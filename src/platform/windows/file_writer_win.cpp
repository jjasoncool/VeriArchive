// Windows durable writer: CREATE_NEW temporary file, WriteFile, FlushFileBuffers,
// then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) over the destination. A
// flush() ahead of the commit makes the data durable, and leaves the commit
// only the rename (decision 0023). commit_new() renames with WRITE_THROUGH
// alone, which Windows refuses where anything of the destination's name is
// there, in the same call (decision 0048).
#include "platform/file_writer.h"
#include "platform/windows/long_path.h"
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
            discard();
        }
    }

    std::expected<void, IoError> write(std::span<const std::byte> data) override
    {
        if (!data.empty()) {
            flushed_ = false;
        }
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

    std::expected<void, IoError> flush() override
    {
        if (!FlushFileBuffers(handle_.get())) {
            return std::unexpected(error_from_win32(GetLastError(), "FlushFileBuffers"));
        }
        flushed_ = true;
        return {};
    }

    std::expected<void, IoError> commit() override
    {
        if (!flushed_) {
            if (auto flushed = flush(); !flushed) {
                return flushed;
            }
        }
        handle_.reset();
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return std::unexpected(error_from_win32(GetLastError(), "MoveFileExW"));
        }
        committed_ = true;
        return {};
    }

    std::expected<void, IoError> commit_new() override
    {
        if (!flushed_) {
            if (auto flushed = flush(); !flushed) {
                return flushed;
            }
        }
        handle_.reset();
        // No MOVEFILE_REPLACE_EXISTING: a file, a folder or a link of that
        // name, also one made after create(), fails the rename itself
        // (ERROR_ALREADY_EXISTS, or ERROR_FILE_EXISTS), which names nothing.
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            auto error = error_from_win32(GetLastError(), "MoveFileExW");
            if (error.kind == IoErrorKind::AlreadyExists) {
                discard();
            }
            return std::unexpected(std::move(error));
        }
        committed_ = true;
        return {};
    }

private:
    // Closes the temporary file and deletes it, once.
    void discard()
    {
        handle_.reset();
        if (!discarded_) {
            DeleteFileW(temporary_.c_str());
            discarded_ = true;
        }
    }

    UniqueHandle handle_;
    std::wstring temporary_;
    std::wstring destination_;
    bool flushed_ = false;   // flush() made every byte written durable
    bool committed_ = false;
    bool discarded_ = false; // the temporary file was deleted
};

class WindowsFileWriter final : public IFileWriter {
public:
    std::expected<std::unique_ptr<IOutputFile>, IoError> create(
        const std::filesystem::path& destination) override
    {
        const auto target = long_path(destination.native());
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
        if (!DeleteFileW(long_path(file.native()).c_str())) {
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
