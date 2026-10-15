#pragma once
// Progress for people: the share of the work done, the recent speed and the
// time left, worked out from the engines' progress reports (HANDOFF §B9).
//
// The share done is exact: bytes hashed of the bytes to hash when that total is
// known, otherwise items done of items to do. The time left is an estimate and
// is shown as a range ("about 8-9 hours left").
//
// How the time left is estimated. Reading a file costs a fixed amount of time
// (opening it; on an HDD, moving the head to it) plus time for each byte.
// So the time a stretch of work takes is modelled as
//
//     time = a * files + b * bytes
//
// and a folder of small photos and a folder of large videos are both predicted
// by one model, although their bytes per second differ a lot. Every couple of
// seconds the estimator records how many files and bytes were done in how much
// time, and fits a and b to those intervals by least squares: the intervals of
// the last few minutes count fully, older ones less and less (half of their
// weight every half hour). The older ones matter when the recent stretch holds
// only files of one size, which cannot tell the two costs apart. The fitted
// model is then scaled so that it reproduces the time the recent intervals
// really took (pauses included), and the time left is a * files left +
// b * bytes left.
//
// The estimate is smoothed (an exponential moving average of the expected end
// time), so a single slow or fast moment does not swing it. The range is at
// least 5 % either side; it widens when the recent intervals disagree with the
// model, when new estimates keep differing from the smoothed one, and, on the
// side it points to, when the recent pace drifts from the earlier one (an HDD
// gets slower toward its inner tracks).
//
// When reports come regularly (the hash pipeline reports five times a second,
// also while nothing moves), a long silence means the work itself was paused:
// the computer slept, the process was stopped, or its terminal stopped taking
// output. The pause is taken out of the estimator's timeline, so it does not
// count as an hour of very slow work.
#include "app/text.h"
#include "core/interfaces.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace va::app {

// The time left: the most likely value and a range around it.
struct TimeLeft {
    std::chrono::seconds low;
    std::chrono::seconds likely;
    std::chrono::seconds high;
};

class ProgressEstimate {
public:
    using Clock = std::chrono::steady_clock;

    // How the reports arrive, which decides what a long gap between two means.
    enum class Reports {
        on_steps,  // only when the numbers moved (a poll of another process's
                   // checkpoints): a long gap is work, such as one large file
        regular,   // at a steady pace, also while nothing moves (the hash
                   // pipeline): a long silence is a pause, not work
    };

    explicit ProgressEstimate(Reports reports = Reports::on_steps) : reports_(reports) {}

    // Takes one progress report and the time it arrived; call it for every
    // report. A report that counts less than the one before, or has a different
    // number of items to do, starts a new operation (hashing after scanning).
    void update(const Progress& progress, Clock::time_point now);

    // The latest report.
    const Progress& progress() const { return last_; }

    // True when the share done counts bytes (the total to hash is known).
    bool counts_bytes() const { return last_.bytes_total > 0; }

    // True when the items to do are known and every one is done.
    bool done() const { return last_.items_total > 0 && last_.items_done >= last_.items_total; }

    // The share done, from 0 to 1: by bytes when their total is known, else by
    // items; nothing when neither total is known (scanning). It stays below 1
    // (at most 0.99) until every item is done, also when the bytes are all
    // counted while items remain (empty files), so 100 % means done.
    std::optional<double> fraction() const;

    // Bytes per second over the last few minutes; nothing during the first
    // seconds.
    std::optional<double> bytes_per_second() const;

    // The time left; nothing while estimating: during the first minute, until
    // several intervals showed progress, and when nothing moved for minutes.
    std::optional<TimeLeft> time_left() const;

private:
    // Times are seconds since the operation's first report.
    struct Sample {
        double time;
        std::uint64_t items;
        std::uint64_t bytes;
    };
    // The work done between two samples, with bytes in MiB to keep the sums small.
    struct Interval {
        double end;
        double seconds;
        double items;
        double mib;
    };
    // The sums a least-squares fit of seconds = a * items + b * mib needs.
    struct Sums {
        double items_items = 0;
        double items_mib = 0;
        double mib_mib = 0;
        double items_seconds = 0;
        double mib_seconds = 0;
        void add(const Interval& interval);
        void add(const Sums& other, double weight);
    };

    void add_to_history(const Interval& interval);
    void estimate();

    Reports reports_;
    Progress last_{};
    std::optional<Clock::time_point> start_;  // the operation's first report, moved forward past pauses
    Clock::time_point last_report_{};         // when the latest report arrived
    double now_ = 0;                          // the latest report
    std::deque<Sample> samples_;              // the window's samples: one more than intervals
    std::deque<Interval> intervals_;          // the last few minutes
    Sums history_;                            // older intervals, weighted as of history_time_
    double history_time_ = 0;

    // The smoothed estimate.
    std::optional<double> finish_;            // when the work is expected to end
    double estimated_at_ = 0;
    double disagreement_ = 0;                 // how far new estimates differ from finish_
    double spread_low_ = 0;                   // share below the likely value
    double spread_high_ = 0;                  // share above it
};

// "2.16 TiB", "240 MiB", "512 B": binary units, as elsewhere in the interface,
// with three significant digits.
std::string format_size(std::uint64_t bytes);

// "about 8-9 hours left", "約剩 8～9 小時", the range rounded outward. The low
// end of the range picks the unit, so that it never rounds down to zero:
// minutes while it is under an hour ("about 55-175 minutes left"), hours while
// it is under two days ("about 47-96 hours left"), then days. "estimating time
// left" when there is no estimate yet.
std::string format_time_left(const Text& text, const std::optional<TimeLeft>& time_left);

// The progress of hashing, without its label: "2.16 TiB of 9.44 TiB (23%) ·
// 240 MiB/s · about 8-9 hours left"; by files when the bytes to hash are not
// known: "1200 of 5000 files (24%) · ...". No time left once every item is done.
std::string describe_progress(const Text& text, const ProgressEstimate& estimate);

// The columns a terminal uses for UTF-8 `text`: two for Chinese characters and
// other wide characters, one for the rest.
std::size_t display_width(std::string_view text);

// One terminal line rewritten in place (the CLI's progress on stderr). Each
// call returns what to write: a carriage return takes the cursor back to the
// start of the line, and blanks cover what a longer line before left behind,
// counted in columns (display_width), not bytes.
class StatusLine {
public:
    // What to write so that `line` replaces the line shown.
    std::string replace(std::string_view line);
    // What to write to blank the line shown and leave the cursor at its start;
    // nothing when no line is shown.
    std::string clear();
    bool shown() const { return width_.has_value(); }

private:
    std::optional<std::size_t> width_;  // columns of the line shown
};

} // namespace va::app
