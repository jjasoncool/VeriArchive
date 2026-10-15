#include "app/progress_estimate.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace va::app {

namespace {

// Tuning, in seconds unless stated otherwise. Checked against simulated
// registrations in tests/unit/progress_estimate_test.cpp.
constexpr double window = 180;              // "recent": speed and full-weight intervals
// Reports on steps: steps can come minutes apart (one large file between two
// checkpoints). The window then grows to this many times their usual spacing,
// the median of the last few, so that it holds three of them and they can give
// a time left (min_moving_intervals).
constexpr double steps_in_window = 3.5;
constexpr std::size_t spacings_kept = 8;
constexpr double sample_spacing = 2;        // reports closer than this share one sample
constexpr double warm_up = 60;              // no time left before this much hashing
constexpr std::size_t min_moving_intervals = 3; // ... nor before this many intervals with progress
constexpr double history_half_life = 1800;  // older intervals lose half their weight per half hour
constexpr double smoothing = 60;            // time constant of the moving average
constexpr double min_speed_span = 5;        // no speed before this much time
constexpr double collinear = 0.02;          // files and bytes this close to proportional: one cost
constexpr double min_spread = 0.05;         // the range is at least 5 % either side ...
constexpr double max_spread = 0.5;          // ... and at most 50 %
constexpr double bytes_per_mib = 1024.0 * 1024.0;
constexpr double most_before_done = 0.99;   // the share done while items remain
// Regular reports: a silence longer than this is a pause. Far longer than the
// work itself ever holds the reports up (a checkpoint written between two
// files), far shorter than a computer's sleep.
constexpr auto pause_after = std::chrono::seconds(30);

// The time model: seconds = per_item * items + per_mib * mib.
struct Model {
    double per_item;
    double per_mib;
};

// A least-squares fit of the model to the sums over the intervals (ii: items
// squared, ib: items times MiB, bb: MiB squared, it: items times seconds, bt:
// MiB times seconds), with no negative cost. When files and bytes rose in
// proportion in every interval, the data cannot tell the two costs apart, and
// bytes alone carry the time.
std::optional<Model> fit(double ii, double ib, double bb, double it, double bt, bool with_bytes)
{
    if (!with_bytes) {
        return ii > 0 ? std::optional<Model>(Model{it / ii, 0}) : std::nullopt;
    }
    if (ii > 0 && bb > 0 && ib * ib < (1 - collinear) * ii * bb) {
        const double determinant = ii * bb - ib * ib;
        const double per_item = (it * bb - bt * ib) / determinant;
        const double per_mib = (bt * ii - it * ib) / determinant;
        if (per_item >= 0 && per_mib >= 0) {
            return Model{per_item, per_mib};
        }
        // A negative cost is not real: the other one alone explains the time.
        return per_item < 0 ? Model{0, bt / bb} : Model{it / ii, 0};
    }
    if (bb > 0) {
        return Model{0, bt / bb};
    }
    if (ii > 0) {
        return Model{it / ii, 0};
    }
    return std::nullopt;
}

} // namespace

void ProgressEstimate::Sums::add(const Interval& interval)
{
    items_items += interval.items * interval.items;
    items_mib += interval.items * interval.mib;
    mib_mib += interval.mib * interval.mib;
    items_seconds += interval.items * interval.seconds;
    mib_seconds += interval.mib * interval.seconds;
}

void ProgressEstimate::Sums::add(const Sums& other, double weight)
{
    items_items += weight * other.items_items;
    items_mib += weight * other.items_mib;
    mib_mib += weight * other.mib_mib;
    items_seconds += weight * other.items_seconds;
    mib_seconds += weight * other.mib_seconds;
}

ProgressEstimate::ProgressEstimate(Reports reports) : reports_(reports), window_(window) {}

void ProgressEstimate::update(const Progress& progress, Clock::time_point now)
{
    const bool new_operation = !start_ || progress.items_total != last_.items_total ||
                               progress.items_done < last_.items_done ||
                               progress.bytes_done < last_.bytes_done;
    if (new_operation) {
        *this = ProgressEstimate{reports_};
        start_ = now;
    } else if (reports_ == Reports::regular && now - last_report_ > pause_after) {
        // The work was paused (the computer slept, the process was stopped,
        // its terminal blocked). Moving the start forward by the silence takes
        // the pause out of the timeline: this report lands where the one before
        // it did, so no interval spans the pause and the samples, the history
        // and the expected end all stay on one timeline.
        *start_ += now - last_report_;
    }
    last_report_ = now;
    last_ = progress;
    now_ = std::chrono::duration<double>(now - *start_).count();
    if (samples_.empty()) {
        samples_.push_back({now_, progress.items_done, progress.bytes_done});
        return;
    }
    const auto previous = samples_.back();
    if (now_ - previous.time < sample_spacing) {
        return;
    }
    samples_.push_back({now_, progress.items_done, progress.bytes_done});
    intervals_.push_back({now_, now_ - previous.time,
        static_cast<double>(progress.items_done - previous.items),
        static_cast<double>(progress.bytes_done - previous.bytes) / bytes_per_mib});
    if (reports_ == Reports::on_steps) {
        size_window(now_ - previous.time);
    }
    // Intervals that ended before the window move into the history, and the
    // sample each one started at goes with it.
    while (!intervals_.empty() && intervals_.front().end <= now_ - window_) {
        add_to_history(intervals_.front());
        intervals_.pop_front();
        samples_.pop_front();
    }
    estimate();
}

void ProgressEstimate::size_window(double spacing)
{
    spacings_.push_back(spacing);
    if (spacings_.size() > spacings_kept) {
        spacings_.pop_front();
    }
    // The median: one large file, a step far later than the others, does not
    // stretch the window.
    std::vector<double> sorted(spacings_.begin(), spacings_.end());
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
    std::ranges::nth_element(sorted, middle);
    window_ = std::max(window, steps_in_window * *middle);
}

void ProgressEstimate::add_to_history(const Interval& interval)
{
    // Fade what is already there to the new interval's time, then add it.
    Sums faded;
    faded.add(history_, std::exp2(-(interval.end - history_time_) / history_half_life));
    history_ = faded;
    history_time_ = interval.end;
    history_.add(interval);
}

void ProgressEstimate::estimate()
{
    const bool with_bytes = last_.bytes_total > 0;
    Sums sums;
    double elapsed = 0;
    std::size_t moving = 0;
    for (const auto& interval : intervals_) {
        sums.add(interval);
        elapsed += interval.seconds;
        moving += interval.items > 0 || interval.mib > 0;
    }
    // History weight: 1 for an interval that has just left the window.
    sums.add(history_, std::exp2(-(now_ - window_ - history_time_) / history_half_life));
    const auto model = fit(sums.items_items, sums.items_mib, sums.mib_mib, sums.items_seconds,
        sums.mib_seconds, with_bytes);

    double predicted = 0;
    if (model) {
        for (const auto& interval : intervals_) {
            predicted += model->per_item * interval.items + model->per_mib * interval.mib;
        }
    }
    if (!model || predicted <= 0 || moving < min_moving_intervals) {
        finish_.reset(); // estimating (again), e.g. after minutes without progress
        return;
    }
    // The fit decides how time splits between files and bytes; the window's
    // real elapsed time sets the pace, so pauses count and a slower device shows.
    // The fit leans on older intervals, so a pace above 1 means the recent ones
    // were slower than the earlier ones: the speed is drifting down.
    const double pace = elapsed / predicted;
    const double per_item = model->per_item * pace;
    const double per_mib = model->per_mib * pace;

    // How well the model explains each recent interval, relative to its length.
    double squares = 0;
    for (const auto& interval : intervals_) {
        const double error = interval.seconds - (per_item * interval.items + per_mib * interval.mib);
        squares += error * error / interval.seconds;
    }
    const double scatter = std::sqrt(squares / elapsed);

    const double items_left = last_.items_total > last_.items_done
                                  ? static_cast<double>(last_.items_total - last_.items_done)
                                  : 0.0;
    const double mib_left = with_bytes && last_.bytes_total > last_.bytes_done
                                ? static_cast<double>(last_.bytes_total - last_.bytes_done) / bytes_per_mib
                                : 0.0;
    const double raw_finish = now_ + per_item * items_left + per_mib * mib_left;

    // Smooth the expected end time. The first estimate is taken as it is.
    double weight = 1;
    if (finish_) {
        weight = 1 - std::exp(-(now_ - estimated_at_) / smoothing);
        *finish_ += weight * (raw_finish - *finish_);
        const double left = std::max(*finish_ - now_, 1.0);
        disagreement_ += weight * (std::abs(raw_finish - *finish_) / left - disagreement_);
    } else {
        finish_ = raw_finish;
        disagreement_ = 0;
    }
    estimated_at_ = now_;

    const double drift = std::log(pace);
    const double noise = min_spread + scatter / 2 + disagreement_;
    const double low = std::min(max_spread, noise + std::max(0.0, -drift));
    const double high = std::min(max_spread, noise + std::max(0.0, drift));
    spread_low_ += weight * (low - spread_low_);
    spread_high_ += weight * (high - spread_high_);
}

std::optional<double> ProgressEstimate::fraction() const
{
    std::optional<double> share;
    if (last_.bytes_total > 0) {
        share = std::min(1.0, static_cast<double>(last_.bytes_done) / static_cast<double>(last_.bytes_total));
    } else if (last_.items_total > 0) {
        share = std::min(1.0, static_cast<double>(last_.items_done) / static_cast<double>(last_.items_total));
    }
    // All bytes can be counted while files remain: empty files at the end, or
    // a file that grew since the scan. The bar and the percentage still wait
    // for the last file.
    if (share && last_.items_total > 0 && !done()) {
        share = std::min(*share, most_before_done);
    }
    return share;
}

std::optional<double> ProgressEstimate::bytes_per_second() const
{
    if (samples_.size() < 2) {
        return std::nullopt;
    }
    const auto& first = samples_.front();
    const auto& last = samples_.back();
    const double span = last.time - first.time;
    if (span < min_speed_span) {
        return std::nullopt;
    }
    return static_cast<double>(last.bytes - first.bytes) / span;
}

std::optional<TimeLeft> ProgressEstimate::time_left() const
{
    if (!start_ || !finish_ || now_ < warm_up) {
        return std::nullopt;
    }
    const double likely = std::max(0.0, *finish_ - now_);
    const auto seconds = [](double value) { return std::chrono::seconds(std::llround(value)); };
    return TimeLeft{seconds(likely * (1 - spread_low_)), seconds(likely), seconds(likely * (1 + spread_high_))};
}

std::string format_size(std::uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    // Up a unit from 1000 on, so a value never needs four digits.
    while (value >= 999.5 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return std::format("{} B", bytes);
    }
    // Three significant digits: 2.16, 24.0, 240.
    if (value < 9.995) {
        return std::format("{:.2f} {}", value, units[unit]);
    }
    if (value < 99.95) {
        return std::format("{:.1f} {}", value, units[unit]);
    }
    return std::format("{:.0f} {}", value, units[unit]);
}

std::string format_time_left(const Text& text, const std::optional<TimeLeft>& time_left)
{
    if (!time_left) {
        return text("estimating time left");
    }
    const double low = static_cast<double>(time_left->low.count());
    const double high = static_cast<double>(time_left->high.count());
    if (high <= 60) {
        return text("less than a minute left");
    }
    // The range is rounded outward to whole units, so it never claims more
    // precision than the estimate has.
    enum class Unit { Minutes, Hours, Days };
    Unit unit = Unit::Days;
    double unit_seconds = 86400;
    double step = 1;
    if (low < 3600) {
        unit = Unit::Minutes;
        unit_seconds = 60;
        step = high <= 900 ? 1 : 5; // "4-5 minutes", "25-30 minutes"
    } else if (low < 2 * 86400) {
        unit = Unit::Hours;
        unit_seconds = 3600;
    }
    const auto from = static_cast<long long>(std::floor(low / unit_seconds / step) * step);
    const auto to = static_cast<long long>(std::ceil(high / unit_seconds / step) * step);
    if (from == 0) {
        return text.format("less than {} minutes left", to);
    }
    if (from == to) {
        switch (unit) {
        case Unit::Minutes:
            return from == 1 ? text("about 1 minute left") : text.format("about {} minutes left", from);
        case Unit::Hours:
            return from == 1 ? text("about 1 hour left") : text.format("about {} hours left", from);
        case Unit::Days:
            return text.format("about {} days left", from); // days start at two
        }
    }
    switch (unit) {
    case Unit::Minutes:
        return text.format("about {}-{} minutes left", from, to);
    case Unit::Hours:
        return text.format("about {}-{} hours left", from, to);
    case Unit::Days:
        break;
    }
    return text.format("about {}-{} days left", from, to);
}

std::string describe_progress(const Text& text, const ProgressEstimate& estimate)
{
    const auto& progress = estimate.progress();
    // Rounded down, and fraction() stays below 1 while items remain: 100 %
    // means done.
    const auto percent = static_cast<int>(std::floor(estimate.fraction().value_or(0) * 100));
    // Two calls: the compiler checks each literal template against its values.
    auto line = estimate.counts_bytes()
                    ? text.format("{} of {} ({}%)", format_size(progress.bytes_done),
                          format_size(progress.bytes_total), percent)
                    : text.format("{} of {} files ({}%)", progress.items_done, progress.items_total, percent);
    // No speed for work that reads no bytes.
    if (const auto speed = estimate.bytes_per_second(); speed && progress.bytes_done > 0) {
        const auto per_second = text.format("{}/s", format_size(static_cast<std::uint64_t>(std::llround(*speed))));
        line = text.format("{} · {}", line, per_second);
    }
    // Once every item is done nothing counted is left. An estimate made a
    // moment earlier would stay on screen while the results are written
    // (the manifest, the run), which this progress does not count.
    if (estimate.done()) {
        return line;
    }
    return text.format("{} · {}", line, format_time_left(text, estimate.time_left()));
}

std::size_t display_width(std::string_view text)
{
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        // The length of the UTF-8 sequence, from its first byte.
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        char32_t code = lead;
        if (length > 1 && index + length <= text.size()) {
            code = lead & (0x7F >> length);
            for (std::size_t next = 1; next < length; ++next) {
                code = (code << 6) | (static_cast<unsigned char>(text[index + next]) & 0x3F);
            }
        }
        // East Asian wide and full-width characters (Unicode UAX #11), roughly.
        const bool wide = (code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) ||
                          (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
                          (code >= 0xFE30 && code <= 0xFE4F) || (code >= 0xFF00 && code <= 0xFF60) ||
                          (code >= 0xFFE0 && code <= 0xFFE6) || (code >= 0x20000 && code <= 0x3FFFD);
        width += wide ? 2 : 1;
        index += length;
    }
    return width;
}

std::string StatusLine::replace(std::string_view line)
{
    const auto width = display_width(line);
    std::string output = "\r";
    output += line;
    if (width_ && width < *width_) {
        output.append(*width_ - width, ' ');
    }
    width_ = width;
    return output;
}

std::string StatusLine::clear()
{
    if (!width_) {
        return {};
    }
    std::string output = "\r";
    output.append(*width_, ' ');
    output += '\r';
    width_.reset();
    return output;
}

} // namespace va::app
