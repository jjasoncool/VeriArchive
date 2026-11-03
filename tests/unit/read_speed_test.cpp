// Read speed (decision 0044): the time spent on each hundredth of the
// content as a FULL verification reads it, how two reads of it compare, what
// a run's counts keep of them, which earlier verification a finished one is
// compared with, and how reports word it. Every archive, device, run and time
// here is sample data made up for the test.
#include "test_support.h"

#include "engine_fixture.h"

#include "app/device_alerts.h"
#include "app/local_time.h"
#include "app/text.h"
#include "core/engine/read_speed.h"

#include <chrono>
#include <memory>

namespace {

using namespace std::chrono_literals;
using va::ReadPart;
using va::ReadSpeed;
using va::ReadSpeedMeter;

// A clock the test moves.
struct ManualClock {
    std::shared_ptr<std::chrono::steady_clock::time_point> now =
        std::make_shared<std::chrono::steady_clock::time_point>();
    ReadSpeedMeter::Clock clock() const
    {
        return [now = now] { return *now; };
    }
    void advance(std::chrono::milliseconds by) const { *now += by; }
};

va::Progress hashed(std::uint64_t bytes)
{
    return {bytes, 1000, 0, 2, ""};
}

const ReadPart* part_of(const std::vector<ReadPart>& parts, std::uint32_t hundredth)
{
    for (const auto& part : parts) {
        if (part.hundredth == hundredth) {
            return &part;
        }
    }
    return nullptr;
}

bool part_is(const std::vector<ReadPart>& parts, std::uint32_t hundredth, std::uint64_t bytes,
    std::uint64_t milliseconds)
{
    const auto* part = part_of(parts, hundredth);
    return part && part->bytes == bytes && part->milliseconds == milliseconds;
}

// Two files of a 1000-byte content, 600 and 400 bytes: each hundredth is 10
// bytes. The progress passes on unchanged.
void the_meter_times_each_hundredth()
{
    ManualClock clock;
    test::RecordedProgress next;
    ReadSpeedMeter meter(next, 1000, {{0, 600, 600}, {600, 400, 400}}, clock.clock());
    meter.start();
    clock.advance(3s);
    meter.on_progress(hashed(300));  // 0..29: 10 bytes in 100 ms each
    clock.advance(6s);
    meter.on_progress(hashed(600));  // 30..59: 10 bytes in 200 ms each
    clock.advance(3s);
    meter.on_progress(hashed(600));  // nothing read: where reading stands, at 60
    clock.advance(4s);
    meter.on_progress(hashed(1000)); // 60..99: 10 bytes in 100 ms each
    const auto parts = meter.finish();
    CHECK(next.reports.size() == 4 && next.reports.back().bytes_done == 1000);
    CHECK(parts.size() == 100);
    CHECK(part_is(parts, 0, 10, 100) && part_is(parts, 29, 10, 100));
    CHECK(part_is(parts, 30, 10, 200) && part_is(parts, 59, 10, 200));
    CHECK(part_is(parts, 60, 10, 3100) && part_is(parts, 61, 10, 100) && part_is(parts, 99, 10, 100));

    // A read stopped (cancelled, the device gone): the time since the last
    // report goes where it stood, at hundredth 10, which kept no byte.
    ManualClock stopped_clock;
    ReadSpeedMeter stopped(next, 1000, {{0, 600, 600}, {600, 400, 400}}, stopped_clock.clock());
    stopped.start();
    stopped_clock.advance(1s);
    stopped.on_progress(hashed(100));
    stopped_clock.advance(5s);
    const auto stopped_parts = stopped.finish();
    CHECK(stopped_parts.size() == 11 && part_is(stopped_parts, 0, 10, 100) && part_is(stopped_parts, 9, 10, 100) &&
          part_is(stopped_parts, 10, 0, 5000));
}

// A resumed run reads only what the run it resumes did not match: its parts
// lie where those files are in the content. A file recorded without hashes
// (unreadable at registration) has no place of its own in it: its bytes and
// time go to the hundredth where it lies.
void a_resumed_read_lies_where_its_files_are()
{
    ManualClock clock;
    test::NullProgress next;
    ReadSpeedMeter meter(next, 1000, {{600, 0, 50}, {600, 400, 400}}, clock.clock());
    meter.start();
    clock.advance(1s);
    meter.on_progress(hashed(50));
    clock.advance(4s);
    meter.on_progress(hashed(450));
    const auto parts = meter.finish();
    CHECK(parts.size() == 40 && parts.front().hundredth == 60 && parts.back().hundredth == 99);
    CHECK(part_is(parts, 60, 60, 1100) && part_is(parts, 61, 10, 100));

    // A file longer now than recorded is read whole: the bytes past its
    // recorded length stay in its last hundredth. An empty content has none.
    ReadSpeedMeter longer(next, 1000, {{990, 10, 30}}, clock.clock());
    longer.start();
    clock.advance(3s);
    longer.on_progress(hashed(30));
    const auto longer_parts = longer.finish();
    CHECK(longer_parts.size() == 1 && part_is(longer_parts, 99, 30, 3000));
    ReadSpeedMeter empty(next, 0, {{0, 0, 10}}, clock.clock());
    empty.start();
    clock.advance(1s);
    empty.on_progress(hashed(10));
    CHECK(empty.finish().empty());
}

ReadSpeed sample_speed(std::uint64_t milliseconds_each)
{
    ReadSpeed speed;
    speed.content_bytes = 100'000'000'000;
    speed.devices = {"sata SAMPLE-SERIAL"};
    speed.volume = "c0ffee00-0000-4000-8000-000000000044";
    for (std::uint32_t hundredth = 0; hundredth < 100; ++hundredth) {
        speed.parts.push_back({hundredth, 1'000'000'000, milliseconds_each});
    }
    return speed;
}

void two_reads_compare()
{
    const auto before = sample_speed(20'000);  // 50 MB/s, 20 s a hundredth

    // As fast: nothing slower.
    const auto same = va::compare_read_speed(sample_speed(20'000), before);
    CHECK(same && same->whole_compared && same->slower_percent == 0 && same->slower_parts.empty());
    CHECK(same && !same->alerts() && same->bytes_per_second == 50'000'000 &&
          same->baseline_bytes_per_second == 50'000'000);

    // 35 % slower over all: the whole alerts, no hundredth is 50 % slower.
    const auto slower = va::compare_read_speed(sample_speed(30'770), before);
    CHECK(slower && slower->slower_percent == 35 && slower->slower_parts.empty() && slower->alerts());
    // 25 % slower: no alert.
    const auto a_little = va::compare_read_speed(sample_speed(26'667), before);
    CHECK(a_little && a_little->slower_percent == 25 && !a_little->alerts());
    // Faster is negative.
    const auto faster = va::compare_read_speed(sample_speed(10'000), before);
    CHECK(faster && faster->slower_percent == -100 && !faster->alerts());

    // Hundredths 10 to 14 and 50 at less than half the speed: two stretches,
    // while the whole is only 8 % slower.
    auto in_parts = sample_speed(20'000);
    for (const auto hundredth : {10u, 11u, 12u, 13u, 14u, 50u}) {
        in_parts.parts[hundredth].milliseconds = 50'000;
    }
    const auto stretches = va::compare_read_speed(in_parts, before);
    CHECK(stretches && stretches->slower_percent == 8 && stretches->slower_parts.size() == 2);
    if (stretches && stretches->slower_parts.size() == 2) {
        CHECK(stretches->slower_parts[0].from == 10 && stretches->slower_parts[0].to == 14 &&
              stretches->slower_parts[0].slower_percent == 60);
        CHECK(stretches->slower_parts[1].from == 50 && stretches->slower_parts[1].to == 50);
        CHECK(stretches->alerts());
    }

    // A hundredth read in less than 10 seconds on either side is not
    // compared on its own; the whole is, from a minute on.
    auto quick = sample_speed(5'000);
    quick.parts[3].milliseconds = 9'000;
    const auto short_parts = va::compare_read_speed(quick, sample_speed(1'000));
    CHECK(short_parts && short_parts->slower_parts.empty() && short_parts->whole_compared &&
          short_parts->slower_percent >= 80);
    const auto too_short = va::compare_read_speed(sample_speed(500), sample_speed(100));
    CHECK(too_short && !too_short->whole_compared && too_short->slower_percent == 0 && !too_short->alerts());

    // Only the hundredths both read are compared; none in common, nothing.
    auto first_half = sample_speed(40'000);
    first_half.parts.resize(50);
    const auto half = va::compare_read_speed(first_half, before);
    CHECK(half && half->slower_percent == 50 && half->slower_parts.size() == 1 && half->slower_parts[0].to == 49);
    auto second_half = sample_speed(20'000);
    second_half.parts.erase(second_half.parts.begin(), second_half.parts.begin() + 50);
    CHECK(!va::compare_read_speed(first_half, second_half));
}

std::string counts_with(const ReadSpeed& speed, const std::optional<va::SpeedComparison>& compared = std::nullopt,
    const std::string& resumed_from = "")
{
    nlohmann::json counts{{"items", {{"Verified", 3}}}, {"read_speed", va::read_speed_json(speed)}};
    if (compared) {
        counts["read_compared"] = va::speed_comparison_json(*compared);
    }
    if (!resumed_from.empty()) {
        counts["resumed_from"] = resumed_from;
    }
    return counts.dump();
}

va::VerifyRunRecord sample_run(const std::string& run_id, const std::string& result, const std::string& counts)
{
    va::VerifyRunRecord run;
    run.run_id = run_id;
    run.archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    run.mode = "FULL";
    run.result = result;
    run.finished = "2026-11-01T08:00:00Z";
    run.counts_json = counts;
    return run;
}

void what_the_counts_keep()
{
    auto speed = sample_speed(20'000);
    speed.parts.resize(3);
    const auto back = va::recorded_read_speed(counts_with(speed));
    CHECK(back && back->comparable(speed) && back->parts.size() == 3 && back->parts[2].milliseconds == 20'000);
    CHECK(!va::recorded_read_speed(R"({"items":{}})"));
    CHECK(!va::recorded_read_speed(R"({"read_speed":{"parts":[[100,1,1]]}})"));
    CHECK(!va::recorded_read_speed(R"({"read_speed":{"parts":[[1,1,1],[1,2,2]]}})"));
    CHECK(!va::recorded_read_speed(R"({"read_speed":{"parts":[[1,"x",1]]}})"));
    CHECK(!va::recorded_read_speed("not json"));

    va::SpeedComparison compared;
    compared.baseline_run = "c0ffee00-0000-4000-8000-0000000000b1";
    compared.baseline_finished = "2026-11-01T08:00:00Z";
    compared.bytes_per_second = 30'000'000;
    compared.baseline_bytes_per_second = 50'000'000;
    compared.whole_compared = true;
    compared.slower_percent = 40;
    compared.slower_parts = {{0, 11, 55}};
    const auto read_back = va::recorded_speed_comparison(counts_with(speed, compared));
    CHECK(read_back && read_back->baseline_run == compared.baseline_run && read_back->slower_percent == 40 &&
          read_back->slower_parts.size() == 1 && read_back->slower_parts[0].to == 11 && read_back->alerts());
    CHECK(va::speed_comparison_json(compared)["alert"] == true);
    CHECK(!va::recorded_speed_comparison(counts_with(speed)));

    // Comparable: the same device and content.
    auto other_device = speed;
    other_device.devices = {"usb SAMPLE-OTHER"};
    auto other_content = speed;
    other_content.content_bytes += 1;
    CHECK(!speed.comparable(other_device) && !speed.comparable(other_content));
    auto no_serial = speed;
    no_serial.devices.clear();
    auto same_volume = no_serial;
    CHECK(no_serial.comparable(same_volume));
    same_volume.volume = "c0ffee00-0000-4000-8000-000000000045";
    CHECK(!no_serial.comparable(same_volume));

    va::VolumeInfo volume;
    volume.filesystem_uuid = "c0ffee00-0000-4000-8000-000000000044";
    va::PhysicalDeviceInfo second;
    second.bus = "usb";
    second.wwn = "0x5000SAMPLE";
    va::PhysicalDeviceInfo first;
    first.bus = "sata";
    first.serial = "SAMPLE-SERIAL";
    va::PhysicalDeviceInfo unknown;
    volume.devices = {second, first, unknown};
    ReadSpeed keyed;
    va::set_device(keyed, volume);
    CHECK((keyed.devices == std::vector<std::string>{"sata SAMPLE-SERIAL", "usb 0x5000SAMPLE"}));
    CHECK(keyed.volume == volume.filesystem_uuid);
}

// A verification is compared as a whole: the runs it resumed add their
// parts; it is compared with the latest earlier verification that reached a
// result on the same device and content and was not itself slower.
void which_verification_is_compared_with()
{
    auto first_half = sample_speed(20'000);
    first_half.parts.resize(50);
    auto second_half = sample_speed(20'000);
    second_half.parts.erase(second_half.parts.begin(), second_half.parts.begin() + 50);
    const auto whole = sample_speed(20'000);
    std::vector<va::VerifyRunRecord> runs{
        sample_run("run-1", "PASSED", counts_with(whole)),
        sample_run("run-2", "INCOMPLETE", counts_with(first_half)),
        sample_run("run-3", "PASSED", counts_with(second_half, std::nullopt, "run-2")),
    };
    CHECK((va::resumed_runs("run-2", runs) == std::vector<std::string>{"run-2"}));
    CHECK(va::resumed_runs("", runs).empty());
    const auto together = va::whole_verification(second_half, "run-2", runs);
    CHECK(together.parts.size() == 100 && together.parts[0].milliseconds == 20'000);

    // The latest, as resumed: run 3 with run 2.
    const auto now = sample_speed(20'000);
    const auto latest = va::speed_baseline(now, {}, runs);
    CHECK(latest && latest->run_id == "run-3" && latest->speed.parts.size() == 100);
    const auto baseline_of = [&](const std::vector<std::string>& chain) {
        const auto baseline = va::speed_baseline(now, chain, runs);
        return baseline ? baseline->run_id : std::string("none");
    };
    // Not the verification being compared, nor one slower than usual.
    CHECK(baseline_of({"run-3"}) == "run-1");
    va::SpeedComparison slower;
    slower.whole_compared = true;
    slower.slower_percent = 45;
    runs.push_back(sample_run("run-4", "PASSED", counts_with(whole, slower)));
    CHECK(baseline_of({}) == "run-3");
    // Not one of another device or content, nor one that read no media.
    auto elsewhere = whole;
    elsewhere.devices = {"usb SAMPLE-OTHER"};
    runs.push_back(sample_run("run-5", "PASSED", counts_with(elsewhere)));
    auto failed_early = sample_run("run-6", "FAILED", counts_with(whole));
    failed_early.counts_json = nlohmann::json{{"items", nlohmann::json::object()}, {"media_read", false},
        {"read_speed", va::read_speed_json(whole)}}.dump();
    runs.push_back(failed_early);
    CHECK(baseline_of({}) == "run-3");
    // A run that failed on its content read it all the same.
    runs.push_back(sample_run("run-7", "FAILED", counts_with(whole)));
    CHECK(baseline_of({}) == "run-7");
    CHECK(baseline_of({"run-7", "run-4", "run-3", "run-1"}) == "none");
    // A run that resumes itself, as no program writes, ends.
    CHECK(va::resumed_runs("run-8", {sample_run("run-8", "INCOMPLETE", counts_with(whole, std::nullopt, "run-8"))})
              .size() == 1);
}

// The clock of a run whose read takes `read_time` in all, however often the
// pipeline reports: the start at 0, every later time at `read_time`.
ReadSpeedMeter::Clock read_in(std::chrono::seconds read_time)
{
    auto calls = std::make_shared<int>(0);
    return [calls, read_time] {
        return std::chrono::steady_clock::time_point{} + ((*calls)++ == 0 ? 0s : read_time);
    };
}

// FULL verifications of a sample archive on a sample device: the first is
// compared with nothing; the second, read in twice the time, is 50 % slower,
// a device alert beside a result that stays PASSED; the third, as fast as the
// first, is compared with the first, as the second was slower than usual.
void verifications_are_compared()
{
    test::EngineFixture fixture;
    for (const auto* name : {"SAMPLE-a.bin", "SAMPLE-b.bin", "SAMPLE-c.bin"}) {
        fixture.archive.add_file(name, std::string(1000, 's'));
    }
    CHECK(fixture.register_root(fixture.archive.root()).has_value());
    const auto verify = [&](std::chrono::seconds read_time) {
        va::VerifyRequest request;
        request.root = fixture.archive.root();
        request.read_clock = read_in(read_time);
        return fixture.verify(request);
    };
    const auto first = verify(1000s);
    CHECK(first && first->result == "PASSED" && first->read_speed && !first->read_compared);
    CHECK(first && first->read_speed->content_bytes == 3000 && first->read_speed->bytes() == 3000 &&
          first->read_speed->milliseconds() == 1'000'000);
    const auto second = verify(2000s);
    CHECK(second && second->result == "PASSED" && second->read_compared);
    if (first && second && second->read_compared) {
        CHECK(second->read_compared->baseline_run == first->run_id && second->read_compared->slower_percent == 50 &&
              second->read_compared->alerts());
        const auto counts = fixture.repository.query(
            "SELECT counts_json FROM verify_runs WHERE run_id = '" + second->run_id + "'");
        const auto recorded = va::recorded_speed_comparison(counts);
        CHECK(recorded && recorded->alerts() && recorded->slower_percent == 50);
        const auto speed = va::recorded_read_speed(counts);
        CHECK(speed && speed->devices == std::vector<std::string>{"SERIAL-1"} &&
              speed->volume == "1111222233334444");
    }
    const auto third = verify(1000s);
    CHECK(third && third->read_compared && first && third->read_compared->baseline_run == first->run_id &&
          third->read_compared->slower_percent == 0 && !third->read_compared->alerts());
}

void words()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    va::SpeedComparison compared;
    compared.baseline_run = "c0ffee00-0000-4000-8000-0000000000b1";
    compared.baseline_finished = "2026-11-01T08:00:00Z";
    compared.bytes_per_second = 100 * 1024 * 1024;
    compared.baseline_bytes_per_second = 200 * 1024 * 1024;
    compared.whole_compared = true;
    compared.slower_percent = 50;
    compared.slower_parts = {{0, 11, 55}, {40, 40, 70}};
    const auto when = va::app::local_time(compared.baseline_finished, english);
    const auto line = va::app::slower_alert_line(english, compared);
    CHECK(line == "⚠ Device alert: reading was 50% slower than the last FULL verification of this archive on this "
                  "device (100 MiB/s against 200 MiB/s, " + when + "); from 0% to 12% of the content, 55% slower; from "
                  "40% to 41% of the content, 70% slower. A device that reads markedly slower than before may be "
                  "failing; another program reading it at the same time, or another device read through the same "
                  "port or hub, also makes reading slower.");
    const auto chinese_when = va::app::local_time(compared.baseline_finished, chinese);
    CHECK(va::app::slower_alert_line(chinese, compared) ==
          "⚠ 裝置警訊：讀取速度比此封存上次在此裝置上的完整驗證慢了 50%（這次每秒 100 MiB，上次每秒 200 MiB，" + chinese_when +
              "）；內容的 0% 至 12% 處慢了 55%；內容的 40% 至 41% 處慢了 70%。讀取明顯比以前慢的裝置可能即將故障；同時有"
              "其他程式在讀取它，或接在同一個連接埠或集線器上的另一個裝置也在被讀取，也會讓讀取變慢。");
    // Slower only in parts.
    auto in_parts = compared;
    in_parts.slower_percent = 10;
    in_parts.slower_parts = {{3, 3, 60}, {5, 5, 60}, {7, 7, 60}, {9, 9, 60}, {11, 11, 60}, {13, 13, 60}, {15, 15, 60}};
    const auto parts_line = va::app::slower_alert_line(english, in_parts);
    CHECK(parts_line && parts_line->starts_with("⚠ Device alert: parts of the content read markedly slower than "
                                                "in the last FULL verification of this archive on this device (" +
                                                when + "); from 3% to 4% of the content, 60% slower;"));
    CHECK(parts_line && parts_line->find("; from 11% to 12% of the content, 60% slower; and 2 more such stretches. ") !=
                            std::string::npos);
    // Not slower: no alert.
    auto usual = compared;
    usual.slower_percent = 2;
    usual.slower_parts.clear();
    CHECK(!va::app::slower_alert_line(english, usual));

    CHECK(va::app::read_speed_words(english, std::nullopt, usual) ==
          "100 MiB/s; the last FULL verification of this archive on this device read 200 MiB/s (" + when + ")");
    CHECK(va::app::read_speed_words(chinese, std::nullopt, usual) ==
          "每秒 100 MiB；此封存上次在此裝置上的完整驗證為每秒 200 MiB（" + chinese_when + "）");
    CHECK(va::app::read_speed_words(english, sample_speed(20'000), std::nullopt) == "47.7 MiB/s");
    CHECK(!va::app::read_speed_words(english, std::nullopt, std::nullopt));
    CHECK(!va::app::read_speed_words(english, ReadSpeed{}, std::nullopt));
}

} // namespace

void read_speed_tests()
{
    the_meter_times_each_hundredth();
    a_resumed_read_lies_where_its_files_are();
    two_reads_compare();
    what_the_counts_keep();
    which_verification_is_compared_with();
    verifications_are_compared();
    words();
}
