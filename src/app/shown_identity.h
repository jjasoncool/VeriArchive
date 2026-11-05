#pragma once
// How a page shows the media identity and the computer that recorded a run
// (decisions 0046, 0048): each as recorded where it is left empty, as the
// command line and the window in normal use show them. The window's
// screenshot mode shows its fixed sample ones in their place
// (gui/displayed_identity.h: shown_identity), as its pictures are published.
//
// Plain C++ without GTK.
#include "common/types.h"

#include <functional>
#include <string>
#include <vector>

namespace va::app {

struct ShownIdentity {
    std::function<VolumeInfo(const VolumeInfo&)> volume;
    std::function<std::vector<PhysicalDeviceInfo>(const std::vector<PhysicalDeviceInfo>&)> devices;
    std::function<std::string(const std::string&)> host;

    VolumeInfo shown_volume(const VolumeInfo& recorded) const { return volume ? volume(recorded) : recorded; }
    std::vector<PhysicalDeviceInfo> shown_devices(const std::vector<PhysicalDeviceInfo>& recorded) const
    {
        return devices ? devices(recorded) : recorded;
    }
    std::string shown_host(const std::string& recorded) const { return host ? host(recorded) : recorded; }
};

} // namespace va::app
