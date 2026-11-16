#pragma once
// When the window reads the attached devices again after the system told of
// a change (decision 0053; platform/device_watch.h). One device attached or
// removed is told several times within a moment (the device, its volume, the
// device tree), and a volume is mounted a little after its device arrives:
// the window reads once the changes have been quiet for a short while, and
// at the latest a little after the first of them, so that a stream of
// changes cannot put the read off for good. A change told later asks for
// another read.
//
// Plain C++ without GTK, so the unit tests can check it.
#include <chrono>
#include <optional>

namespace va::gui {

// How long the changes must have been quiet before the read, and how long
// after the first change the read comes at the latest.
inline constexpr std::chrono::milliseconds device_changes_quiet{500};
inline constexpr std::chrono::milliseconds device_changes_at_most{2000};

class DeviceChanges {
public:
    using Clock = std::chrono::steady_clock;

    // The system told of a change at `now`.
    void told(Clock::time_point now)
    {
        if (!first_) {
            first_ = now;
        }
        last_ = now;
    }

    // Whether changes wait for their read.
    bool waiting() const { return first_.has_value(); }

    // Whether the read is due at `now`: changes wait, and they have been
    // quiet for device_changes_quiet, or the first is device_changes_at_most
    // old.
    bool due(Clock::time_point now) const
    {
        return first_ && (now - *last_ >= device_changes_quiet || now - *first_ >= device_changes_at_most);
    }

    // The read was started (or is not wanted: no page shows the devices):
    // nothing waits until the next change.
    void taken()
    {
        first_.reset();
        last_.reset();
    }

private:
    std::optional<Clock::time_point> first_;
    std::optional<Clock::time_point> last_;
};

} // namespace va::gui
