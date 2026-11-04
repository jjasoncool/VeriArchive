// How the window words a job's progress for its bar (src/gui/progress_wording.*):
// the bytes of a manifest copied by an export (R-94, decision 0046), which
// count no items and need a bar of their own rather than the pulsing bar of a
// scan; the entries of a comparison; the files of a registration or
// verification; and at most five updates a second, the last one always.
// Every number below is sample data made up for the test.
#include "test_support.h"

#include "gui/progress_wording.h"

#include <chrono>
#include <cmath>

namespace {

using namespace std::chrono_literals;
using va::app::Language;
using va::app::Text;
using va::gui::Counting;
using va::gui::ProgressWording;

constexpr std::uint64_t mib = std::uint64_t{1} << 20;
const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::time_point{} + 1h;

bool near(double left, double right)
{
    return std::abs(left - right) < 1e-9;
}

// An export copies its manifest piece by piece; the bar shows how much of it,
// and ends full once it is all copied, even when the update comes at once.
void an_export_shows_the_bytes_copied()
{
    ProgressWording wording(Text{}, Counting::Bytes);
    auto bar = wording.update({0, 4 * mib, 0, 0, ""}, start);
    CHECK(bar && !bar->pulse && near(bar->fraction, 0.0) && bar->text == "Copying: 0 B of 4.00 MiB (0%)");
    bar = wording.update({mib, 4 * mib, 0, 0, ""}, start + 1s);
    CHECK(bar && !bar->pulse && near(bar->fraction, 0.25) && bar->text == "Copying: 1.00 MiB of 4.00 MiB (25%)");
    // Within a fifth of a second of the last: not shown, unless it completes.
    CHECK(!wording.update({2 * mib, 4 * mib, 0, 0, ""}, start + 1s + 50ms));
    // Not yet all of it: never 100 %.
    bar = wording.update({4 * mib - 1, 4 * mib, 0, 0, ""}, start + 2s);
    CHECK(bar && bar->text.ends_with("(99%)") && bar->fraction < 1.0);
    bar = wording.update({4 * mib, 4 * mib, 0, 0, ""}, start + 2s + 10ms);
    CHECK(bar && near(bar->fraction, 1.0) && bar->text == "Copying: 4.00 MiB of 4.00 MiB (100%)");

    ProgressWording chinese(Text(Language::TraditionalChinese), Counting::Bytes);
    bar = chinese.update({mib, 4 * mib, 0, 0, ""}, start);
    CHECK(bar && bar->text == "複製中：1.00 MiB / 4.00 MiB（25%）");
    // Of a file whose size is not known (nothing yet), the bar moves to and fro.
    ProgressWording unknown(Text{}, Counting::Bytes);
    bar = unknown.update({0, 0, 0, 0, ""}, start);
    CHECK(bar && bar->pulse && bar->text == "Copying: 0 B");
}

// The other countings as before: a scan pulses, a comparison counts entries.
void the_other_countings_stay()
{
    ProgressWording scanning(Text{}, Counting::Files);
    auto bar = scanning.update({0, 0, 12, 0, ""}, start);
    CHECK(bar && bar->pulse && bar->text == "Scanning: 12 entries");
    ProgressWording comparing(Text{}, Counting::Entries);
    bar = comparing.update({0, 0, 3, 4, ""}, start);
    CHECK(bar && !bar->pulse && near(bar->fraction, 0.75) && bar->text == "Comparing: 3 / 4 entries");
    ProgressWording hashing(Text{}, Counting::Files);
    bar = hashing.update({mib, 4 * mib, 1, 4, ""}, start);
    CHECK(bar && !bar->pulse && bar->text.starts_with("Hashing: 1.00 MiB of 4.00 MiB (25%)"));
}

} // namespace

void progress_wording_tests()
{
    an_export_shows_the_bytes_copied();
    the_other_countings_stay();
}
