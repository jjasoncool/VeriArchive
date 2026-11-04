#pragma once
// The result page of one recorded run (decision 0046), worded from what the
// repository records of it: its row of the history (VerifyRunRecord, its
// counts included), its items (verify_items), the media evidence recorded
// with it, and the archive's other runs, which a resumed run continues and a
// read is compared with. Never a bare status (R-21): the result keeps its
// mode; every time is local with its offset (decision 0017); the guarantee
// (R-02) is on every page. The devices, the volume and the computer are the
// ones the views show (displayed_identity): the sample ones in the screenshot
// mode.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "core/engine/read_speed.h"
#include "core/repository/archive_store.h"
#include "gui/archive_rows.h"
#include "gui/displayed_identity.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

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

// An item of a run: its status in a pill in the tone of what it does to the
// result (item_tone), its path ("path:stream" for an alternate data stream),
// and its detail in plain words (as the report of a verification words them,
// app::item_detail_words).
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
// report said it after "Read speed:" (app::read_speed_words), in bytes per
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

// "152,408": a count with its thousands grouped, as both languages write it.
std::string grouped(std::uint64_t count);

// The result page of a run, top to bottom.
struct ResultPage {
    std::string title;             // of the page: the archive and when the run ended
    // The banner: the result with its mode in its tone, the archive (its
    // label and volume label) and when the run ended, local time with its
    // offset; then, PASSED, that every registered byte was read and
    // matched; each failure of the run; what else the run is (a
    // relocation's, app::history_note); INCOMPLETE, how far it came and that
    // no report can be saved for it.
    Tone tone = Tone::Neutral;
    std::string heading;
    std::string archive;
    std::vector<std::string> lines;
    bool incomplete = false;
    // The device alerts that the run's counts record, as the run's report
    // gave them (app::health_alert_lines, app::slower_alert_line).
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
    // it (app::read_speed_words), or that nothing was compared.
    bool speed_shown = false;
    std::optional<SpeedChart> chart;
    std::vector<std::string> speed_lines;
    // The expanders: the media identity, the volume and its devices with the
    // health each reported; the run's id, mode, revision, the run it resumed,
    // chunk hashes added, its times, item counts, the computer and program
    // version that recorded it, and the guarantee (R-02).
    std::vector<Field> identity;
    std::vector<Field> technical;
};

ResultPage result_page(const app::Text& text, const RecordedRun& recorded, IdentityShown shown);

// The stopped verification that the run `run` left and that Resume
// continues, as the details' Resume finds it (details_operations): the
// latest read of the unfinished operations (`watches`) lists it with its run
// and offers Resume on it; nothing for any other run.
std::optional<app::OperationRow> stopped_run(const app::Text& text, const std::vector<app::OperationWatch>& watches,
    const VerifyRunRecord& run, app::OperationWatch::Clock::time_point now, std::chrono::sys_seconds wall_now);

} // namespace va::gui
