#include "core/write_file.h"

namespace va {

std::expected<void, IoError> write_file_atomically(
    IFileWriter& writer,
    const std::filesystem::path& destination,
    std::string_view contents)
{
    auto file = writer.create(destination);
    if (!file) {
        return std::unexpected(file.error());
    }
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    if (auto written = (*file)->write(bytes); !written) {
        return written;
    }
    return (*file)->commit();
}

} // namespace va
