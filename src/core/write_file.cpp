#include "core/write_file.h"

namespace va {
namespace {

enum class Naming { Replace, New };

std::expected<void, IoError> write_whole_file(
    IFileWriter& writer, const std::filesystem::path& destination, std::string_view contents, Naming naming)
{
    auto file = writer.create(destination);
    if (!file) {
        return std::unexpected(file.error());
    }
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    if (auto written = (*file)->write(bytes); !written) {
        return written;
    }
    return naming == Naming::New ? (*file)->commit_new() : (*file)->commit();
}

} // namespace

std::expected<void, IoError> write_file_atomically(
    IFileWriter& writer,
    const std::filesystem::path& destination,
    std::string_view contents)
{
    return write_whole_file(writer, destination, contents, Naming::Replace);
}

std::expected<void, IoError> write_new_file_atomically(
    IFileWriter& writer,
    const std::filesystem::path& destination,
    std::string_view contents)
{
    return write_whole_file(writer, destination, contents, Naming::New);
}

std::expected<std::string, IoError> read_whole_file(
    IFileSystemAccess& files, const std::filesystem::path& path)
{
    auto source = files.open(path, ReadOptions{false, true});
    if (!source) {
        return std::unexpected(source.error());
    }
    std::string bytes;
    std::vector<std::byte> buffer(1 << 20);
    for (std::uint64_t offset = 0;;) {
        const auto count = (*source)->read(offset, buffer);
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count == 0) {
            return bytes;
        }
        bytes.append(reinterpret_cast<const char*>(buffer.data()), *count);
        offset += *count;
    }
}

} // namespace va
