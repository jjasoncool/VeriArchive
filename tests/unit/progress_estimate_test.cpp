// Progress for people: share done by bytes, speed and time remaining.
//
// The estimator is fed simulated registrations: files that each cost a fixed
// time before their bytes (opening, seeking) and then bytes at the device's
// speed, reported every 0.2 seconds like the hash pipeline does. The true time
// left is known, so the estimate can be measured against it, and against a
// naive estimate (bytes left at the speed of the last three minutes).
#include "app/progress_estimate.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string_view>
#include <vector>

namespace {

using va::app::Language;
using va::app::ProgressEstimate;
using va::app::Text;
using va::app::TimeLeft;
using Clock = ProgressEstimate::Clock;
using std::chrono::seconds;

constexpr double mb = 1e6;

struct Report {
    double time;  // seconds since the start
    va::Progress progress;
};

Clock::time_point at(double time)
{
    return Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(time));
}

// A run over files of `sizes`: each file costs `per_file` seconds, then its
// bytes at `speed(time)` bytes per second. Bytes count per 8 MiB buffer read,
// a file when it is done. Without `with_bytes` the total bytes are unknown.
std::vector<Report> simulate(const std::vector<std::uint64_t>& sizes, double per_file,
    const std::function<double(double)>& speed, bool with_bytes = true)
{
    constexpr double step = 0.2;
    constexpr std::uint64_t buffer = 8u << 20;
    std::uint64_t total = 0;
    for (const auto size : sizes) {
        total += size;
    }
    std::vector<Report> reports;
    double time = 0;
    std::size_t index = 0;
    double overhead_left = per_file;
    double read = 0;             // bytes of the current file read
    std::uint64_t counted = 0;   // ... and counted as done
    std::uint64_t files_done = 0;
    std::uint64_t bytes_done = 0;
    while (index < sizes.size()) {
        double budget = step;
        while (budget > 1e-12 && index < sizes.size()) {
            if (overhead_left > 0) {
                const double used = std::min(budget, overhead_left);
                overhead_left -= used;
                budget -= used;
                continue;
            }
            const double rate = speed(time + step - budget);
            const double left = static_cast<double>(sizes[index]) - read;
            if (budget * rate >= left) {
                budget -= left / rate;
                bytes_done += sizes[index] - counted;
                ++files_done;
                ++index;
                overhead_left = per_file;
                read = 0;
                counted = 0;
            } else {
                read += budget * rate;
                budget = 0;
                const auto whole = static_cast<std::uint64_t>(read) / buffer * buffer;
                bytes_done += whole - counted;
                counted = whole;
            }
        }
        time += step - budget;
        reports.push_back({time, {bytes_done, with_bytes ? total : 0, files_done, sizes.size(), ""}});
    }
    return reports;
}

// File sizes that vary but repeat: `count` sizes from `smallest` to about
// `largest`, in a scrambled order.
std::vector<std::uint64_t> varied_sizes(std::size_t count, double smallest, double largest)
{
    std::vector<std::uint64_t> sizes;
    for (std::size_t index = 0; index < count; ++index) {
        const double share = static_cast<double>(index * 7919 % 101) / 100.0;
        sizes.push_back(static_cast<std::uint64_t>(smallest + share * (largest - smallest)));
    }
    return sizes;
}

// The naive estimate: bytes left at the speed of the last three minutes.
std::optional<double> naive_time_left(const std::vector<Report>& reports, std::size_t index)
{
    const auto& now = reports[index];
    std::size_t first = index;
    while (first > 0 && reports[first - 1].time >= now.time - 180) {
        --first;
    }
    const auto& then = reports[first];
    if (now.time <= then.time || now.progress.bytes_done <= then.progress.bytes_done) {
        return std::nullopt;
    }
    const double speed = static_cast<double>(now.progress.bytes_done - then.progress.bytes_done) /
                         (now.time - then.time);
    return static_cast<double>(now.progress.bytes_total - now.progress.bytes_done) / speed;
}

double seconds_of(std::chrono::seconds value)
{
    return static_cast<double>(value.count());
}

// Feeds a steady stream: `speed` bytes per second toward `total`, a report
// every two seconds up to `until`.
ProgressEstimate steady_stream(double speed, std::uint64_t total, double until)
{
    ProgressEstimate estimate;
    for (double time = 0; time <= until + 1e-9; time += 2) {
        const auto done = static_cast<std::uint64_t>(speed * time);
        estimate.update({done, total, 0, 1, ""}, at(time));
    }
    return estimate;
}

// No time left during the first minute, whatever the progress; then one.
void warm_up_shows_estimating()
{
    const auto reports = simulate(varied_sizes(3000, 1 * mb, 100 * mb), 0.012, [](double) { return 200 * mb; });
    ProgressEstimate estimate;
    bool estimating_early = true;
    bool estimate_later = false;
    for (const auto& report : reports) {
        estimate.update(report.progress, at(report.time));
        if (report.time < 59.9) {
            estimating_early = estimating_early && !estimate.time_left();
        } else if (report.time > 62) {
            estimate_later = estimate.time_left().has_value();
            break;
        }
    }
    CHECK(estimating_early);
    CHECK(estimate_later);

    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::app::format_time_left(english, std::nullopt) == "estimating time left");
    CHECK(va::app::format_time_left(chinese, std::nullopt) == "剩餘時間估算中");
}

// At a steady rate the estimate is within 3 % of the truth once warmed up,
// and the range holds the truth.
void steady_rate_is_accurate()
{
    const auto reports = simulate(varied_sizes(3000, 1 * mb, 100 * mb), 0.012, [](double) { return 200 * mb; });
    const double end = reports.back().time;
    ProgressEstimate estimate;
    double worst = 0;
    bool in_range = true;
    for (const auto& report : reports) {
        estimate.update(report.progress, at(report.time));
        const auto left = estimate.time_left();
        const double truth = end - report.time;
        if (report.time < 90 || truth < 120) {
            continue;
        }
        CHECK(left.has_value());
        if (!left) {
            continue;
        }
        worst = std::max(worst, std::abs(seconds_of(left->likely) - truth) / truth);
        in_range = in_range && seconds_of(left->low) <= truth + 1 && truth - 1 <= seconds_of(left->high);
    }
    CHECK(worst < 0.03);
    CHECK(in_range);
    // The speed: 200 MB/s less the time spent opening files.
    const auto speed = estimate.bytes_per_second();
    CHECK(speed && *speed > 180 * mb && *speed < 200 * mb);
}

struct Steadiness {
    double largest_step;    // largest change of the expected end between two reports, share of the time left
    double estimate_error;  // mean distance of the expected end from the true end, seconds
    double naive_error;
    double estimate_swing;  // largest minus smallest expected end, seconds
    double naive_swing;
};

// Measures the estimate and the naive one from `after` seconds on, while more
// than five minutes are left.
Steadiness measure(const std::vector<Report>& reports, double after)
{
    const double end = reports.back().time;
    ProgressEstimate estimate;
    Steadiness result{0, 0, 0, 0, 0};
    std::optional<double> previous_end;
    double lowest = 1e300, highest = -1e300, naive_lowest = 1e300, naive_highest = -1e300;
    std::size_t counted = 0;
    for (std::size_t index = 0; index < reports.size(); ++index) {
        const auto& report = reports[index];
        estimate.update(report.progress, at(report.time));
        const auto left = estimate.time_left();
        if (!left || end - report.time < 300) {
            previous_end.reset();
            continue;
        }
        const double expected_end = report.time + seconds_of(left->likely);
        if (previous_end) {
            result.largest_step = std::max(result.largest_step,
                std::abs(expected_end - *previous_end) / seconds_of(left->likely));
        }
        previous_end = expected_end;
        const auto naive = naive_time_left(reports, index);
        if (report.time < after || !naive) {
            continue;
        }
        const double naive_end = report.time + *naive;
        result.estimate_error += std::abs(expected_end - end);
        result.naive_error += std::abs(naive_end - end);
        lowest = std::min(lowest, expected_end);
        highest = std::max(highest, expected_end);
        naive_lowest = std::min(naive_lowest, naive_end);
        naive_highest = std::max(naive_highest, naive_end);
        ++counted;
    }
    if (counted > 0) {
        result.estimate_error /= static_cast<double>(counted);
        result.naive_error /= static_cast<double>(counted);
        result.estimate_swing = highest - lowest;
        result.naive_swing = naive_highest - naive_lowest;
    }
    return result;
}

// Folders of small photos alternate with folders of large videos, ten
// minutes each: bytes per second halve and double, but the time per file and
// per byte stay the same, and the estimate learns them. Once it has seen both
// kinds, its expected end stays put while the naive one swings by many
// minutes; between two reports it never jumps.
std::vector<Report> alternating_run()
{
    std::vector<std::uint64_t> sizes;
    for (int cycle = 0; cycle < 4; ++cycle) {
        sizes.insert(sizes.end(), 20000, static_cast<std::uint64_t>(3 * mb));   // 10 minutes of photos
        sizes.insert(sizes.end(), 60, static_cast<std::uint64_t>(2000 * mb));   // 10 minutes of videos
    }
    return simulate(sizes, 0.015, [](double) { return 200 * mb; });
}

void alternating_phases_are_steady()
{
    const auto reports = alternating_run();
    const double cycle = 20 * 60 + 60; // one cycle of photos and videos, plus the window
    const auto measured = measure(reports, cycle);
    CHECK(measured.largest_step < 0.03);
    CHECK(measured.estimate_error < measured.naive_error / 5);
    CHECK(measured.estimate_swing < measured.naive_swing / 5);
    CHECK(measured.estimate_error < 60);     // within a minute of the true end, in 80 minutes
    CHECK(measured.naive_swing > 10 * 60);   // the naive one: ten minutes and more
}

// The HDD slows from 250 MB/s to 125 MB/s (its inner tracks): the estimate
// follows the recent speed, so it is closer than one from the average speed
// since the start, its error shrinks toward the end, and its range reaches
// further up (slower) than down.
void gradual_slowdown_is_followed()
{
    const auto reports = simulate(varied_sizes(5000, 50 * mb, 350 * mb), 0.012,
        [](double time) { return 250 * mb - 125 * mb * std::min(time / 7500, 1.0); });
    const double end = reports.back().time;
    ProgressEstimate estimate;
    std::vector<double> errors;            // at 25 %, 50 % and 90 %
    double average_error_at_half = 0;
    bool wider_above = false;
    const double marks[] = {0.25, 0.5, 0.9};
    for (const auto& report : reports) {
        estimate.update(report.progress, at(report.time));
        if (errors.size() == std::size(marks) || !estimate.fraction() || *estimate.fraction() < marks[errors.size()]) {
            continue;
        }
        const auto left = estimate.time_left();
        CHECK(left.has_value());
        if (!left) {
            return;
        }
        const double truth = end - report.time;
        errors.push_back(std::abs(seconds_of(left->likely) - truth) / truth);
        if (errors.size() == 2) {
            const auto& p = report.progress;
            const double average = static_cast<double>(p.bytes_done) / report.time;
            const double from_average = static_cast<double>(p.bytes_total - p.bytes_done) / average;
            average_error_at_half = std::abs(from_average - truth) / truth;
            wider_above = left->high - left->likely > left->likely - left->low;
        }
    }
    CHECK(errors.size() == 3);
    if (errors.size() == 3) {
        CHECK(errors[0] < 0.3);
        CHECK(errors[1] < errors[0]);
        CHECK(errors[2] < errors[1]);
        CHECK(errors[2] < 0.1);
        CHECK(errors[1] < average_error_at_half);
    }
    CHECK(wider_above);
}

// Without a byte total the share done counts files, and so does the time left.
void items_only_progress()
{
    const auto reports = simulate(varied_sizes(3000, 1 * mb, 100 * mb), 0.012,
        [](double) { return 200 * mb; }, false);
    const double end = reports.back().time;
    ProgressEstimate estimate;
    double worst = 0;
    for (const auto& report : reports) {
        estimate.update(report.progress, at(report.time));
        CHECK(!estimate.counts_bytes());
        const auto left = estimate.time_left();
        const double truth = end - report.time;
        if (report.time < 120 || truth < 120 || !left) {
            continue;
        }
        worst = std::max(worst, std::abs(seconds_of(left->likely) - truth) / truth);
    }
    CHECK(worst < 0.1);
    const auto& middle = reports[reports.size() / 2].progress;
    ProgressEstimate half;
    half.update(middle, at(0));
    CHECK(middle.bytes_done > 0 && middle.bytes_total == 0);
    CHECK(half.fraction() && std::abs(*half.fraction() - static_cast<double>(middle.items_done) / 3000.0) < 1e-12);

    // Files that read no bytes: no speed is shown.
    ProgressEstimate entries;
    entries.update({0, 0, 12, 345, ""}, at(0));
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::app::describe_progress(english, entries) == "12 of 345 files (3%) · estimating time left");
    CHECK(va::app::describe_progress(chinese, entries) == "12 / 345 個檔案（3%）・剩餘時間估算中");
}

// Hashing after scanning is a new operation: the warm-up starts again. So does
// a stall: minutes without progress bring back "estimating".
void new_operations_and_stalls()
{
    ProgressEstimate estimate;
    for (double time = 0; time < 100; time += 0.2) {
        estimate.update({0, 0, static_cast<std::uint64_t>(time * 10), 0, ""}, at(time));
    }
    CHECK(!estimate.fraction());
    for (double time = 100; time < 150; time += 0.2) {
        const auto done = static_cast<std::uint64_t>((time - 100) * 100 * mb);
        estimate.update({done, 100000 * static_cast<std::uint64_t>(mb), 0, 10, ""}, at(time));
        CHECK(!estimate.time_left());
    }
    double time = 150;
    std::uint64_t done = 0;
    for (; time < 200; time += 0.2) {
        done = static_cast<std::uint64_t>((time - 100) * 100 * mb);
        estimate.update({done, 100000 * static_cast<std::uint64_t>(mb), 0, 10, ""}, at(time));
    }
    CHECK(estimate.time_left().has_value());
    for (; time < 500; time += 0.2) {
        estimate.update({done, 100000 * static_cast<std::uint64_t>(mb), 0, 10, ""}, at(time));
    }
    CHECK(!estimate.time_left());
    const auto speed = estimate.bytes_per_second();
    CHECK(speed && *speed == 0);
}

// The computer sleeps for an hour in the middle of hashing (or the process is
// stopped, or its terminal stops taking output): no report at all, then the
// work goes on at the same speed. Reports that come regularly make the silence
// a pause, not an hour of very slow work, so the estimate stays as good as it
// was. Reports that come only when the numbers moved count a long gap as work:
// one large file read in fifteen minutes, with no checkpoint in between.
void a_pause_is_not_slow_work()
{
    constexpr double speed = 200 * mb;
    constexpr double work = 10000;  // seconds of hashing: 2 TB
    const auto total = static_cast<std::uint64_t>(speed * work);

    constexpr double sleep_at = 1200;
    constexpr double pause = 3600;
    ProgressEstimate regular(ProgressEstimate::Reports::regular);
    double worst = 0;
    bool estimated = true;
    for (int step = 0; step * 0.2 <= work; ++step) {
        const double worked = step * 0.2;
        const double clock = worked < sleep_at ? worked : worked + pause;
        regular.update({static_cast<std::uint64_t>(speed * worked), total, 0, 1, ""}, at(clock));
        const double truth = work - worked;
        if (worked < 90 || truth < 120) {
            continue;
        }
        const auto left = regular.time_left();
        estimated = estimated && left.has_value();
        if (left) {
            worst = std::max(worst, std::abs(seconds_of(left->likely) - truth) / truth);
        }
    }
    CHECK(estimated);
    CHECK(worst < 0.1);

    // A checkpoint a minute, then fifteen minutes on one large file.
    constexpr double gap_at = 1200;
    constexpr double gap = 900;
    ProgressEstimate on_steps;
    worst = 0;
    estimated = true;
    for (double time = 0; time <= work; time += time == gap_at ? gap : 60) {
        on_steps.update({static_cast<std::uint64_t>(speed * time), total, 0, 1, ""}, at(time));
        const double truth = work - time;
        // Three steps after the gap the window holds enough intervals again.
        // The first estimate also waits for three intervals.
        if (time < 180 || (time > gap_at && time < gap_at + gap + 3 * 60) || truth < 120) {
            continue;
        }
        const auto left = on_steps.time_left();
        estimated = estimated && left.has_value();
        if (left) {
            worst = std::max(worst, std::abs(seconds_of(left->likely) - truth) / truth);
        }
    }
    CHECK(estimated);
    CHECK(worst < 0.1);
}

// Reports on steps that come irregularly: a file far larger than the others
// between them (decision 0022: the estimate counts at least the last three
// steps however far apart they come). A step far later than the usual spacing
// leaves no fewer than three steps to estimate from: the time left stays, and
// stays right. Two kinds: two minute steps then one of six minutes, repeated;
// and minute steps with one of nine minutes among them.
void irregular_steps_keep_a_time_left()
{
    constexpr double speed = 150 * mb;
    const auto total = static_cast<std::uint64_t>(speed * 40000);
    std::vector<double> mixed;
    for (int index = 0; index < 45; ++index) {
        mixed.push_back(index % 3 == 2 ? 360.0 : 60.0);
    }
    std::vector<double> one_long(30, 60.0);
    one_long.push_back(540);
    one_long.insert(one_long.end(), 5, 60.0);
    for (const auto& gaps : {mixed, one_long}) {
        ProgressEstimate estimate;  // on steps
        estimate.update({0, total, 0, 0, ""}, at(0));
        double time = 0;
        int missing = 0;
        double worst = 0;
        for (std::size_t index = 0; index < gaps.size(); ++index) {
            time += gaps[index];
            const auto done = static_cast<std::uint64_t>(speed * time);
            estimate.update({done, total, 0, 0, ""}, at(time));
            if (index < 3) {
                continue;  // the first estimate waits for three steps
            }
            const auto left = estimate.time_left();
            if (!left) {
                ++missing;
                continue;
            }
            const double truth = static_cast<double>(total - done) / speed;
            worst = std::max(worst, std::abs(seconds_of(left->likely) / truth - 1));
        }
        CHECK(missing == 0);
        CHECK(worst < 0.05);
    }
}

// 100 % means done. Every byte can be counted while files remain (empty files
// last): the share stops at 99 % until the last file. Once every file is done
// the line shows no time left, as an estimate made a moment before would stay
// on screen while the manifest is written.
void hundred_percent_means_done()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const std::uint64_t total = 50'000'000'000;  // 46.6 GiB
    ProgressEstimate estimate(ProgressEstimate::Reports::regular);
    estimate.update({total, total, 800, 5500, ""}, at(0));
    CHECK(estimate.fraction() == 0.99);
    CHECK(va::app::describe_progress(english, estimate) == "46.6 GiB of 46.6 GiB (99%) · estimating time left");
    CHECK(va::app::describe_progress(chinese, estimate) == "46.6 GiB / 46.6 GiB（99%）・剩餘時間估算中");
    estimate.update({total, total, 5500, 5500, ""}, at(1));
    CHECK(estimate.fraction() == 1.0);
    CHECK(va::app::describe_progress(english, estimate) == "46.6 GiB of 46.6 GiB (100%)");
    CHECK(va::app::describe_progress(chinese, estimate) == "46.6 GiB / 46.6 GiB（100%）");

    // A whole run that ends with empty files.
    auto sizes = varied_sizes(2000, 1 * mb, 100 * mb);
    sizes.insert(sizes.end(), 500, 0);  // six seconds with every byte counted
    const auto reports = simulate(sizes, 0.012, [](double) { return 200 * mb; });
    ProgressEstimate run(ProgressEstimate::Reports::regular);
    bool full_too_early = false;
    std::size_t waiting_at_99 = 0;
    for (const auto& report : reports) {
        run.update(report.progress, at(report.time));
        const auto line = va::app::describe_progress(english, run);
        const auto& progress = report.progress;
        if (progress.items_done < progress.items_total) {
            full_too_early = full_too_early || line.contains("(100%)");
            waiting_at_99 += progress.bytes_done == progress.bytes_total && line.contains("(99%)");
        }
    }
    CHECK(!full_too_early);
    CHECK(waiting_at_99 > 10);
    const auto last = va::app::describe_progress(english, run);
    CHECK(last.contains("(100%)") && last.contains("MiB/s") && !last.contains("left"));
}

void sizes_are_formatted()
{
    using va::app::format_size;
    CHECK(format_size(0) == "0 B");
    CHECK(format_size(999) == "999 B");
    CHECK(format_size(1000) == "0.98 KiB");
    CHECK(format_size(1536) == "1.50 KiB");
    CHECK(format_size(240ull << 20) == "240 MiB");
    CHECK(format_size(12'300'000'000ull) == "11.5 GiB");
    CHECK(format_size(static_cast<std::uint64_t>(2.16 * 1099511627776.0)) == "2.16 TiB");
    CHECK(format_size(10ull << 40) == "10.0 TiB");
    CHECK(format_size(1023ull << 40) == "1.00 PiB"); // never four digits: not "1023 TiB"
}

void time_left_is_formatted()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    // True when the time left reads `in_english` and `in_chinese`.
    const auto reads = [&](std::int64_t low, std::int64_t likely, std::int64_t high, std::string_view in_english,
                           std::string_view in_chinese) {
        const TimeLeft left{seconds(low), seconds(likely), seconds(high)};
        const auto shown_english = va::app::format_time_left(english, left);
        const auto shown_chinese = va::app::format_time_left(chinese, left);
        if (shown_english != in_english || shown_chinese != in_chinese) {
            std::cerr << "  time left shown as: " << shown_english << " / " << shown_chinese << '\n';
            return false;
        }
        return true;
    };
    CHECK(reads(30, 40, 50, "less than a minute left", "剩不到 1 分鐘"));
    CHECK(reads(50, 70, 100, "less than 2 minutes left", "剩不到 2 分鐘"));
    CHECK(reads(250, 270, 290, "about 4-5 minutes left", "約剩 4～5 分鐘"));
    CHECK(reads(120, 120, 120, "about 2 minutes left", "約剩 2 分鐘"));
    // Five-minute steps above a quarter of an hour.
    CHECK(reads(22 * 60, 25 * 60, 28 * 60, "about 20-30 minutes left", "約剩 20～30 分鐘"));
    CHECK(reads(50 * 60, 60 * 60, 75 * 60, "about 50-75 minutes left", "約剩 50～75 分鐘"));
    // Hours from an hour on, days from two days on; rounded outward.
    CHECK(reads(8 * 3600 + 360, 8 * 3600 + 1800, 8 * 3600 + 3240, "about 8-9 hours left", "約剩 8～9 小時"));
    CHECK(reads(3600, 3600, 3600, "about 1 hour left", "約剩 1 小時"));
    CHECK(reads(3 * 3600, 3 * 3600, 3 * 3600, "about 3 hours left", "約剩 3 小時"));
    CHECK(reads(47 * 3600, 50 * 3600, 53 * 3600, "about 47-53 hours left", "約剩 47～53 小時"));
    CHECK(reads(50 * 3600, 55 * 3600, 60 * 3600, "about 2-3 days left", "約剩 2～3 天"));
    CHECK(reads(2 * 86400, 2 * 86400, 2 * 86400, "about 2 days left", "約剩 2 天"));
    // The low end picks the unit, so it never rounds down to zero hours or days.
    CHECK(reads(3420, 6840, 10260, "about 55-175 minutes left", "約剩 55～175 分鐘"));
    CHECK(reads(172799, 259200, 345600, "about 47-96 hours left", "約剩 47～96 小時"));
}

// The whole line, in both languages: 2.5 TiB of 10 TiB hashed at 256 MiB/s
// leaves 8.5 hours, shown as 8-9 hours.
void progress_line_is_formatted()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const double speed = 256.0 * 1024 * 1024;
    const auto total = 10ull << 40;

    const auto early = steady_stream(speed, total, 30);
    CHECK(english.format("hashing: {}", va::app::describe_progress(english, early)) ==
          "hashing: 7.50 GiB of 10.0 TiB (0%) · 256 MiB/s · estimating time left");
    CHECK(chinese.format("hashing: {}", va::app::describe_progress(chinese, early)) ==
          "計算雜湊中：7.50 GiB / 10.0 TiB（0%）・每秒 256 MiB・剩餘時間估算中");

    const auto later = steady_stream(speed, total, 10240);
    const auto left = later.time_left();
    CHECK(left && left->likely == seconds(30720));
    CHECK(english.format("hashing: {}", va::app::describe_progress(english, later)) ==
          "hashing: 2.50 TiB of 10.0 TiB (25%) · 256 MiB/s · about 8-9 hours left");
    CHECK(chinese.format("hashing: {}", va::app::describe_progress(chinese, later)) ==
          "計算雜湊中：2.50 TiB / 10.0 TiB（25%）・每秒 256 MiB・約剩 8～9 小時");
    CHECK(chinese.format("Hashing: {}", va::app::describe_progress(chinese, later)) ==
          chinese.format("hashing: {}", va::app::describe_progress(chinese, later)));
}

void display_width_counts_columns()
{
    using va::app::display_width;
    CHECK(display_width("hashing: 23%") == 12);
    CHECK(display_width("計算雜湊中：") == 12);
    CHECK(display_width("（23%）") == 7);
    CHECK(display_width("・～") == 4);
    CHECK(display_width("·") == 1);
    CHECK(display_width("") == 0);
}

// The CLI's progress line: a shorter line blanks what the longer one before it
// left, counted in columns (a Chinese character takes two), and clearing
// blanks the whole line.
void status_line_is_rewritten_in_place()
{
    va::app::StatusLine line;
    CHECK(!line.shown() && line.clear().empty());
    CHECK(line.replace("hashing: 12 of 345 files") == "\rhashing: 12 of 345 files");  // 24 columns
    CHECK(line.replace("hashing: 9%") == "\rhashing: 9%" + std::string(13, ' '));     // 11 columns
    CHECK(line.replace("計算雜湊中：（23%）") == "\r計算雜湊中：（23%）");             // 19 columns
    CHECK(line.replace("abc") == "\rabc" + std::string(16, ' '));
    CHECK(line.shown());
    CHECK(line.clear() == "\r   \r");
    CHECK(!line.shown() && line.clear().empty());
}

} // namespace

void progress_estimate_tests()
{
    warm_up_shows_estimating();
    steady_rate_is_accurate();
    alternating_phases_are_steady();
    gradual_slowdown_is_followed();
    items_only_progress();
    new_operations_and_stalls();
    a_pause_is_not_slow_work();
    irregular_steps_keep_a_time_left();
    hundred_percent_means_done();
    sizes_are_formatted();
    time_left_is_formatted();
    progress_line_is_formatted();
    display_width_counts_columns();
    status_line_is_rewritten_in_place();
}
