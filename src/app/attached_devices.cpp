#include "app/attached_devices.h"

#include <map>
#include <string>
#include <utility>

namespace va::app {

std::vector<AttachedDevice> devices_of(const std::vector<VolumeInfo>& volumes)
{
    std::vector<AttachedDevice> devices;
    std::map<std::string, std::size_t> named;  // device_ref: its index in `devices`
    for (std::size_t index = 0; index < volumes.size(); ++index) {
        for (const auto& device : volumes[index].devices) {
            if (!device.device_ref.empty()) {
                if (const auto known = named.find(device.device_ref); known != named.end()) {
                    auto& on = devices[known->second].volumes;
                    if (on.back() != index) {
                        on.push_back(index);
                    }
                    continue;
                }
                named.emplace(device.device_ref, devices.size());
            }
            devices.push_back({device, {index}});
        }
    }
    return devices;
}

std::expected<AttachedDevices, IoError> attached_devices(IMediaEnumerator& media)
{
    auto volumes = media.volumes();
    if (!volumes) {
        return std::unexpected(volumes.error());
    }
    // Once for a device that holds several volumes (decision 0044). A device
    // without a system name cannot be asked (each platform says Unknown at
    // once), nor told to be another volume's.
    std::map<std::string, DeviceHealth> asked;
    for (auto& volume : *volumes) {
        for (auto& device : volume.devices) {
            if (const auto known = asked.find(device.device_ref); !device.device_ref.empty() && known != asked.end()) {
                device.health = known->second;
                continue;
            }
            const auto health = media.device_health(device);
            device.health = health ? *health : DeviceHealth{};
            if (!device.device_ref.empty()) {
                asked.emplace(device.device_ref, *device.health);
            }
        }
    }
    AttachedDevices attached;
    attached.devices = devices_of(*volumes);
    attached.volumes = std::move(*volumes);
    return attached;
}

} // namespace va::app
