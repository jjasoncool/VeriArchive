#pragma once
// Read speed (decision 0044): how fast a FULL verification read an archive's
// content, by hundredths of that content in the canonical order in which it
// is read, and how that compares with the last FULL verification of the
// archive on the same device whose speed was as usual. Reading markedly
// slower is a device alert: told beside the run result, never changing it.
#include "common/types.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va {

// One hundredth of the content (0 to 99) and the time spent reading what of
// it was read.
struct ReadPart {
    std::uint32_t hundredth = 0;
    std::uint64_t bytes = 0;
    std::uint64_t milliseconds = 0;
};

struct ReadSpeed {
    // The revision's content: the bytes of every file and stream it records
    // with hashes, in canonical order. Its hundredth n is its bytes from
    // n/100 of it on.
    std::uint64_t content_bytes = 0;
    // The device that was read: each device of the volume by its bus and its
    // serial (or WWN), and the volume's filesystem id, for a volume whose
    // devices have neither. Only a verification that read the same device
    // is compared with.
    std::vector<std::string> devices;
    std::string volume;
    std::vector<ReadPart> parts;  // ascending, the hundredths read

    std::uint64_t bytes() const;
    std::uint64_t milliseconds() const;
    // How fast it read, in bytes per second, over the hundredths it read in
    // a measurable time (bytes and milliseconds both above 0), as
    // compare_read_speed takes them; nothing where it read none so. A
    // hundredth of a few bytes read in no measurable time would otherwise
    // count its bytes without its time.
    std::optional<std::uint64_t> bytes_per_second() const;
    // Whether `other` read the same device and the same content.
    bool comparable(const ReadSpeed& other) const;
};

// The device key of ReadSpeed (`devices`, `volume`) for `volume`.
void set_device(ReadSpeed& speed, const VolumeInfo& volume);

// Measures the time spent on each hundredth while the comparison's hash
// pipeline reads, from the progress it reports (five times a second), which
// it passes on to `next`. `files` gives, for each file in the order the
// pipeline reads them, where its content starts in the revision's content
// and how long it is there (0 for a file recorded without hashes), and its
// size as scanned, which the pipeline counts. Time with no byte read goes to
// the hundredth being read, where a device that retries a read spends it.
class ReadSpeedMeter final : public IProgressSink {
public:
    struct File {
        std::uint64_t content_offset = 0;
        std::uint64_t content_length = 0;
        std::uint64_t size = 0;
    };
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    ReadSpeedMeter(IProgressSink& next, std::uint64_t content_bytes, std::vector<File> files, Clock clock = {});

    // Reading starts now.
    void start();
    void on_progress(const Progress& progress) override;
    // Reading has ended; the time since the last report goes where it stopped.
    std::vector<ReadPart> finish();

private:
    void spend(std::uint64_t bytes_done, std::chrono::steady_clock::time_point now);
    // Where `within` bytes into a file lie in the content.
    double content_position(std::size_t file, std::uint64_t within) const;
    std::uint32_t hundredth_of(double position) const;
    // Bytes and time of the content from `from` to `to`, shared among the
    // hundredths it covers; all to the hundredth at `from` where it covers
    // none.
    void add(double from, double to, double bytes, double microseconds);

    IProgressSink& next_;
    std::uint64_t content_bytes_;
    std::vector<File> files_;
    std::vector<std::uint64_t> starts_;  // where each file starts in the pipeline's count of bytes
    Clock clock_;
    bool started_ = false;
    std::uint64_t last_bytes_ = 0;
    std::chrono::steady_clock::time_point last_time_{};
    std::vector<double> bytes_ = std::vector<double>(100, 0.0);
    std::vector<double> microseconds_ = std::vector<double>(100, 0.0);
};

// What is compared: the whole read (over the hundredths both verifications
// read) once each spent at least a minute on them, slower by 30 % or more;
// and each hundredth that each spent at least 10 seconds on, slower by 50 %
// or more, told as stretches of such hundredths in a row.
inline constexpr int whole_slower_alert_percent = 30;
inline constexpr int part_slower_alert_percent = 50;
inline constexpr std::uint64_t whole_compared_milliseconds = 60'000;
inline constexpr std::uint64_t part_compared_milliseconds = 10'000;

struct SlowerStretch {
    std::uint32_t from = 0;  // hundredths from..to, each slower by part_slower_alert_percent or more
    std::uint32_t to = 0;
    int slower_percent = 0;  // the stretch as a whole
};

struct SpeedComparison {
    std::string baseline_run;               // the verification compared with (its last attempt's run)
    Timestamp baseline_finished;
    std::uint64_t bytes_per_second = 0;     // this verification, over the hundredths both read
    std::uint64_t baseline_bytes_per_second = 0;
    bool whole_compared = false;            // both spent a minute or more on those hundredths
    int slower_percent = 0;                 // negative where faster; only with whole_compared
    std::vector<SlowerStretch> slower_parts;

    // Reading markedly slower is a device alert.
    bool alerts() const
    {
        return (whole_compared && slower_percent >= whole_slower_alert_percent) || !slower_parts.empty();
    }
};

// How `now` compares with `before`, two reads of the same content on the same
// device; nothing where no hundredth was read by both.
std::optional<SpeedComparison> compare_read_speed(const ReadSpeed& now, const ReadSpeed& before);

// The read speed and comparison as a run's counts keep them ("read_speed",
// "read_compared"), and read back; nothing where the counts have none or
// what they hold cannot be read.
nlohmann::json read_speed_json(const ReadSpeed& speed);
std::optional<ReadSpeed> recorded_read_speed(std::string_view counts_json);
nlohmann::json speed_comparison_json(const SpeedComparison& comparison);
std::optional<SpeedComparison> recorded_speed_comparison(std::string_view counts_json);

// A verification as it read the content: `own`, the parts read by the run
// that finishes it, added to those of each run that it resumes and that read
// the same device and content (R-104). `resumed_from` names the run it
// resumes; `runs` is the archive's history.
ReadSpeed whole_verification(
    const ReadSpeed& own, const std::string& resumed_from, const std::vector<VerifyRunRecord>& runs);

// The verification to compare a finished one with: the latest earlier FULL
// verification of the archive that reached a result, read the same device
// and content, and was not itself slower (no comparison of it alerts), as
// whole_verification puts it together. `chain` names the runs of the
// verification being compared, which are none of its candidates.
struct SpeedBaseline {
    ReadSpeed speed;
    std::string run_id;
    Timestamp finished;
};
std::optional<SpeedBaseline> speed_baseline(
    const ReadSpeed& now, const std::vector<std::string>& chain, const std::vector<VerifyRunRecord>& runs);

// The runs that a run resumed, from the newest to the oldest (its
// `resumed_from`, then theirs).
std::vector<std::string> resumed_runs(const std::string& resumed_from, const std::vector<VerifyRunRecord>& runs);

} // namespace va
