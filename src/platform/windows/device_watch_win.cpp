// The device watch on Windows (decision 0053): a hidden top-level window on a
// thread of its own, to which Windows sends WM_DEVICECHANGE. Windows
// broadcasts a volume that got or lost its drive letter (DBT_DEVICEARRIVAL,
// DBT_DEVICEREMOVECOMPLETE) and a change of the device tree
// (DBT_DEVNODES_CHANGED) to every top-level window, not to a message-only
// one; a volume without a drive letter is told only to a window that asked
// for volume interfaces (RegisterDeviceNotificationW), which this one does.
// Nothing needs elevation, and no device is opened.
#include "platform/device_watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>

#include <atomic>
#include <thread>
#include <utility>

namespace va::platform {
namespace {

constexpr wchar_t watch_window_class[] = L"VeriArchiveDeviceWatch";
// GUID_DEVINTERFACE_VOLUME, written out so that no header has to define it
// in this file.
constexpr GUID volume_interface{0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

} // namespace

struct DeviceWatch::Watching {
    std::function<void()> changed;
    std::atomic<HWND> window{nullptr};
    HANDLE ready = nullptr;
    std::thread thread;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM what, LPARAM detail)
    {
        if (message == WM_DEVICECHANGE &&
            (what == DBT_DEVNODES_CHANGED || what == DBT_DEVICEARRIVAL || what == DBT_DEVICEREMOVECOMPLETE)) {
            if (auto* watching = reinterpret_cast<Watching*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
                try {
                    watching->changed();
                } catch (...) {
                }
            }
            return TRUE;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, what, detail);
    }

    void run()
    {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = procedure;
        window_class.hInstance = instance;
        window_class.lpszClassName = watch_window_class;
        // A second watch of the process finds the class registered already.
        RegisterClassExW(&window_class);
        // Top-level and never shown: Windows broadcasts device changes to
        // such a window, not to a message-only one.
        const HWND made = CreateWindowExW(0, watch_window_class, L"VeriArchive", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
            nullptr, instance, nullptr);
        HDEVNOTIFY volumes = nullptr;
        if (made != nullptr) {
            SetWindowLongPtrW(made, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            DEV_BROADCAST_DEVICEINTERFACE_W filter{};
            filter.dbcc_size = sizeof(filter);
            filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            filter.dbcc_classguid = volume_interface;
            volumes = RegisterDeviceNotificationW(made, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        }
        window.store(made);
        SetEvent(ready);
        if (made == nullptr) {
            return;
        }
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            DispatchMessageW(&message);
        }
        if (volumes != nullptr) {
            UnregisterDeviceNotification(volumes);
        }
        window.store(nullptr);
    }
};

DeviceWatch::DeviceWatch(std::function<void()> changed)
{
    auto watching = std::make_unique<Watching>();
    watching->changed = std::move(changed);
    watching->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (watching->ready == nullptr) {
        return;
    }
    try {
        watching->thread = std::thread([state = watching.get()] { state->run(); });
    } catch (...) {
        CloseHandle(watching->ready);
        return;
    }
    WaitForSingleObject(watching->ready, INFINITE);
    watching_ = std::move(watching);
}

DeviceWatch::~DeviceWatch()
{
    if (!watching_) {
        return;
    }
    // The window's own thread destroys it and ends; a message it is handling
    // (a call of `changed`) finishes first.
    if (const HWND window = watching_->window.load()) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    if (watching_->thread.joinable()) {
        watching_->thread.join();
    }
    CloseHandle(watching_->ready);
}

bool DeviceWatch::watching() const
{
    return watching_ && watching_->window.load() != nullptr;
}

void* DeviceWatch::native_window() const
{
    return watching_ ? watching_->window.load() : nullptr;
}

} // namespace va::platform
