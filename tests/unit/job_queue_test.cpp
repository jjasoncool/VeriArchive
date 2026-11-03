// The window's jobs (decision 0045): which devices a job reads, the order in
// which jobs of one device take their turn, that a stopped job gives up its
// place, and which operation a job runs. Every device, volume, archive and
// root here is sample data made up for the test.
#include "test_support.h"

#include "engine_fixture.h"

#include "gui/job_queue.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using va::gui::DeviceQueue;

va::VolumeInfo sample_volume()
{
    va::VolumeInfo volume;
    volume.filesystem_uuid = "c0ffee00-0000-4000-8000-000000000045";
    va::PhysicalDeviceInfo by_serial;
    by_serial.serial = "SAMPLE-SERIAL";
    by_serial.wwn = "SAMPLE-WWN-1";
    by_serial.device_ref = "/dev/sdx";
    va::PhysicalDeviceInfo by_wwn;
    by_wwn.wwn = "SAMPLE-WWN-2";
    by_wwn.device_ref = "/dev/sdy";
    va::PhysicalDeviceInfo by_ref;
    by_ref.device_ref = "/dev/sdz";
    volume.devices = {by_serial, by_wwn, by_ref, by_serial};
    return volume;
}

void devices_are_keyed()
{
    CHECK((va::gui::device_keys(sample_volume()) ==
           std::vector<std::string>{"device:SAMPLE-SERIAL", "device:SAMPLE-WWN-2", "attached:/dev/sdz",
               "volume:c0ffee00-0000-4000-8000-000000000045"}));
    CHECK(va::gui::device_keys(va::VolumeInfo{}).empty());

    // As the archive's latest revision records its carrier: the same keys
    // for the same device, and the archive itself.
    const nlohmann::json header{{"identity",
        {{"filesystem_uuid", "c0ffee00-0000-4000-8000-000000000045"},
            {"devices", {{{"serial", "SAMPLE-SERIAL"}, {"wwn", ""}}, {{"serial", ""}, {"wwn", "SAMPLE-WWN-2"}},
                            {{"serial", ""}, {"wwn", ""}}}}}}};
    CHECK((va::gui::recorded_device_keys("c0ffee00-0000-4000-8000-000000000001", header) ==
           std::vector<std::string>{"device:SAMPLE-SERIAL", "device:SAMPLE-WWN-2",
               "volume:c0ffee00-0000-4000-8000-000000000045", "archive:c0ffee00-0000-4000-8000-000000000001"}));
    // A header that cannot be read names the archive alone.
    CHECK((va::gui::recorded_device_keys("c0ffee00-0000-4000-8000-000000000001", nlohmann::json("x")) ==
           std::vector<std::string>{"archive:c0ffee00-0000-4000-8000-000000000001"}));
    CHECK((va::gui::recorded_device_keys(
               "c0ffee00-0000-4000-8000-000000000001", nlohmann::json{{"identity", {{"devices", 5}}}}) ==
           std::vector<std::string>{"archive:c0ffee00-0000-4000-8000-000000000001"}));
}

// Waits up to two seconds for `done`.
bool soon(const std::atomic<bool>& done)
{
    for (int tries = 0; tries < 200 && !done.load(); ++tries) {
        std::this_thread::sleep_for(10ms);
    }
    return done.load();
}

void jobs_of_one_device_take_turns()
{
    DeviceQueue queue;
    va::CancelToken never;
    // Nothing to read, or another device: at once.
    CHECK(queue.take(1, {"device:A"}, never));
    CHECK(queue.take(2, {}, never));
    CHECK(queue.take(3, {"device:B", "volume:B"}, never));

    // Job 4 waits for job 1, which reads device A; job 5, started after it,
    // waits behind it, and so takes its turn after job 4 even once job 1 is done.
    std::atomic<bool> fourth{false};
    std::atomic<bool> fifth{false};
    std::atomic<std::uint64_t> fourth_waits_for{0};
    std::thread four([&] {
        CHECK(queue.take(4, {"volume:X", "device:A"}, never,
            [&](std::uint64_t reading) { fourth_waits_for = reading; }));
        fourth = true;
    });
    for (int tries = 0; tries < 200 && fourth_waits_for == 0; ++tries) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(fourth_waits_for == 1 && !fourth);
    std::thread five([&] {
        CHECK(queue.take(5, {"device:A"}, never));
        fifth = true;
    });
    std::this_thread::sleep_for(300ms);
    CHECK(!fourth && !fifth);
    queue.give_back(1);
    CHECK(soon(fourth));
    std::this_thread::sleep_for(300ms);
    CHECK(!fifth);
    queue.give_back(4);
    CHECK(soon(fifth));
    four.join();
    five.join();
    queue.give_back(5);
    queue.give_back(3);
    queue.give_back(2);

    // A job stopped while it waits gives up its place, holding nothing.
    CHECK(queue.take(6, {"device:A"}, never));
    va::CancelToken stop;
    std::atomic<bool> gave_up{false};
    std::thread seven([&] {
        CHECK(!queue.take(7, {"device:A"}, stop));
        gave_up = true;
    });
    std::this_thread::sleep_for(100ms);
    stop.cancel();
    CHECK(soon(gave_up));
    seven.join();
    queue.give_back(6);
    CHECK(queue.take(8, {"device:A"}, never));
    queue.give_back(8);
    // One stopped before it asked takes nothing, also of no device.
    CHECK(!queue.take(9, {"device:A"}, stop) && !queue.take(10, {}, stop));
    CHECK(queue.take(11, {"device:A"}, never));
    queue.give_back(11);
}

void a_jobs_operation_is_told()
{
    using va::gui::JobTarget;
    using va::gui::runs_operation_of;
    const JobTarget by_root{std::filesystem::path("/media/SAMPLE"), std::nullopt};
    const JobTarget by_archive{std::nullopt, std::string("c0ffee00-0000-4000-8000-000000000001")};
    // A registration's plan and a verification's record name their root, and
    // a verification's its archive too.
    CHECK(runs_operation_of(by_root, {{"root", "/media/SAMPLE"}}));
    CHECK(runs_operation_of(by_root, {{"root", "/media/SAMPLE/"}}));
    CHECK(runs_operation_of(by_root, {{"root", "/media/./SAMPLE"}}));
    CHECK(!runs_operation_of(by_root, {{"root", "/media/SAMPLE-2"}}));
    CHECK(!runs_operation_of(by_root, {{"archive_uuid", "c0ffee00-0000-4000-8000-000000000001"}}));
    CHECK(runs_operation_of(by_archive,
        {{"root", "/media/OTHER"}, {"archive_uuid", "c0ffee00-0000-4000-8000-000000000001"}}));
    CHECK(!runs_operation_of(by_archive, {{"archive_uuid", "c0ffee00-0000-4000-8000-000000000002"}}));
    CHECK(!runs_operation_of(by_archive, nlohmann::json("not a record")));
    CHECK(!runs_operation_of(JobTarget{}, {{"root", ""}, {"archive_uuid", ""}}));
}

// Two jobs of one window are two verifications of one repository at once, in
// one process, each with a store of its own, as Jobs runs them: both reach
// their result, and the history chain holds both runs.
void two_verifications_of_one_repository_at_once()
{
    test::TestRepository repository;
    struct Carrier {
        test::FakeFileSystem files;
        test::FakeMediaEnumerator media;
        explicit Carrier(const std::filesystem::path& root) : files(root)
        {
            for (int index = 0; index < 50; ++index) {
                files.add_file("SAMPLE-" + std::to_string(index) + ".bin",
                    std::string(4096, static_cast<char>('a' + index % 26)));
            }
        }
    };
    Carrier first("/vol-a");
    Carrier second("/vol-b");
    first.media.volume = test::fake_volume("/vol-a");
    second.media.volume = test::fake_volume("/vol-b");
    second.media.volume.filesystem_uuid = "5555666677778888";
    second.media.volume.filesystem_uuid_short = "77778888";
    second.media.volume.devices.front().serial = "SERIAL-2";
    const va::HostInfo host{"windows", "test-host", "0.1.0"};
    test::FakeProcessTable processes;
    for (auto* carrier : {&first, &second}) {
        auto store = repository.store();
        test::FakeMediaWriter media_writer{carrier->files};
        va::RegisterServices services{carrier->files, *repository.files, *repository.writer, media_writer,
            carrier->media, store, repository.context.repo, host, processes, {}};
        va::RegisterRequest request;
        request.root = carrier->files.root();
        CHECK(va::register_archive(services, request, test::not_cancelled(), test::no_progress()).has_value());
    }
    std::string results[2];
    const auto verify = [&](Carrier& carrier, std::string& result) {
        auto store = repository.store();
        test::FakeProcessTable own;
        va::VerifyServices services{carrier.files, *repository.files, carrier.media, store, repository.context.repo,
            repository.context.repository_uuid, host, *repository.writer, own};
        va::VerifyRequest request;
        request.root = carrier.files.root();
        va::CancelToken cancel;
        test::NullProgress progress;
        const auto outcome = va::verify_full(services, request, cancel, progress);
        result = outcome ? outcome->result : outcome.error().message;
    };
    std::thread one([&] { verify(first, results[0]); });
    std::thread two([&] { verify(second, results[1]); });
    one.join();
    two.join();
    CHECK(results[0] == "PASSED" && results[1] == "PASSED");
    auto store = repository.store();
    const auto chain = store.check_history(repository.context.repository_uuid);
    CHECK(chain && chain->runs == 2 && !chain->first_broken_run);
}

} // namespace

void job_queue_tests()
{
    devices_are_keyed();
    jobs_of_one_device_take_turns();
    a_jobs_operation_is_told();
    two_verifications_of_one_repository_at_once();
}
