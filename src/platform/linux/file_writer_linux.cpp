// Linux durable writer: O_EXCL temporary file, write, fsync, rename over the
// destination, then fsync the directory so the rename itself is durable.
#include "platform/file_writer.h"
#include "platform/linux/posix_util.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace va::platform {
namespace {

using posix::UniqueFd;
using posix::error_from_errno;

std::atomic<unsigned> temporary_counter{0};

class LinuxOutputFile final : public IOutputFile {
public:
    LinuxOutputFile(UniqueFd descriptor, std::filesystem::path temporary,
        std::filesystem::path destination)
        : descriptor_(std::move(descriptor)),
          temporary_(std::move(temporary)),
          destination_(std::move(destination))
    {
    }

    ~LinuxOutputFile() override
    {
        if (!committed_) {
            descriptor_.reset();
            ::unlink(temporary_.c_str());
        }
    }

    std::expected<void, IoError> write(std::span<const std::byte> data) override
    {
        while (!data.empty()) {
            const auto written = ::write(descriptor_.get(), data.data(), data.size());
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(error_from_errno(errno, "write"));
            }
            data = data.subspan(static_cast<std::size_t>(written));
        }
        return {};
    }

    std::expected<void, IoError> commit() override
    {
        if (::fsync(descriptor_.get()) != 0) {
            return std::unexpected(error_from_errno(errno, "fsync"));
        }
        descriptor_.reset();
        if (::rename(temporary_.c_str(), destination_.c_str()) != 0) {
            return std::unexpected(error_from_errno(errno, "rename"));
        }
        committed_ = true;

        auto directory = destination_.parent_path();
        if (directory.empty()) {
            directory = ".";
        }
        UniqueFd directory_descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (directory_descriptor.valid() && ::fsync(directory_descriptor.get()) != 0) {
            return std::unexpected(error_from_errno(errno, "fsync directory"));
        }
        return {};
    }

private:
    UniqueFd descriptor_;
    std::filesystem::path temporary_;
    std::filesystem::path destination_;
    bool committed_ = false;
};

class LinuxFileWriter final : public IFileWriter {
public:
    std::expected<std::unique_ptr<IOutputFile>, IoError> create(
        const std::filesystem::path& destination) override
    {
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto temporary = destination;
            temporary += ".tmp-" + std::to_string(::getpid()) + "-" +
                         std::to_string(temporary_counter++);
            UniqueFd descriptor(
                ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
            if (descriptor.valid()) {
                return std::make_unique<LinuxOutputFile>(
                    std::move(descriptor), std::move(temporary), destination);
            }
            if (errno != EEXIST) {
                return std::unexpected(error_from_errno(errno, "open"));
            }
        }
        return std::unexpected(IoError{IoErrorKind::Other, 0, "cannot create temporary file"});
    }

    std::expected<void, IoError> remove(const std::filesystem::path& file) override
    {
        if (::unlink(file.c_str()) != 0) {
            return std::unexpected(error_from_errno(errno, "unlink"));
        }
        return {};
    }

    std::expected<void, IoError> create_directories(const std::filesystem::path& directory) override
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return std::unexpected(error_from_errno(error.value(), "create_directories"));
        }
        return {};
    }
};

} // namespace

std::unique_ptr<IFileWriter> make_file_writer()
{
    return std::make_unique<LinuxFileWriter>();
}

} // namespace va::platform
