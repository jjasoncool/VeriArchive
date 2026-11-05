// Linux has no device identity reader (decision 0047): reading a device's own
// identity behind its enclosure would take a SCSI command through SG_IO,
// which needs root or CAP_SYS_RAWIO, and no program of VeriArchive asks for
// either. The window offers no read there.
#include "platform/device_identity_helper.h"

namespace va::platform {

std::unique_ptr<IDeviceIdentityHelper> make_device_identity_helper()
{
    return std::make_unique<NullDeviceIdentityHelper>();
}

} // namespace va::platform
