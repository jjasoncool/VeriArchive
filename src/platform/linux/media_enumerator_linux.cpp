// Placeholder until the Linux enumerator (mountinfo + udev + sysfs) lands.
#include "platform/media_enumerator.h"

namespace va::platform {
namespace {

class LinuxMediaEnumerator final : public IMediaEnumerator {
public:
    std::expected<std::vector<VolumeInfo>, IoError> volumes() override
    {
        return std::unexpected(IoError{IoErrorKind::NotSupported, 0, "not implemented yet"});
    }

    std::expected<VolumeInfo, IoError> volume_for_path(const std::filesystem::path&) override
    {
        return std::unexpected(IoError{IoErrorKind::NotSupported, 0, "not implemented yet"});
    }
};

} // namespace

std::unique_ptr<IMediaEnumerator> make_media_enumerator()
{
    return std::make_unique<LinuxMediaEnumerator>();
}

} // namespace va::platform
