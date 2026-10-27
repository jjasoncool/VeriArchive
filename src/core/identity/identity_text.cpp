#include "core/identity/identity_text.h"

#include "common/utf8.h"

namespace va {

VolumeInfo with_valid_identity_text(VolumeInfo volume)
{
    const auto make_valid = [](std::string& text) {
        if (!is_valid_utf8(text)) {
            text = utf8_lossy(text);
        }
    };
    for (auto* text : {&volume.filesystem_type, &volume.filesystem_uuid, &volume.filesystem_uuid_short,
             &volume.volume_label, &volume.volume_guid, &volume.container}) {
        make_valid(*text);
    }
    for (auto& device : volume.devices) {
        for (auto* text : {&device.device_ref, &device.model, &device.serial, &device.wwn, &device.bus}) {
            make_valid(*text);
        }
    }
    return volume;
}

} // namespace va
