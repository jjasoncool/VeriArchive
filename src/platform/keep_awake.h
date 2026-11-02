#pragma once
// While a long operation runs, the computer does not go to sleep on its own
// (decision 0043): a FULL verification reads for hours, and a sleep in the
// middle of it pauses it at best; a device on USB may not come back as it
// was, which interrupts the run (decision 0034).
#include <memory>
#include <string>

namespace va::platform {

// While it lives, the computer does not sleep on its own, without elevation
// (R-08). Windows: a power request that the system is required
// (PowerCreateRequest, PowerSetRequest with PowerRequestSystemRequired),
// named `reason`, which `powercfg /requests` lists. Linux: a logind inhibitor
// lock of sleep in block mode (Inhibit on the system bus), held by "VeriArchive"
// for `reason`, which `systemd-inhibit --list` lists. The display may still
// turn off and the session lock. Where the request cannot be made
// (Linux without logind or a system bus, as in a container), the operation
// runs as before: held() is false, and nothing is said.
class KeepAwake {
public:
    explicit KeepAwake(const std::string& reason);
    ~KeepAwake();
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    bool held() const { return held_ != nullptr; }

private:
    struct Held;
    std::unique_ptr<Held> held_;
};

} // namespace va::platform
