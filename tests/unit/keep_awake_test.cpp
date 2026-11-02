// A long operation of the window keeps the computer awake while it runs
// (decision 0043). On Windows the request is made for any process, without
// elevation, and the system's execution state shows it; on Linux it needs
// logind on the system bus, which a CI container lacks, so there the test makes
// and releases it, held or not, as an operation does.
#include "test_support.h"

#include "app/keep_awake.h"
#include "app/text.h"
#include "platform/keep_awake.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#endif

namespace {

#ifdef _WIN32
// Whether the system counts a request that it is required, from this or any
// other program (CallNtPowerInformation needs no elevation).
bool system_required()
{
    ULONG state = 0;
    return CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof state) == 0 &&
           (state & ES_SYSTEM_REQUIRED) != 0;
}
#endif

void held_where_the_system_makes_it([[maybe_unused]] const va::platform::KeepAwake& awake)
{
#ifdef _WIN32
    CHECK(awake.held());
    CHECK(system_required());
#endif
}

} // namespace

void keep_awake_tests()
{
    using va::app::Language;
    using va::app::Text;

#ifdef _WIN32
    const bool required_before = system_required();
#endif
    {
        const auto awake = va::app::keep_awake(Text(Language::TraditionalChinese));
        held_where_the_system_makes_it(awake);
        // A second at once, as the operations of two windows hold.
        const va::platform::KeepAwake second("VeriArchive test");
        held_where_the_system_makes_it(second);
    }
#ifdef _WIN32
    // Released with the last of them, unless another program holds one too.
    if (!required_before) {
        CHECK(!system_required());
    }
#endif
    // Made again once the others went away.
    const auto again = va::app::keep_awake(Text());
    held_where_the_system_makes_it(again);

    // The reason is the system's list's, in the interface's language.
    CHECK(Text()(va::app::keep_awake_reason) == "A long operation is running");
    CHECK(Text(Language::TraditionalChinese)(va::app::keep_awake_reason) == "長時間作業執行中");
}
