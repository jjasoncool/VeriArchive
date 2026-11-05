#pragma once
// What the result page of one recorded run says (decision 0046), worded from
// what the repository records of it: its row of the history (VerifyRunRecord,
// its counts included), its items (verify_items), the media evidence recorded
// with it, and the archive's other runs, which a resumed run continues and a
// read is compared with. Never a bare status (R-21): the result keeps its
// mode; every time is local with its offset (decision 0017); the guarantee
// (R-02) is on every page. The one source of what the window's result page
// and the verification report of the run (decision 0048: run_report_html.h,
// the window's Save report... and `veriarchive-cli report`) say of it: each
// renders this model in its own form.
//
// The devices, the volume and the computer are shown as `ShownIdentity` says:
// as recorded on the command line and in the window's normal use; the window
// gives the sample ones of its screenshot mode (gui/displayed_identity.h).
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/run_words.h"
#include "app/shown_identity.h"
#include "app/text.h"
#include "common/error.h"
#include "core/engine/read_speed.h"
#include "core/repository/archive_store.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace va::app {

// What the repository records of a run, read for its result page.
struct RecordedRun {
    VerifyRunRecord run;
    std::vector<VerifyItemRecord> items;     // verify_items: each entry that was not Verified
    std::vector<VerifyRunRecord> history;    // the archive's runs, oldest first (verify_runs)
    std::optional<EvidenceRecord> evidence;  // the media evidence recorded with it (run_evidence)
    std::string label;                       // the archive as the views name it (display_label)
    // The volume label of the archive's current carrier as the views show
    // it, for a run whose evidence names none.
    std::string volume_label;
};

// Reads what the repository records of the run `run_id`: the run, its items,
// its archive's runs and its media evidence; the archive named by its
// record's label, or the start of its uuid where it has none (a view names
// it as it lists it, display_label, in place of that), and no volume label
// (a view sets that of the archive's current carrier). Nothing where no run
// is recorded under `run_id`.
std::expected<std::optional<RecordedRun>, IoError> read_recorded_run(ArchiveStore& store, const std::string& run_id);

// An item of a run: its status in a pill in the tone of what it does to the
// result (item_tone), its path ("path:stream" for an alternate data stream),
// and its detail in plain words (as the report of a verification words them,
// item_detail_words).
struct ResultItem {
    Pill status;
    std::string path;
    std::string detail;
    bool operator==(const ResultItem&) const = default;
};

// What an item of `run` does to its result, as the comparison judged it
// (§A6): Neutral for what changes no result (changes_no_result), Warning for
// what only holds a run at PASSED_WITH_WARNINGS (an Unbaselined entry, a name
// this system cannot hold, a metadata difference, an entry unreadable since
// its registration), Error for what fails it. Every other item of a run that
// PASSED_WITH_WARNINGS is a warning; in a run that FAILED with no item that
// fails it and no failure of the run, its metadata differences failed it
// (strict metadata, R-51).
Tone item_tone(const VerifyRunRecord& run, const VerifyItemRecord& item, bool any_failing_item);

// Whether an item changes no result: a mount point where the revision
// records nothing at or below it holds no part of the archive (R-30,
// mount_point_outside_archive).
bool changes_no_result(const VerifyItemRecord& item);

// Whether an item's status fails a run whatever the metadata policy: every
// status but Unbaselined, Unrepresentable and MetadataMismatch, but neither
// an entry unreadable since its registration (decision 0010) nor an item that
// changes no result.
bool fails_the_run(const VerifyItemRecord& item);

// The read speed chart: how fast each hundredth of the content was read, in
// MiB per second, by the verification (this run with the runs it resumed,
// whole_verification) and by the one it was compared with; the stretches
// read markedly slower than that one; and the vertical axis, from 0 to `top`
// with a line every `step`.
struct SpeedChart {
    std::array<std::optional<double>, 100> now{};     // nothing for a hundredth not read, or in no measurable time
    std::array<std::optional<double>, 100> before{};  // all nothing where none was compared with
    bool compared = false;
    std::vector<SlowerStretch> slower;
    double top = 0.0;
    double step = 0.0;
};

// The stretches of hundredths in a row that `values` (a line of a chart)
// holds, each from..to, first to last: where the window's page and the
// verification report draw a line, unbroken.
std::vector<std::pair<int, int>> chart_stretches(const std::array<std::optional<double>, 100>& values);

// A step of an axis that runs from 0 to about `most` in about four steps: 1,
// 2, 2.5 or 5 times a power of ten. 1 for nothing to show.
double axis_step(double most);

// A value of an axis whose lines are `step` apart, as the chart writes it:
// with as many decimals as the step needs, the same for every value of the
// axis ("0", "50", "200"; "0.00", "0.25"; "0.0000", "0.0005").
std::string axis_words(double value, double step);

// The least time that the verification spent reading the hundredths of the
// content it timed for its chart to be drawn: below it, a few hundredths
// read in a few milliseconds would draw spikes that tell nothing.
inline constexpr std::uint64_t chart_least_milliseconds = 1000;

// The chart of `now` against `before`, as the comparison of decision 0044
// found it (`slower`); nothing where `now` spent less than
// chart_least_milliseconds on the hundredths it read in a measurable time.
std::optional<SpeedChart> speed_chart(
    const ReadSpeed& now, const std::optional<ReadSpeed>& before, const std::vector<SlowerStretch>& slower);

// How fast the verification that `run` finished read the content, as its
// report said it after "Read speed:" (read_speed_words), in bytes per
// second: over the hundredths that it and the verification it was compared
// with both read, where it was compared; otherwise over the hundredths that
// it, with the runs it resumed that read the same device
// (whole_verification), read in a measurable time. Nothing where the run
// recorded no read speed, or read in no measurable time. `history` holds the
// archive's runs, or more.
std::optional<std::uint64_t> run_read_speed(const VerifyRunRecord& run, const std::vector<VerifyRunRecord>& history);

// How far an INCOMPLETE run of a FULL verification came, with the runs it
// resumed, as a percentage of the content (below 100); nothing where its
// read speed, which gives the content's size, was not recorded.
std::optional<int> stopped_percent(const VerifyRunRecord& run, const std::vector<VerifyRunRecord>& history);

// The words of the read speed chart, as the window's page and the
// verification report show them: its name, which is also the heading of the
// part of the page that holds it and the chart's accessible name; its
// legend's (this verification's line, the one compared with, dashed, and the
// stretches read markedly slower); and the unit of its vertical axis.
struct ChartWords {
    std::string name;
    std::string now;
    std::string before;
    std::string slower;
    std::string unit;
};

ChartWords chart_words(const Text& text);

// The result page of a run, top to bottom.
struct ResultPage {
    std::string title;             // of the page: the archive and when the run ended
    // The banner: the result with its mode in its tone, the archive (its
    // label and volume label) and when the run ended, local time with its
    // offset; then, PASSED, that every registered byte was read and
    // matched; each failure of the run; what else the run is (a
    // relocation's, history_note); INCOMPLETE, how far it came and that
    // no report can be saved for it.
    Tone tone = Tone::Neutral;
    std::string heading;
    std::string archive;
    std::vector<std::string> lines;
    bool incomplete = false;
    // The device alerts that the run's counts record, as the run's report
    // gave them (health_alert_lines, slower_alert_line).
    std::vector<std::string> alerts;
    // Its items, those that fail the run first, then those that warn, then
    // those that change no result, each in the order of their paths: first
    // on the page for a run that FAILED or PASSED_WITH_WARNINGS; after the
    // read speed for the others, not at all for an INCOMPLETE one that
    // recorded none. `items_none` where there are none.
    bool items_first = false;
    bool items_shown = true;
    std::string items_heading;     // "Items (3)"
    std::vector<ResultItem> items;
    std::string items_none;
    // What was read and matched, the entries compared, how long the run took,
    // the average read speed ("—" where not recorded), each a value and its name.
    std::vector<Field> metrics;
    // The read speed, for a FULL verification that reached a result and read
    // media: the chart where the run recorded its speed, else why there is
    // none; the speed with the one it was compared with, as the report says
    // it (read_speed_words), or that nothing was compared.
    bool speed_shown = false;
    std::optional<SpeedChart> chart;
    std::vector<std::string> speed_lines;
    // The expanders: the media identity, the volume and its devices with the
    // health each reported; the run's id, mode, revision, the run it resumed,
    // chunk hashes added, its times, item counts, the computer and program
    // version that recorded it, and the guarantee (R-02).
    std::vector<Field> identity;
    std::vector<Field> technical;
    // Decision 0047: the device's own identity that the carrier confirmation
    // of a run that went on past a changed enclosure read behind it (its
    // model, its own serial, its WWN), as the confirmation records it; none
    // for any other run. The verification report lists it with the identity,
    // under "Device's own serial" as Devices names it; the window's page
    // does not show it.
    std::vector<Field> confirmed_device;
    // The headings of its parts, as the window's page and the verification
    // report both show them: the read speed's is chart_words.name.
    ChartWords chart_words;
    std::string identity_heading;
    std::string technical_heading;
};

ResultPage result_page(const Text& text, const RecordedRun& recorded, const ShownIdentity& shown = {});

// Why a run that did not finish has no verification report (decision 0048):
// its result page says so, and run_report_html refuses with it (in English;
// Text translates it).
inline constexpr std::string_view no_report_of_incomplete =
    "A report cannot be saved for a verification that has not finished.";

} // namespace va::app
