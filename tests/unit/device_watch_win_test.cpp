// The device watch on Windows (decision 0053): its hidden window calls back
// for what Windows sends when a device or volume arrives or is removed
// (WM_DEVICECHANGE with DBT_DEVICEARRIVAL, DBT_DEVICEREMOVECOMPLETE or
// DBT_DEVNODES_CHANGED), sent here to the window as Windows sends it, and for
// nothing else; and it hears a real change of the volumes that needs no
// device: a drive letter made for a sample folder and removed again
// (DefineDosDeviceW, as `subst` makes one, without elevation). No device of
// this computer is touched. The folder and the drive letter are sample data.
#include "test_support.h"

#include "platform/device_watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Waits until `told` passes `more_than`, at most five seconds.
bool told_more(const std::atomic<int>& told, int more_than)
{
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (told.load() <= more_than) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// A drive letter that no volume has, from Z down; none where all are taken.
std::optional<wchar_t> free_drive_letter()
{
    const DWORD taken = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'H'; --letter) {
        if ((taken & (1u << (letter - L'A'))) == 0) {
            return letter;
        }
    }
    return std::nullopt;
}

void the_window_tells_what_windows_sends()
{
    std::atomic<int> told{0};
    const va::platform::DeviceWatch watch([&told] { ++told; });
    const auto window = static_cast<HWND>(watch.native_window());
    CHECK(watch.watching() && window != nullptr && IsWindow(window) && !IsWindowVisible(window));
    if (window == nullptr) {
        return;
    }
    // Top-level, as Windows broadcasts device changes only to such windows.
    CHECK(GetAncestor(window, GA_PARENT) == GetDesktopWindow());

    // SendMessageW returns once the window's thread has handled the message.
    // An arrival and a removal carry what arrived, as Windows sends it: here a
    // sample volume without a drive letter (Windows does not deliver them to
    // another thread's window without it).
    DEV_BROADCAST_VOLUME volume{};
    volume.dbcv_size = sizeof(volume);
    volume.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    const auto detail = reinterpret_cast<LPARAM>(&volume);
    int before = told.load();
    SendMessageW(window, WM_DEVICECHANGE, DBT_DEVNODES_CHANGED, 0);
    CHECK(told.load() >= before + 1);
    SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, detail);
    CHECK(told.load() >= before + 2);
    SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE, detail);
    CHECK(told.load() >= before + 3);
    // Not for a question before a removal, a change of configuration, or
    // another message.
    before = told.load();
    SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, detail);
    SendMessageW(window, WM_DEVICECHANGE, DBT_CONFIGCHANGED, 0);
    SendMessageW(window, WM_SETTINGCHANGE, 0, 0);
    // (A device attached to this computer during the test would count too.)
    CHECK(told.load() == before);
}

void a_drive_letter_made_and_removed_is_told()
{
    const auto letter = free_drive_letter();
    CHECK(letter.has_value());
    if (!letter) {
        return;
    }
    std::error_code error;
    const auto folder = std::filesystem::temp_directory_path(error) / L"VeriArchive-SAMPLE-device-watch";
    std::filesystem::create_directories(folder, error);
    const std::wstring drive{*letter, L':'};

    std::atomic<int> told{0};
    const va::platform::DeviceWatch watch([&told] { ++told; });
    const int before = told.load();
    const bool made = DefineDosDeviceW(0, drive.c_str(), folder.c_str()) != 0;
    CHECK(made);
    if (made) {
        CHECK(told_more(told, before));
        const int then = told.load();
        CHECK(DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, drive.c_str(), folder.c_str()) != 0);
        CHECK(told_more(told, then));
    }
    std::filesystem::remove_all(folder, error);
}

} // namespace

void device_watch_windows_tests()
{
    the_window_tells_what_windows_sends();
    a_drive_letter_made_and_removed_is_told();
}
