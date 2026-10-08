// Linux file access for scanning and hashing.
//  - Listing uses fstatat(AT_SYMLINK_NOFOLLOW), so symlinks are never followed
//    (R-46); mount points come from mountinfo, not st_dev (R-30).
//  - Reads drop the page cache before and after each file with posix_fadvise so
//    the bytes come from the medium (R-100).
//  - FIFOs, sockets and device nodes are reported as Special and never opened.
#include "platform/file_system.h"
#include "platform/linux/mountinfo.h"
#include "platform/linux/posix_util.h"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace va::platform {
namespace {

using posix::UniqueFd;
using posix::error_from_errno;

class LinuxByteSource final : public IByteSource {
public:
    LinuxByteSource(UniqueFd descriptor, std::uint64_t size, bool drop_cache)
        : descriptor_(std::move(descriptor)), size_(size), drop_cache_(drop_cache)
    {
    }

    ~LinuxByteSource() override
    {
        if (drop_cache_) {
            ::posix_fadvise(descriptor_.get(), 0, 0, POSIX_FADV_DONTNEED);
        }
    }

    std::expected<std::size_t, IoError> read(std::uint64_t offset, std::span<std::byte> output) override
    {
        for (;;) {
            const auto count = ::pread(descriptor_.get(), output.data(), output.size(),
                static_cast<off_t>(offset));
            if (count >= 0) {
                return static_cast<std::size_t>(count);
            }
            if (errno != EINTR) {
                return std::unexpected(error_from_errno(errno, "pread"));
            }
        }
    }

    std::uint64_t size() const override { return size_; }
    std::size_t alignment() const override { return 1; }

private:
    UniqueFd descriptor_;
    std::uint64_t size_;
    bool drop_cache_;
};

class LinuxFileSystemAccess final : public IFileSystemAccess {
public:
    LinuxFileSystemAccess()
    {
        for (const auto& mount : linux_mounts::read_mountinfo()) {
            mount_points_.insert(mount.mount_point.lexically_normal());
        }
    }

    std::expected<std::vector<DirEntry>, IoError> list(const std::filesystem::path& directory) override
    {
        UniqueFd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!directory_fd.valid()) {
            return std::unexpected(error_from_errno(errno, "open directory"));
        }
        DIR* stream = ::fdopendir(directory_fd.get());
        if (!stream) {
            return std::unexpected(error_from_errno(errno, "fdopendir"));
        }
        const int descriptor = directory_fd.release(); // now owned by `stream`

        std::vector<DirEntry> entries;
        std::expected<std::vector<DirEntry>, IoError> failure;
        errno = 0;
        while (const dirent* item = ::readdir(stream)) {
            const std::string name(item->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            struct stat information {};
            if (::fstatat(descriptor, item->d_name, &information, AT_SYMLINK_NOFOLLOW) != 0) {
                failure = std::unexpected(error_from_errno(errno, "fstatat"));
                break;
            }
            DirEntry entry{};
            entry.native_name = std::filesystem::path(name);
            entry.size = 0;
            entry.mtime_ns = static_cast<std::int64_t>(information.st_mtim.tv_sec) * 1000000000LL +
                             information.st_mtim.tv_nsec;
            entry.mode = static_cast<std::uint32_t>(information.st_mode & 07777);
            entry.is_mount_point = false;
            if (S_ISREG(information.st_mode)) {
                entry.type = EntryType::File;
                entry.size = static_cast<std::uint64_t>(information.st_size);
            } else if (S_ISDIR(information.st_mode)) {
                entry.type = EntryType::Directory;
                entry.is_mount_point =
                    mount_points_.contains((directory / name).lexically_normal());
            } else if (S_ISLNK(information.st_mode)) {
                entry.type = EntryType::Link;
            } else {
                entry.type = EntryType::Special;
            }
            entries.push_back(std::move(entry));
            errno = 0;
        }
        const int read_error = errno;
        ::closedir(stream);
        if (!failure) {
            return failure;
        }
        if (read_error != 0) {
            return std::unexpected(error_from_errno(read_error, "readdir"));
        }
        return entries;
    }

    std::expected<std::unique_ptr<IByteSource>, IoError> open(
        const std::filesystem::path& file, ReadOptions options) override
    {
        UniqueFd descriptor(::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (!descriptor.valid()) {
            return std::unexpected(error_from_errno(errno, "open"));
        }
        struct stat information {};
        if (::fstat(descriptor.get(), &information) != 0) {
            return std::unexpected(error_from_errno(errno, "fstat"));
        }
        if (!S_ISREG(information.st_mode)) {
            return std::unexpected(IoError{IoErrorKind::Other, 0, "not a regular file"});
        }
        // Back to blocking reads now that it is known to be a regular file.
        ::fcntl(descriptor.get(), F_SETFL, 0);
        if (options.uncached) {
            ::posix_fadvise(descriptor.get(), 0, 0, POSIX_FADV_DONTNEED);
        }
        if (options.sequential) {
            ::posix_fadvise(descriptor.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
        }
        return std::make_unique<LinuxByteSource>(std::move(descriptor),
            static_cast<std::uint64_t>(information.st_size), options.uncached);
    }

    // Decision 0008: the target bytes exactly as stored ('\' is an ordinary character).
    std::expected<std::string, IoError> read_link(const std::filesystem::path& link) override
    {
        std::string target(256, '\0');
        for (;;) {
            const auto length = ::readlink(link.c_str(), target.data(), target.size());
            if (length < 0) {
                return std::unexpected(error_from_errno(errno, "readlink"));
            }
            if (static_cast<std::size_t>(length) < target.size()) {
                target.resize(static_cast<std::size_t>(length));
                return target;
            }
            target.resize(target.size() * 2);
        }
    }

    std::expected<void, IoError> drop_cache(const std::filesystem::path& file) override
    {
        UniqueFd descriptor(::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (!descriptor.valid()) {
            return std::unexpected(error_from_errno(errno, "open"));
        }
        ::posix_fadvise(descriptor.get(), 0, 0, POSIX_FADV_DONTNEED);
        return {};
    }

private:
    std::set<std::filesystem::path> mount_points_;
};

} // namespace

std::unique_ptr<IFileSystemAccess> make_file_system_access()
{
    return std::make_unique<LinuxFileSystemAccess>();
}

} // namespace va::platform
