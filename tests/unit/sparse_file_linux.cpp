// A sparse sample file on Linux: the tail is written after the hole, which
// the filesystem leaves unallocated where it keeps holes (ext4, xfs, btrfs,
// tmpfs); SEEK_DATA then finds the first data past offset 0.
#include "sparse_file.h"

#include <fcntl.h>
#include <unistd.h>

namespace test {

SparseFile make_sparse_file(const std::filesystem::path& path, std::uint64_t hole, std::string_view tail)
{
    SparseFile result;
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return result;
    }
    const auto written = ::pwrite(descriptor, tail.data(), tail.size(), static_cast<off_t>(hole));
    const auto first_data = ::lseek(descriptor, 0, SEEK_DATA);
    ::close(descriptor);
    result.created = written >= 0 && static_cast<std::size_t>(written) == tail.size();
    result.sparse = result.created && first_data > 0;
    return result;
}

} // namespace test
