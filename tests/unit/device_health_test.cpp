// What a device reports of its own health (decision 0044): Windows' answer to
// StorageDeviceManagementStatus and udisks2's SMART properties as the
// platforms read them (both parsers tested on every system), which healths
// are device alerts, how media evidence keeps them, and their words. Every
// device and answer here is sample data made up for the test.
#include "test_support.h"

#include "engine_fixture.h"
#include "fakes.h"

#include "app/device_alerts.h"
#include "app/text.h"
#include "core/engine/device_alerts.h"
#include "core/identity/volume_json.h"
#include "platform/linux/udisks_health.h"
#include "platform/media_enumerator.h"
#include "platform/windows/device_health_status.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

using va::HealthStatus;

// STORAGE_DEVICE_MANAGEMENT_STATUS as the IOCTL writes it: version, size,
// health, the number of operational statuses, of additional reasons, then
// the statuses; little-endian 4-byte fields.
std::vector<std::byte> management_status(std::uint32_t version, std::uint32_t health,
    const std::vector<std::uint32_t>& statuses, std::uint32_t claimed = 0)
{
    std::vector<std::uint32_t> fields{version, 84, health, claimed ? claimed : static_cast<std::uint32_t>(statuses.size()), 0};
    fields.insert(fields.end(), statuses.begin(), statuses.end());
    std::vector<std::byte> bytes(fields.size() * 4);
    std::memcpy(bytes.data(), fields.data(), bytes.size());
    return bytes;
}

void windows_answers()
{
    using va::platform::windows::health_from_management_status;
    // What this program's developer saw from every disk of a healthy computer.
    const auto healthy = health_from_management_status(management_status(100, 3, {2}));
    CHECK(healthy.status == HealthStatus::Healthy && healthy.conditions.empty());

    const auto warning = health_from_management_status(management_status(100, 2, {3}));
    CHECK(warning.status == HealthStatus::Warning);
    CHECK(warning.conditions == std::vector<std::string>{"predicting-failure"});

    const auto unhealthy = health_from_management_status(management_status(100, 1, {5, 6, 2}));
    CHECK(unhealthy.status == HealthStatus::Unhealthy);
    CHECK((unhealthy.conditions == std::vector<std::string>{"hardware-error", "not-usable"}));

    // A transient error and a repair under way, from a disk of unknown health.
    const auto unknown = health_from_management_status(management_status(100, 0, {7, 4}));
    CHECK(unknown.status == HealthStatus::Unknown);
    CHECK((unknown.conditions == std::vector<std::string>{"transient-error", "in-service"}));

    // A later version keeps these fields; an older one, or an answer too
    // short to hold the health, is Unknown.
    CHECK(health_from_management_status(management_status(120, 3, {2})).status == HealthStatus::Healthy);
    CHECK(health_from_management_status(management_status(99, 3, {2})).status == HealthStatus::Unknown);
    auto short_answer = management_status(100, 2, {3});
    short_answer.resize(8);
    CHECK(health_from_management_status(short_answer).status == HealthStatus::Unknown);
    CHECK(health_from_management_status({}).status == HealthStatus::Unknown);

    // A count of statuses beyond what was written reads only what was.
    const auto claimed = health_from_management_status(management_status(100, 2, {3}, 16));
    CHECK(claimed.status == HealthStatus::Warning && claimed.conditions.size() == 1);
}

void udisks_properties()
{
    using va::platform::linux_health::AtaSmart;
    using va::platform::linux_health::health_from_ata;
    using va::platform::linux_health::health_from_nvme;

    AtaSmart smart;
    smart.supported = true;
    smart.enabled = true;
    smart.updated = 1'790'000'000;
    CHECK(health_from_ata(smart).status == HealthStatus::Healthy);

    auto failing = smart;
    failing.failing = true;
    failing.bad_sectors = 12;
    const auto failed = health_from_ata(failing);
    CHECK(failed.status == HealthStatus::Unhealthy);
    CHECK((failed.conditions == std::vector<std::string>{"predicting-failure", "bad-sectors"}));

    auto attributes = smart;
    attributes.attributes_failing = 1;
    const auto past_threshold = health_from_ata(attributes);
    CHECK(past_threshold.status == HealthStatus::Warning);
    CHECK(past_threshold.conditions == std::vector<std::string>{"attributes-failing"});

    // Unknown counts (-1) are no condition.
    auto unknown_counts = smart;
    unknown_counts.attributes_failing = -1;
    unknown_counts.bad_sectors = -1;
    CHECK(health_from_ata(unknown_counts).status == HealthStatus::Healthy);

    // SMART that is not supported, not enabled or not read yet tells nothing.
    for (const auto change : {0, 1, 2}) {
        auto told_nothing = failing;
        told_nothing.supported = change != 0;
        told_nothing.enabled = change != 1;
        told_nothing.updated = change == 2 ? 0 : smart.updated;
        const auto health = health_from_ata(told_nothing);
        CHECK(health.status == HealthStatus::Unknown && health.conditions.empty());
    }

    CHECK(health_from_nvme(1'790'000'000, {}).status == HealthStatus::Healthy);
    CHECK(health_from_nvme(0, {"degraded"}).status == HealthStatus::Unknown);
    const auto spare = health_from_nvme(1'790'000'000, {"spare", "temperature"});
    CHECK(spare.status == HealthStatus::Warning);
    CHECK((spare.conditions == std::vector<std::string>{"spare-low", "temperature"}));
    const auto degraded = health_from_nvme(1'790'000'000, {"readonly", "pmr_readonly", "degraded", "volatile_mem"});
    CHECK(degraded.status == HealthStatus::Unhealthy);
    CHECK((degraded.conditions == std::vector<std::string>{"read-only", "degraded", "volatile-memory-failed"}));
    const auto later = health_from_nvme(1'790'000'000, {"something_new"});
    CHECK(later.status == HealthStatus::Warning && later.conditions == std::vector<std::string>{"something_new"});
}

void which_health_alerts()
{
    CHECK(!va::health_alerts({}));
    CHECK(!va::health_alerts({HealthStatus::Healthy, {}}));
    CHECK(va::health_alerts({HealthStatus::Warning, {}}));
    CHECK(va::health_alerts({HealthStatus::Unhealthy, {}}));
    // A condition of failure alerts whatever the status says.
    CHECK(va::health_alerts({HealthStatus::Unknown, {"predicting-failure"}}));
    CHECK(va::health_alerts({HealthStatus::Healthy, {"transient-error"}}));
    // A repair under way is none.
    CHECK(!va::health_alerts({HealthStatus::Healthy, {"in-service"}}));
}

va::VolumeInfo sample_volume()
{
    va::VolumeInfo volume;
    volume.mount_point = "/media/SAMPLE";
    volume.filesystem_type = "ext4";
    volume.filesystem_uuid = "c0ffee00-0000-4000-8000-000000000044";
    va::PhysicalDeviceInfo first;
    first.device_ref = "/dev/sdx";
    first.model = "SAMPLE DISK";
    first.serial = "SAMPLE-SERIAL-1";
    va::PhysicalDeviceInfo second = first;
    second.device_ref = "/dev/sdy";
    second.serial = "SAMPLE-SERIAL-2";
    volume.devices = {first, second};
    return volume;
}

void observed_and_kept()
{
    test::FakeMediaEnumerator media;
    media.health["SAMPLE-SERIAL-2"] = {HealthStatus::Warning, {"predicting-failure"}};
    auto volume = sample_volume();
    // Not asked: the evidence keeps no health.
    CHECK(!va::volume_to_json(volume)["devices"][0].contains("health"));

    va::observe_device_health(media, volume);
    CHECK(media.health_asked == 2);
    CHECK(volume.devices[0].health && volume.devices[0].health->status == HealthStatus::Unknown);
    CHECK(volume.devices[1].health && volume.devices[1].health->status == HealthStatus::Warning);

    const auto json = va::volume_to_json(volume);
    CHECK(json["devices"][1]["health"] ==
          (nlohmann::json{{"status", "warning"}, {"conditions", {"predicting-failure"}}}));
    CHECK(json["devices"][0]["health"]["status"] == "unknown");
    const auto read_back = va::volume_from_json(json);
    CHECK(read_back.devices[1].health && read_back.devices[1].health->status == HealthStatus::Warning);
    CHECK(read_back.devices[1].health->conditions == std::vector<std::string>{"predicting-failure"});
    CHECK(read_back.devices[0].health && read_back.devices[0].health->status == HealthStatus::Unknown);
    // Evidence recorded before decision 0044 has none.
    CHECK(!va::volume_from_json(va::volume_to_json(sample_volume())).devices[0].health);
    CHECK(va::health_from_json(nlohmann::json{{"status", "on fire"}}).status == HealthStatus::Unknown);
}

void words()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(va::app::health_words(english, {HealthStatus::Healthy, {}}) == "healthy");
    CHECK(va::app::health_words(english, {}) == "cannot be read");
    CHECK(va::app::health_words(english, {HealthStatus::Warning, {"predicting-failure", "bad-sectors"}}) ==
          "warning (predicting failure, bad sectors)");
    CHECK(va::app::health_words(english, {HealthStatus::Unhealthy, {"something_new"}}) ==
          "unhealthy (something_new)");
    CHECK(va::app::health_words(chinese, {HealthStatus::Healthy, {}}) == "健康");
    CHECK(va::app::health_words(chinese, {}) == "無法讀取");
    CHECK(va::app::health_words(chinese, {HealthStatus::Warning, {"predicting-failure", "bad-sectors"}}) ==
          "警告（預測即將故障、有損壞的磁區）");
    CHECK(chinese.format("health: {}", va::app::health_words(chinese, {HealthStatus::Unhealthy, {}})) ==
          "健康狀態：不健康");
}

// A registration and a verification ask each device of the volume for its
// health before they read anything of the archive, tell it at once, keep it
// with their media evidence (a verification also in its counts, in the hash
// chain), and their result stays what the content says.
void operations_ask_the_devices()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("SAMPLE.txt", "sample content");
    fixture.media.health["SERIAL-1"] = {HealthStatus::Warning, {"predicting-failure"}};
    std::vector<va::PhysicalDeviceInfo> told;
    std::uint64_t read_when_told = 999;
    std::size_t times_told = 0;
    const auto observed = [&](const std::vector<va::PhysicalDeviceInfo>& devices) {
        told = devices;
        read_when_told = fixture.archive.bytes_read;
        ++times_told;
    };
    const auto warned = [](const std::vector<va::PhysicalDeviceInfo>& devices) {
        return devices.size() == 1 && devices[0].serial == "SERIAL-1" && devices[0].health &&
               devices[0].health->status == HealthStatus::Warning &&
               devices[0].health->conditions == std::vector<std::string>{"predicting-failure"};
    };
    const auto last_evidence = [&] {
        return fixture.repository.query("SELECT volume_json FROM media_evidence ORDER BY id DESC LIMIT 1");
    };

    va::RegisterRequest registration;
    registration.root = fixture.archive.root();
    registration.devices_observed = observed;
    const auto registered = fixture.register_root(registration);
    CHECK(registered && warned(registered->devices));
    CHECK(times_told == 1 && warned(told) && read_when_told == 0);
    CHECK(last_evidence().find(R"("health":{"conditions":["predicting-failure"],"status":"warning"})") !=
          std::string::npos);

    va::VerifyRequest verification;
    verification.root = fixture.archive.root();
    verification.devices_observed = observed;
    const auto read_before = fixture.archive.bytes_read;
    const auto verified = fixture.verify(verification);
    CHECK(verified && verified->result == "PASSED");
    CHECK(verified && warned(verified->devices));
    CHECK(times_told == 2 && warned(told) && read_when_told == read_before);
    if (verified) {
        const auto counts = fixture.repository.query(
            "SELECT counts_json FROM verify_runs WHERE run_id = '" + verified->run_id + "'");
        CHECK(warned(va::recorded_device_health(counts)));
        CHECK(fixture.repository
                  .query("SELECT volume_json FROM media_evidence WHERE run_id = '" + verified->run_id + "'")
                  .find("predicting-failure") != std::string::npos);
    }
    CHECK(fixture.media.health_asked == 2);
}

// What a run's counts give back: nothing of a run recorded before decision
// 0044, and only what is written as it is written of counts changed by hand.
void recorded_health()
{
    CHECK(va::recorded_device_health(R"({"items":{"Verified":3}})").empty());
    CHECK(va::recorded_device_health("not json").empty());
    const auto changed = va::recorded_device_health(
        R"({"device_health":[{"model":5,"serial":"SAMPLE","health":{"status":7}},"x",{"wwn":"0x5000"}]})");
    CHECK(changed.size() == 2);
    if (changed.size() == 2) {
        CHECK(changed[0].model.empty() && changed[0].serial == "SAMPLE");
        CHECK(changed[0].health && changed[0].health->status == HealthStatus::Unknown);
        CHECK(changed[1].wwn == "0x5000" && changed[1].health && changed[1].health->status == HealthStatus::Unknown);
    }
    va::PhysicalDeviceInfo asked;
    asked.model = "SAMPLE DISK";
    asked.serial = "SAMPLE-SERIAL";
    asked.health = va::DeviceHealth{HealthStatus::Unhealthy, {"hardware-error"}};
    va::PhysicalDeviceInfo not_asked = asked;
    not_asked.health.reset();
    CHECK(va::device_health_json({asked, not_asked}).size() == 1);
    const auto back = va::recorded_device_health(nlohmann::json{{"device_health", va::device_health_json({asked})}}.dump());
    CHECK(back.size() == 1 && back[0].model == "SAMPLE DISK" && back[0].health &&
          back[0].health->status == HealthStatus::Unhealthy);
    CHECK(va::any_health_alerts(back) && !va::any_health_alerts({not_asked}));
}

void alert_words()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE DISK";
    device.serial = "SAMPLE-SERIAL-2";
    device.health = va::DeviceHealth{HealthStatus::Warning, {"predicting-failure"}};
    const auto english_lines = va::app::health_alert_lines(english, {device});
    CHECK(english_lines == std::vector<std::string>{
                               "⚠ Device alert: SAMPLE DISK (serial SAMPLE-SERIAL-2) reports its health as warning "
                               "(predicting failure). Copy the archives on it to another device soon, and verify the "
                               "copies."});
    CHECK(va::app::health_alert_lines(chinese, {device}) ==
          std::vector<std::string>{"⚠ 裝置警訊：SAMPLE DISK（序號 SAMPLE-SERIAL-2） 回報的健康狀態為「警告（預測即將故障）」。"
                                   "請盡快把其中的封存複製到另一個裝置，並驗證這些副本。"});
    // A healthy device, one that cannot tell, and one never asked are no alert.
    auto healthy = device;
    healthy.health = va::DeviceHealth{HealthStatus::Healthy, {}};
    auto untold = device;
    untold.health = va::DeviceHealth{};
    auto never_asked = device;
    never_asked.health.reset();
    CHECK(va::app::health_alert_lines(english, {healthy, untold, never_asked}).empty());

    CHECK(va::app::device_health_words(english, healthy) == "SAMPLE DISK (serial SAMPLE-SERIAL-2): healthy");
    CHECK(va::app::device_health_words(chinese, untold) == "SAMPLE DISK（序號 SAMPLE-SERIAL-2）：無法讀取");
    auto by_wwn = device;
    by_wwn.serial.clear();
    by_wwn.wwn = "0x5000c500a1b2c3d4";
    CHECK(va::app::device_name(english, by_wwn) == "SAMPLE DISK (WWN 0x5000c500a1b2c3d4)");
    auto unnamed = by_wwn;
    unnamed.wwn.clear();
    unnamed.model.clear();
    CHECK(va::app::device_name(english, unnamed) == "?");
}

// The platform's own question, of the devices that hold the test's temporary
// folder: it answers for each, whatever the device says (a CI runner's
// virtual disk may say nothing), and a device it cannot know is Unknown.
void asked_of_this_computer()
{
    const auto media = va::platform::make_media_enumerator();
    if (const auto volume = media->volume_for_path(std::filesystem::temp_directory_path())) {
        for (const auto& device : volume->devices) {
            const auto health = media->device_health(device);
            CHECK(!health || health->status == HealthStatus::Unknown || health->status == HealthStatus::Healthy ||
                  health->status == HealthStatus::Warning || health->status == HealthStatus::Unhealthy);
        }
    }
    va::PhysicalDeviceInfo nothing;
    nothing.device_ref = "SAMPLE-NOT-A-DEVICE";
    const auto unknown = media->device_health(nothing);
    CHECK(unknown && unknown->status == HealthStatus::Unknown && unknown->conditions.empty());
}

} // namespace

void device_health_tests()
{
    windows_answers();
    udisks_properties();
    which_health_alerts();
    observed_and_kept();
    words();
    operations_ask_the_devices();
    recorded_health();
    alert_words();
    asked_of_this_computer();
}
