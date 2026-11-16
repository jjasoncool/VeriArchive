#pragma once
// Tells the window that the attached devices or volumes may have changed
// (decision 0053), so that it reads them again at once in place of waiting
// for its next timed read (decision 0046). It only says that something
// changed, never what: the window reads the devices as before
// (IMediaEnumerator), and its timed reads stay, for a change this misses.
// Without elevation (R-08, R-09). The core never uses it.
#include <functional>
#include <memory>

namespace va::platform {

// While it lives, `changed` is called, on a thread of its own, each time the
// system tells of a change: Windows, WM_DEVICECHANGE to a hidden top-level
// window of its own (a device or a volume arrived or was removed, or the
// device tree changed); Linux, a change of the mounted filesystems
// (/proc/self/mountinfo, the list the volumes are read from). One change of
// the devices is often told several times; the caller reads once for them.
// `changed` must not throw, and is not called once the destructor has
// returned. Where the system cannot be listened to, watching() is false and
// nothing is ever told.
class DeviceWatch {
public:
    explicit DeviceWatch(std::function<void()> changed);
    ~DeviceWatch();
    DeviceWatch(const DeviceWatch&) = delete;
    DeviceWatch& operator=(const DeviceWatch&) = delete;

    bool watching() const;
    // For the tests on Windows: the hidden window (a HWND), to which they
    // send what Windows sends; null elsewhere, and where it is not watching.
    void* native_window() const;

private:
    struct Watching;
    std::unique_ptr<Watching> watching_;
};

} // namespace va::platform
