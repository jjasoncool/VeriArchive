// Linux durable writer: O_EXCL temporary file, write, fsync, rename over the
// destination, then fsync the directory so the rename itself is durable. A
// flush() ahead of the commit makes the data durable, and leaves the commit
// only the rename (decision 0023).
//
// commit_new() names the file only where nothing of the destination's name is
// there (decision 0048): renameat2 with RENAME_NOREPLACE, which refuses an
// existing name in the same call; where the file system refuses that flag, a
// hard link of the temporary file under the destination's name, which refuses
// one as well, then the temporary name removed; where it has no hard links
// either, a look for the name and then the rename (see name_new).
#include "platform/file_writer.h"
#include "platform/linux/posix_util.h"

#include <atomic>
#include <cerrno>
#include <cstdio> // renameat2 and RENAME_NOREPLACE (glibc 2.28, _GNU_SOURCE)
#include <fcntl.h>
#include <sys/stat.h>
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
            discard();
        }
    }

    std::expected<void, IoError> write(std::span<const std::byte> data) override
    {
        if (!data.empty()) {
            flushed_ = false;
        }
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

    std::expected<void, IoError> flush() override
    {
        if (::fsync(descriptor_.get()) != 0) {
            return std::unexpected(error_from_errno(errno, "fsync"));
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
        descriptor_.reset();
        if (::rename(temporary_.c_str(), destination_.c_str()) != 0) {
            return std::unexpected(error_from_errno(errno, "rename"));
        }
        committed_ = true;
        return sync_directory();
    }

    std::expected<void, IoError> commit_new() override
    {
        if (!flushed_) {
            if (auto flushed = flush(); !flushed) {
                return flushed;
            }
        }
        descriptor_.reset();
        if (auto named = name_new(); !named) {
            if (named.error().kind == IoErrorKind::AlreadyExists) {
                discard();
            }
            return named;
        }
        committed_ = true;
        return sync_directory();
    }

private:
    // Gives the temporary file the destination's name where nothing of that
    // name is there; AlreadyExists where something is.
    std::expected<void, IoError> name_new()
    {
        if (::renameat2(AT_FDCWD, temporary_.c_str(), AT_FDCWD, destination_.c_str(), RENAME_NOREPLACE) == 0) {
            return {};
        }
        if (const int code = errno; code != EINVAL && code != ENOSYS && code != EOPNOTSUPP) {
            return std::unexpected(error_from_errno(code, "renameat2"));
        }
        // The file system takes no RENAME_NOREPLACE (a FUSE program or a
        // network file system, NFS among them, may not): a hard link under
        // the destination's name refuses an existing name too. Once it is
        // made, the file has its name; the temporary name is removed (where
        // that fails, it is left beside it as a second name of the same file).
        if (::link(temporary_.c_str(), destination_.c_str()) == 0) {
            ::unlink(temporary_.c_str());
            return {};
        }
        const int not_linked = errno;
        // NFS: a LINK sent again after its reply was lost says EEXIST although
        // the first one made the name (link(2); open(2) under O_EXCL). The
        // name is this file's where it names the temporary file itself.
        if (not_linked == EEXIST && destination_names_temporary()) {
            ::unlink(temporary_.c_str());
            return {};
        }
        if (not_linked != EPERM && not_linked != EOPNOTSUPP && not_linked != ENOSYS) {
            return std::unexpected(error_from_errno(not_linked, "link"));
        }
        // No hard links either (a FUSE program that offers neither, such as
        // exFAT's through FUSE): the name is looked for, then the file
        // renamed. This is the one narrow window left: a file that another
        // program makes under that name between the look and the rename is
        // replaced.
        struct stat there {};
        if (::lstat(destination_.c_str(), &there) == 0) {
            return std::unexpected(error_from_errno(EEXIST, "rename"));
        }
        if (const int code = errno; code != ENOENT) {
            return std::unexpected(error_from_errno(code, "lstat"));
        }
        if (::rename(temporary_.c_str(), destination_.c_str()) != 0) {
            return std::unexpected(error_from_errno(errno, "rename"));
        }
        return {};
    }

    // Whether the destination's name names the temporary file itself: the
    // same device and inode, which an attribute cache cannot make stale.
    bool destination_names_temporary() const
    {
        struct stat temporary {};
        struct stat named {};
        return ::lstat(temporary_.c_str(), &temporary) == 0 && ::lstat(destination_.c_str(), &named) == 0 &&
               temporary.st_dev == named.st_dev && temporary.st_ino == named.st_ino;
    }

    // Makes the new name durable: fsync of the destination's directory.
    std::expected<void, IoError> sync_directory() const
    {
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

    // Closes the temporary file and removes it, once.
    void discard()
    {
        descriptor_.reset();
        if (!discarded_) {
            ::unlink(temporary_.c_str());
            discarded_ = true;
        }
    }

    UniqueFd descriptor_;
    std::filesystem::path temporary_;
    std::filesystem::path destination_;
    bool flushed_ = false;   // flush() made every byte written durable
    bool committed_ = false;
    bool discarded_ = false; // the temporary file was removed
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
