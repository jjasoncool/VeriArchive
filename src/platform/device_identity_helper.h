#pragma once
// Factory for the platform's device identity reader (IDeviceIdentityHelper in
// core/interfaces.h, decision 0047). On Windows it starts
// veriarchive-device-identity.exe from the program's own folder, which
// Windows starts only once the operator allows it administrator permission;
// on Linux there is none (NullDeviceIdentityHelper). Only the window makes
// one, and only reads with it on the operator's request: the command line
// never asks for administrator permission.
#include "core/interfaces.h"

#include <memory>

namespace va::platform {

std::unique_ptr<IDeviceIdentityHelper> make_device_identity_helper();

} // namespace va::platform
