// Reading the attached devices at once when the system tells of a change
// (decision 0053): when the window reads after the changes it was told of
// (DeviceChanges: once they have been quiet for a moment, at the latest a
// little after the first, and again for a change told later), and the
// platform's watch, which starts, tells nothing by itself and ends without
// calling back afterwards. What Windows sends is in device_watch_win_test.cpp.
#include "test_support.h"

#include "gui/device_changes.h"
#include "platform/device_watch.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using va::gui::DeviceChanges;

void changes_are_read_once_quiet()
{
    const auto start = DeviceChanges::Clock::time_point{} + 1h;
    DeviceChanges changes;
    // Nothing told: nothing waits, nothing is due.
    CHECK(!changes.waiting() && !changes.due(start));

    // One change: due once it has been quiet for device_changes_quiet.
    changes.told(start);
    CHECK(changes.waiting() && !changes.due(start) && !changes.due(start + 499ms));
    CHECK(changes.due(start + 500ms));

    // Several within a moment, as one device attached is told: one read,
    // after the last of them has been quiet.
    changes.told(start + 200ms);
    changes.told(start + 400ms);
    CHECK(!changes.due(start + 800ms) && changes.due(start + 900ms));
    changes.taken();
    CHECK(!changes.waiting() && !changes.due(start + 10s));

    // A stream of changes cannot put the read off for good: it comes
    // device_changes_at_most after the first.
    for (auto at = 0ms; at <= 2000ms; at += 100ms) {
        changes.told(start + at);
        CHECK(changes.due(start + at) == (at >= 2000ms));
    }
    changes.taken();

    // A change told after the read asks for another one.
    changes.told(start + 5s);
    CHECK(changes.waiting() && !changes.due(start + 5s) && changes.due(start + 5s + 500ms));
    CHECK(va::gui::device_changes_quiet == 500ms && va::gui::device_changes_at_most == 2000ms);
}

void the_watch_starts_and_ends()
{
    std::atomic<int> told{0};
    const auto started = std::chrono::steady_clock::now();
    {
        const va::platform::DeviceWatch watch([&told] { ++told; });
        CHECK(watch.watching());
#ifndef _WIN32
        CHECK(watch.native_window() == nullptr);
#endif
        std::this_thread::sleep_for(200ms);
    }
    // It ended at once, and tells nothing once it is gone. (A device attached
    // to this computer during the test may be told while it lives.)
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    const int after = told.load();
    std::this_thread::sleep_for(100ms);
    CHECK(told.load() == after);

    // Two at once, as two windows of one process: each has its own.
    const va::platform::DeviceWatch first([] {});
    const va::platform::DeviceWatch second([] {});
    CHECK(first.watching() && second.watching());
}

} // namespace

void device_watch_tests()
{
    changes_are_read_once_quiet();
    the_watch_starts_and_ends();
#ifdef _WIN32
    device_watch_windows_tests();
#endif
}
