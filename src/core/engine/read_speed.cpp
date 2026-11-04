#include "core/engine/read_speed.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace va {
namespace {

// A member of `json` of the type asked for, or nothing: the counts come from
// the database, which anyone may have changed (§A5).
std::optional<std::uint64_t> unsigned_of(const nlohmann::json& json, const char* key)
{
    const auto found = json.find(key);
    if (found == json.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

std::string text_of(const nlohmann::json& json, const char* key)
{
    const auto found = json.find(key);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

nlohmann::json counts_of(std::string_view counts_json)
{
    auto counts = nlohmann::json::parse(counts_json, nullptr, false);
    return counts.is_object() ? counts : nlohmann::json::object();
}

// Percent slower of `bytes` in `milliseconds` than `before_bytes` in
// `before_milliseconds`; negative where faster.
int slower_percent(std::uint64_t bytes, std::uint64_t milliseconds, std::uint64_t before_bytes,
    std::uint64_t before_milliseconds)
{
    const double speed = static_cast<double>(bytes) / static_cast<double>(milliseconds);
    const double before = static_cast<double>(before_bytes) / static_cast<double>(before_milliseconds);
    return static_cast<int>(std::lround((1.0 - speed / before) * 100.0));
}

std::uint64_t per_second(std::uint64_t bytes, std::uint64_t milliseconds)
{
    return milliseconds == 0 ? 0
                             : static_cast<std::uint64_t>(std::llround(
                                   static_cast<double>(bytes) * 1000.0 / static_cast<double>(milliseconds)));
}

const VerifyRunRecord* run_named(const std::vector<VerifyRunRecord>& runs, const std::string& run_id)
{
    const auto found = std::ranges::find(runs, run_id, &VerifyRunRecord::run_id);
    return found == runs.end() ? nullptr : &*found;
}

} // namespace

std::uint64_t ReadSpeed::bytes() const
{
    std::uint64_t total = 0;
    for (const auto& part : parts) {
        total += part.bytes;
    }
    return total;
}

std::uint64_t ReadSpeed::milliseconds() const
{
    std::uint64_t total = 0;
    for (const auto& part : parts) {
        total += part.milliseconds;
    }
    return total;
}

std::optional<std::uint64_t> ReadSpeed::bytes_per_second() const
{
    std::uint64_t bytes = 0;
    std::uint64_t milliseconds = 0;
    for (const auto& part : parts) {
        if (part.bytes > 0 && part.milliseconds > 0) {
            bytes += part.bytes;
            milliseconds += part.milliseconds;
        }
    }
    if (milliseconds == 0) {
        return std::nullopt;
    }
    return bytes * 1000 / milliseconds;
}

bool ReadSpeed::comparable(const ReadSpeed& other) const
{
    return content_bytes == other.content_bytes && devices == other.devices &&
           (!devices.empty() || (!volume.empty() && volume == other.volume));
}

void set_device(ReadSpeed& speed, const VolumeInfo& volume)
{
    speed.devices.clear();
    for (const auto& device : volume.devices) {
        const auto& id = device.serial.empty() ? device.wwn : device.serial;
        if (!id.empty()) {
            speed.devices.push_back(device.bus.empty() ? id : device.bus + " " + id);
        }
    }
    std::ranges::sort(speed.devices);
    speed.volume = volume.filesystem_uuid;
}

ReadSpeedMeter::ReadSpeedMeter(IProgressSink& next, std::uint64_t content_bytes, std::vector<File> files, Clock clock)
    : next_(next), content_bytes_(content_bytes), files_(std::move(files)), clock_(std::move(clock))
{
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
    std::uint64_t start = 0;
    for (const auto& file : files_) {
        starts_.push_back(start);
        start += file.size;
    }
}

void ReadSpeedMeter::start()
{
    started_ = true;
    last_bytes_ = 0;
    last_time_ = clock_();
}

void ReadSpeedMeter::on_progress(const Progress& progress)
{
    next_.on_progress(progress);
    if (started_) {
        spend(progress.bytes_done, clock_());
    }
}

std::vector<ReadPart> ReadSpeedMeter::finish()
{
    if (started_) {
        spend(last_bytes_, clock_());
        started_ = false;
    }
    // A hundredth where reading stood still and stopped keeps its time: the
    // run that resumes there adds its own (whole_verification).
    std::vector<ReadPart> parts;
    for (std::uint32_t hundredth = 0; hundredth < 100; ++hundredth) {
        const auto bytes = static_cast<std::uint64_t>(std::llround(bytes_[hundredth]));
        const auto milliseconds = static_cast<std::uint64_t>(std::llround(microseconds_[hundredth] / 1000.0));
        if (bytes > 0 || milliseconds > 0) {
            parts.push_back({hundredth, bytes, milliseconds});
        }
    }
    return parts;
}

double ReadSpeedMeter::content_position(std::size_t file, std::uint64_t within) const
{
    const auto& read = files_[file];
    double position = static_cast<double>(read.content_offset);
    if (read.size > 0 && read.content_length > 0) {
        position += static_cast<double>(std::min(within, read.size)) / static_cast<double>(read.size) *
                    static_cast<double>(read.content_length);
    }
    return std::min(position, static_cast<double>(content_bytes_));
}

std::uint32_t ReadSpeedMeter::hundredth_of(double position) const
{
    const auto hundredth = position * 100.0 / static_cast<double>(content_bytes_);
    return static_cast<std::uint32_t>(std::clamp(std::floor(hundredth), 0.0, 99.0));
}

void ReadSpeedMeter::add(double from, double to, double bytes, double microseconds)
{
    if (to <= from) {
        bytes_[hundredth_of(from)] += bytes;
        microseconds_[hundredth_of(from)] += microseconds;
        return;
    }
    // Shared among the hundredths that the piece covers, by how much of each.
    const double width = static_cast<double>(content_bytes_) / 100.0;
    for (auto hundredth = hundredth_of(from); hundredth <= hundredth_of(std::nextafter(to, from)); ++hundredth) {
        const double covered = std::min(to, (hundredth + 1) * width) - std::max(from, hundredth * width);
        if (covered > 0) {
            const double share = covered / (to - from);
            bytes_[hundredth] += bytes * share;
            microseconds_[hundredth] += microseconds * share;
        }
    }
}

void ReadSpeedMeter::spend(std::uint64_t bytes_done, std::chrono::steady_clock::time_point now)
{
    const auto elapsed =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - last_time_).count());
    last_time_ = now;
    if (files_.empty() || content_bytes_ == 0 || elapsed < 0) {
        last_bytes_ = std::max(last_bytes_, bytes_done);
        return;
    }
    // The file that the pipeline's count of bytes is in, and how far into
    // it: past the end of every file (one read longer than it was scanned),
    // the last.
    const auto file_at = [&](std::uint64_t counted) {
        const auto after = std::upper_bound(starts_.begin(), starts_.end(), counted);
        return std::min(static_cast<std::size_t>(std::distance(starts_.begin(), after)) - 1, files_.size() - 1);
    };
    if (bytes_done <= last_bytes_) {
        const auto file = file_at(last_bytes_);
        const auto at = content_position(file, last_bytes_ - starts_[file]);
        add(at, at, 0, elapsed);
        return;
    }
    // The bytes read since the last report, and the time in their share,
    // file by file, where they lie in the content.
    const auto total = static_cast<double>(bytes_done - last_bytes_);
    for (auto counted = last_bytes_; counted < bytes_done;) {
        const auto file = file_at(counted);
        const auto end = file + 1 == files_.size() ? bytes_done : starts_[file] + files_[file].size;
        const auto until = std::min(bytes_done, end);
        const auto read = static_cast<double>(until - counted);
        add(content_position(file, counted - starts_[file]), content_position(file, until - starts_[file]), read,
            elapsed * read / total);
        counted = until;
    }
    last_bytes_ = bytes_done;
}

std::optional<SpeedComparison> compare_read_speed(const ReadSpeed& now, const ReadSpeed& before)
{
    std::map<std::uint32_t, ReadPart> earlier;
    for (const auto& part : before.parts) {
        if (part.bytes > 0 && part.milliseconds > 0) {
            earlier[part.hundredth] = part;
        }
    }
    SpeedComparison comparison;
    std::uint64_t bytes = 0;
    std::uint64_t milliseconds = 0;
    std::uint64_t before_bytes = 0;
    std::uint64_t before_milliseconds = 0;
    std::optional<SlowerStretch> stretch;
    struct Sums {
        std::uint64_t bytes = 0, milliseconds = 0, before_bytes = 0, before_milliseconds = 0;
    } in_stretch;
    const auto close_stretch = [&] {
        if (stretch) {
            stretch->slower_percent = slower_percent(in_stretch.bytes, in_stretch.milliseconds,
                in_stretch.before_bytes, in_stretch.before_milliseconds);
            comparison.slower_parts.push_back(*stretch);
            stretch.reset();
            in_stretch = {};
        }
    };
    for (const auto& part : now.parts) {
        const auto found = earlier.find(part.hundredth);
        if (part.bytes == 0 || part.milliseconds == 0 || found == earlier.end()) {
            close_stretch();
            continue;
        }
        const auto& was = found->second;
        bytes += part.bytes;
        milliseconds += part.milliseconds;
        before_bytes += was.bytes;
        before_milliseconds += was.milliseconds;
        const bool compared =
            part.milliseconds >= part_compared_milliseconds && was.milliseconds >= part_compared_milliseconds;
        if (compared && slower_percent(part.bytes, part.milliseconds, was.bytes, was.milliseconds) >=
                            part_slower_alert_percent) {
            if (stretch && stretch->to + 1 != part.hundredth) {
                close_stretch();
            }
            if (!stretch) {
                stretch = SlowerStretch{part.hundredth, part.hundredth, 0};
            }
            stretch->to = part.hundredth;
            in_stretch.bytes += part.bytes;
            in_stretch.milliseconds += part.milliseconds;
            in_stretch.before_bytes += was.bytes;
            in_stretch.before_milliseconds += was.milliseconds;
        } else {
            close_stretch();
        }
    }
    close_stretch();
    if (milliseconds == 0 || before_milliseconds == 0) {
        return std::nullopt;
    }
    comparison.bytes_per_second = per_second(bytes, milliseconds);
    comparison.baseline_bytes_per_second = per_second(before_bytes, before_milliseconds);
    comparison.whole_compared =
        milliseconds >= whole_compared_milliseconds && before_milliseconds >= whole_compared_milliseconds;
    if (comparison.whole_compared) {
        comparison.slower_percent = slower_percent(bytes, milliseconds, before_bytes, before_milliseconds);
    }
    return comparison;
}

nlohmann::json read_speed_json(const ReadSpeed& speed)
{
    auto parts = nlohmann::json::array();
    for (const auto& part : speed.parts) {
        parts.push_back({part.hundredth, part.bytes, part.milliseconds});
    }
    return {{"content_bytes", speed.content_bytes}, {"devices", speed.devices}, {"volume", speed.volume},
        {"parts", std::move(parts)}};
}

std::optional<ReadSpeed> recorded_read_speed(std::string_view counts_json)
{
    const auto counts = counts_of(counts_json);
    const auto found = counts.find("read_speed");
    if (found == counts.end() || !found->is_object()) {
        return std::nullopt;
    }
    ReadSpeed speed;
    speed.content_bytes = unsigned_of(*found, "content_bytes").value_or(0);
    speed.volume = text_of(*found, "volume");
    if (const auto devices = found->find("devices"); devices != found->end() && devices->is_array()) {
        for (const auto& device : *devices) {
            if (device.is_string()) {
                speed.devices.push_back(device.get<std::string>());
            }
        }
    }
    const auto parts = found->find("parts");
    if (parts == found->end() || !parts->is_array()) {
        return std::nullopt;
    }
    std::set<std::uint32_t> seen;
    for (const auto& part : *parts) {
        if (!part.is_array() || part.size() != 3 || !part[0].is_number_unsigned() || !part[1].is_number_unsigned() ||
            !part[2].is_number_unsigned() || part[0].get<std::uint64_t>() > 99 ||
            !seen.insert(part[0].get<std::uint32_t>()).second) {
            return std::nullopt;
        }
        speed.parts.push_back({part[0].get<std::uint32_t>(), part[1].get<std::uint64_t>(), part[2].get<std::uint64_t>()});
    }
    std::ranges::sort(speed.parts, {}, &ReadPart::hundredth);
    return speed;
}

nlohmann::json speed_comparison_json(const SpeedComparison& comparison)
{
    auto parts = nlohmann::json::array();
    for (const auto& stretch : comparison.slower_parts) {
        parts.push_back({stretch.from, stretch.to, stretch.slower_percent});
    }
    return {{"baseline", comparison.baseline_run}, {"baseline_finished", comparison.baseline_finished},
        {"bytes_per_second", comparison.bytes_per_second},
        {"baseline_bytes_per_second", comparison.baseline_bytes_per_second},
        {"whole_compared", comparison.whole_compared}, {"slower_percent", comparison.slower_percent},
        {"slower_parts", std::move(parts)}, {"alert", comparison.alerts()}};
}

std::optional<SpeedComparison> recorded_speed_comparison(std::string_view counts_json)
{
    const auto counts = counts_of(counts_json);
    const auto found = counts.find("read_compared");
    if (found == counts.end() || !found->is_object()) {
        return std::nullopt;
    }
    SpeedComparison comparison;
    comparison.baseline_run = text_of(*found, "baseline");
    comparison.baseline_finished = text_of(*found, "baseline_finished");
    comparison.bytes_per_second = unsigned_of(*found, "bytes_per_second").value_or(0);
    comparison.baseline_bytes_per_second = unsigned_of(*found, "baseline_bytes_per_second").value_or(0);
    if (const auto whole = found->find("whole_compared"); whole != found->end() && whole->is_boolean()) {
        comparison.whole_compared = whole->get<bool>();
    }
    if (const auto slower = found->find("slower_percent"); slower != found->end() && slower->is_number_integer()) {
        comparison.slower_percent = slower->get<int>();
    }
    if (const auto parts = found->find("slower_parts"); parts != found->end() && parts->is_array()) {
        for (const auto& part : *parts) {
            if (part.is_array() && part.size() == 3 && part[0].is_number_unsigned() && part[1].is_number_unsigned() &&
                part[2].is_number_integer()) {
                comparison.slower_parts.push_back(
                    {part[0].get<std::uint32_t>(), part[1].get<std::uint32_t>(), part[2].get<int>()});
            }
        }
    }
    return comparison;
}

std::vector<std::string> resumed_runs(const std::string& resumed_from, const std::vector<VerifyRunRecord>& runs)
{
    std::vector<std::string> chain;
    std::set<std::string> seen;
    for (auto run_id = resumed_from; !run_id.empty() && seen.insert(run_id).second;) {
        chain.push_back(run_id);
        const auto* run = run_named(runs, run_id);
        run_id = run ? text_of(counts_of(run->counts_json), "resumed_from") : std::string{};
    }
    return chain;
}

ReadSpeed whole_verification(
    const ReadSpeed& own, const std::string& resumed_from, const std::vector<VerifyRunRecord>& runs)
{
    std::map<std::uint32_t, ReadPart> parts;
    const auto add = [&](const ReadSpeed& speed) {
        for (const auto& part : speed.parts) {
            auto& sum = parts.try_emplace(part.hundredth, ReadPart{part.hundredth, 0, 0}).first->second;
            sum.bytes += part.bytes;
            sum.milliseconds += part.milliseconds;
        }
    };
    add(own);
    for (const auto& run_id : resumed_runs(resumed_from, runs)) {
        const auto* run = run_named(runs, run_id);
        if (!run) {
            continue;
        }
        if (const auto speed = recorded_read_speed(run->counts_json); speed && speed->comparable(own)) {
            add(*speed);
        }
    }
    ReadSpeed whole = own;
    whole.parts.clear();
    for (const auto& [hundredth, part] : parts) {
        whole.parts.push_back(part);
    }
    return whole;
}

std::optional<SpeedBaseline> speed_baseline(
    const ReadSpeed& now, const std::vector<std::string>& chain, const std::vector<VerifyRunRecord>& runs)
{
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        if (std::ranges::find(chain, run->run_id) != chain.end() || run->mode != "FULL" ||
            run->result == "INCOMPLETE" || run->result == "RUNNING" || read_no_media(run->counts_json)) {
            continue;
        }
        const auto own = recorded_read_speed(run->counts_json);
        if (!own || !own->comparable(now)) {
            continue;
        }
        if (const auto compared = recorded_speed_comparison(run->counts_json); compared && compared->alerts()) {
            continue; // itself slower than usual
        }
        auto whole = whole_verification(*own, text_of(counts_of(run->counts_json), "resumed_from"), runs);
        if (whole.parts.empty()) {
            continue;
        }
        return SpeedBaseline{std::move(whole), run->run_id, run->finished};
    }
    return std::nullopt;
}

} // namespace va
