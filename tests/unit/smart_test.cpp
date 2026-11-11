// SMART (decision 0052): the pages a device returns, decoded (ATA SMART READ
// DATA with its thresholds, the NVMe health log and identification); what a
// read says compared with the read of the same device before it (its alerts
// and hints: an alert stays while its count is above zero, a pending count
// back at zero is a hint once, CRC errors are a hint only, temperature says
// nothing); which reads Devices shows for a device described as the system
// describes it; the SMART alert that marks an archive; the words of the
// window in both languages; and the repository's record of the reads.
// Every page, model, serial, WWN and time here is SAMPLE DATA made up for
// the test.
#include "test_support.h"
#include "test_repository.h"
#include "engine_fixture.h"

#include "app/run_result.h"
#include "app/smart_words.h"
#include "app/text.h"
#include "core/repository/archive_store.h"
#include "core/smart/smart_data.h"
#include "gui/smart_rows.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using va::SmartFinding;
using va::SmartFindingKind;
using va::SmartRead;

// SAMPLE DATA: an attribute as a test gives it.
struct Attribute {
    std::uint8_t id;
    std::uint8_t value;
    std::uint8_t threshold;
    std::uint64_t raw;
};

// A 512-byte ATA page whose checksum holds: SMART READ DATA of `attributes`,
// or, with `thresholds`, SMART READ THRESHOLDS of them.
std::vector<std::uint8_t> ata_page(const std::vector<Attribute>& attributes, bool thresholds = false)
{
    std::vector<std::uint8_t> page(512, 0);
    page[0] = 0x10;
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const auto& attribute = attributes[index];
        const std::size_t at = 2 + 12 * index;
        page[at] = attribute.id;
        if (thresholds) {
            page[at + 1] = attribute.threshold;
            continue;
        }
        page[at + 1] = 0x03;
        page[at + 3] = attribute.value;
        page[at + 4] = attribute.value;
        for (std::size_t byte = 0; byte < 6; ++byte) {
            page[at + 5 + byte] = static_cast<std::uint8_t>(attribute.raw >> (8 * byte));
        }
    }
    unsigned sum = 0;
    for (std::size_t index = 0; index < 511; ++index) {
        sum += page[index];
    }
    page[511] = static_cast<std::uint8_t>(-sum & 0xff);
    return page;
}

// The usual attributes of a sample disk, with the counts given.
std::vector<Attribute> disk(std::uint64_t reallocated, std::uint64_t pending, std::uint64_t crc = 0,
    std::uint64_t offline = 0)
{
    return {{0x01, 100, 16, 0}, {0x05, 100, 5, reallocated}, {0x09, 90, 0, 1234}, {0xC2, 70, 0, 0x0000'2400'1e00'0024},
        {0xC5, 100, 0, pending}, {0xC6, 100, 0, offline}, {0xC7, 200, 0, crc}};
}

va::PhysicalDeviceInfo seen(std::string serial, std::string model = "SAMPLE ENCLOSURE")
{
    va::PhysicalDeviceInfo device;
    device.device_ref = "\\\\.\\PhysicalDrive9";
    device.model = std::move(model);
    device.serial = std::move(serial);
    device.bus = "usb";
    device.capacity_bytes = 4'000'000'000'000;
    return device;
}

SmartRead ata_read(std::string at, const std::vector<Attribute>& attributes, std::string own = "SAMPLE-OWN-0001",
    std::string enclosure = "SAMPLE-ENCLOSURE-A")
{
    SmartRead read;
    read.read_at = std::move(at);
    read.seen = seen(std::move(enclosure));
    read.device = va::DeviceIdentity{"SAMPLE MODEL", std::move(own), ""};
    read.how = "sat";
    read.data = ata_page(attributes);
    read.thresholds = ata_page(attributes, true);
    return read;
}

std::vector<std::uint8_t> nvme_log(std::uint8_t warning, std::uint8_t spare, std::uint8_t threshold,
    std::uint64_t media_errors)
{
    std::vector<std::uint8_t> log(512, 0);
    log[0] = warning;
    log[1] = 0x3a;  // 314 K: 41 °C
    log[2] = 0x01;
    log[3] = spare;
    log[4] = threshold;
    log[5] = 7;
    log[48] = 0x10;  // data units written: 16
    log[128] = 0xe8;  // power-on hours: 1000
    log[129] = 0x03;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        log[160 + byte] = static_cast<std::uint8_t>(media_errors >> (8 * byte));
    }
    return log;
}

SmartRead nvme_read(std::string at, std::uint8_t warning, std::uint8_t spare, std::uint64_t media_errors)
{
    SmartRead read;
    read.read_at = std::move(at);
    read.seen = seen("SAMPLE-NVME-0001", "SAMPLE NVME");
    read.seen.bus = "nvme";
    read.device = va::DeviceIdentity{"SAMPLE NVME", "SAMPLE-NVME-0001", ""};
    read.how = "nvme";
    read.data = nvme_log(warning, spare, 10, media_errors);
    return read;
}

const SmartFinding* finding(const std::vector<SmartFinding>& findings, const std::string& code)
{
    for (const auto& found : findings) {
        if (found.code == code) {
            return &found;
        }
    }
    return nullptr;
}

void pages_are_decoded()
{
    const auto attributes = disk(8, 0);
    const auto decoded = va::decode_ata_attributes(ata_page(attributes), ata_page(attributes, true));
    CHECK(decoded && decoded->size() == attributes.size());
    if (decoded && decoded->size() == attributes.size()) {
        CHECK((*decoded)[1].id == 0x05 && (*decoded)[1].raw == 8 && (*decoded)[1].value == 100 &&
              (*decoded)[1].threshold == 5 && (*decoded)[1].prefailure());
        // A 48-bit raw value is read whole, little-endian.
        CHECK((*decoded)[3].raw == 0x0000'2400'1e00'0024);
    }
    // A page whose checksum does not hold, that is all zeros, or of another
    // size: nothing. Thresholds that do not check: none.
    auto broken = ata_page(attributes);
    broken[100] ^= 0x01;
    CHECK(!va::decode_ata_attributes(broken, {}));
    CHECK(!va::decode_ata_attributes(std::vector<std::uint8_t>(512, 0), {}));
    CHECK(!va::decode_ata_attributes(std::vector<std::uint8_t>(511, 1), {}));
    auto bad_thresholds = ata_page(attributes, true);
    bad_thresholds[3] ^= 0x01;
    const auto without = va::decode_ata_attributes(ata_page(attributes), bad_thresholds);
    CHECK(without && !without->front().threshold);

    // At or below a threshold of 1 to 253 fails; a threshold of 0 never.
    CHECK((va::SmartAttribute{0x05, 0, 5, 5, 0, 5}).failing());
    CHECK((va::SmartAttribute{0x05, 0, 4, 4, 0, 5}).failing());
    CHECK(!(va::SmartAttribute{0x05, 0, 6, 6, 0, 5}).failing());
    CHECK(!(va::SmartAttribute{0xC2, 0, 1, 1, 0, 0}).failing());
    CHECK(!(va::SmartAttribute{0xC2, 0, 1, 1, 0, 254}).failing());
    CHECK(!(va::SmartAttribute{0xC2, 0, 1, 1, 0, std::nullopt}).failing());

    // SMART supported and on, as the identification says (words 82, 85).
    std::vector<std::uint8_t> identify(512, 0);
    identify[164] = 0x01;
    identify[170] = 0x01;
    const auto on = va::ata_smart_support(identify);
    CHECK(on && on->supported && on->enabled);
    identify[170] = 0x00;
    identify[171] = 0x40;
    const auto off = va::ata_smart_support(identify);
    CHECK(off && off->supported && !off->enabled);
    CHECK(!va::ata_smart_support(std::vector<std::uint8_t>(4096, 0)));

    // The NVMe health log and identification.
    const auto health = va::decode_nvme_health(nvme_log(0x04, 50, 10, 3));
    CHECK(health && health->critical_warning == 0x04 && health->temperature_kelvin == 314 &&
          health->available_spare == 50 && health->spare_threshold == 10 && health->percentage_used == 7 &&
          health->data_units_written == 16 && health->power_on_hours == 1000 && health->media_errors == 3);
    auto huge = nvme_log(0, 100, 10, 0);
    huge[160 + 8] = 1;  // a media error count above 64 bits
    CHECK(va::decode_nvme_health(huge)->media_errors == UINT64_MAX);
    CHECK(!va::decode_nvme_health(std::vector<std::uint8_t>(512, 0)));
    std::vector<std::uint8_t> controller(4096, ' ');
    const std::string serial = "SAMPLE-NVME-SN";
    const std::string model = "SAMPLE NVME MODEL";
    std::copy(serial.begin(), serial.end(), controller.begin() + 4);
    std::copy(model.begin(), model.end(), controller.begin() + 24);
    controller[64] = 'F';
    controller[65] = 0x01;
    const auto named = va::decode_nvme_identify(controller);
    CHECK(named && named->serial == serial && named->model == model && named->firmware == "F?");
    CHECK(!va::decode_nvme_identify(std::vector<std::uint8_t>(4096, ' ')));
    CHECK(!va::decode_nvme_identify(std::vector<std::uint8_t>(512, 'A')));

    // What the window shows most: temperature (the low byte of 194) and
    // power-on hours.
    const auto values = va::smart_values(ata_read("2026-11-10T01:00:00Z", disk(0, 0)));
    CHECK(values && values->temperature_celsius() == 36 && values->power_on_hours() == 1234u);
    const auto nvme = va::smart_values(nvme_read("2026-11-10T01:00:00Z", 0, 100, 0));
    CHECK(nvme && nvme->temperature_celsius() == 41 && nvme->power_on_hours() == 1000u);
}

void reads_are_judged()
{
    const auto first = ata_read("2026-11-01T01:00:00Z", disk(0, 0, 3));
    // Pending sectors rose: an alert, with the count before.
    const auto rose = ata_read("2026-11-10T01:00:00Z", disk(0, 8, 3));
    const auto findings = va::smart_findings(rose, &first);
    const auto* pending = finding(findings, "pending-sectors");
    CHECK(pending && pending->kind == SmartFindingKind::Alert && pending->value == 8 && pending->before == 0u);
    CHECK(va::any_smart_alert(findings) && findings.size() == 1);
    // Without a read before it: the count alone.
    const auto alone = va::smart_findings(rose, nullptr);
    CHECK(finding(alone, "pending-sectors") && !finding(alone, "pending-sectors")->before);

    // Back at zero without a reallocation: no alert, a hint once.
    const auto cleared = ata_read("2026-11-20T01:00:00Z", disk(0, 0, 3));
    const auto back = va::smart_findings(cleared, &rose);
    CHECK(!va::any_smart_alert(back) && finding(back, "pending-sectors-cleared") &&
          finding(back, "pending-sectors-cleared")->kind == SmartFindingKind::Hint &&
          finding(back, "pending-sectors-cleared")->before == 8u);
    CHECK(va::smart_findings(cleared, &cleared).empty());
    // Back at zero because they were reallocated: the reallocated sectors
    // alert, and stay an alert while they stay.
    const auto reallocated = ata_read("2026-11-20T01:00:00Z", disk(8, 0, 3));
    const auto moved = va::smart_findings(reallocated, &rose);
    CHECK(va::any_smart_alert(moved) && finding(moved, "reallocated-sectors")->before == 0u &&
          finding(moved, "pending-sectors-cleared"));
    const auto same = va::smart_findings(ata_read("2026-12-01T01:00:00Z", disk(8, 0, 3)), &reallocated);
    CHECK(va::any_smart_alert(same) && finding(same, "reallocated-sectors")->value == 8 &&
          finding(same, "reallocated-sectors")->before == 8u);

    // CRC errors rising are a hint only; temperature never alerts.
    const auto crc = va::smart_findings(ata_read("2026-11-10T01:00:00Z", disk(0, 0, 12)), &first);
    CHECK(!va::any_smart_alert(crc) && finding(crc, "crc-errors-increased") &&
          finding(crc, "crc-errors-increased")->before == 3u && finding(crc, "crc-errors-increased")->value == 12);
    CHECK(va::smart_findings(first, nullptr).empty());

    // An attribute at or below its threshold, and offline uncorrectable
    // sectors; reported uncorrectable errors.
    auto failing = disk(0, 0, 0, 2);
    failing[0] = {0x01, 10, 16, 0};
    failing.push_back({0xBB, 100, 0, 1});
    const auto worn = va::smart_findings(ata_read("2026-11-10T01:00:00Z", failing), nullptr);
    CHECK(finding(worn, "attribute-failing") && finding(worn, "attribute-failing")->attribute == 0x01);
    CHECK(finding(worn, "offline-uncorrectable") && finding(worn, "reported-uncorrectable"));

    // NVMe: the spare below its threshold, another critical warning, media
    // errors; a healthy log says nothing.
    CHECK(va::smart_findings(nvme_read("2026-11-10T01:00:00Z", 0, 100, 0), nullptr).empty());
    const auto low = va::smart_findings(nvme_read("2026-11-10T01:00:00Z", 0x01, 5, 0), nullptr);
    CHECK(finding(low, "spare-low") && finding(low, "spare-low")->value == 5 && !finding(low, "critical-warning"));
    const auto warned = va::smart_findings(nvme_read("2026-11-10T01:00:00Z", 0x0c, 100, 0), nullptr);
    CHECK(finding(warned, "critical-warning") && finding(warned, "critical-warning")->warning_bits == 0x0c);
    const auto earlier = nvme_read("2026-11-01T01:00:00Z", 0, 100, 1);
    const auto errors = va::smart_findings(
        nvme_read("2026-11-10T01:00:00Z", 0, 100, 4), &earlier);
    CHECK(finding(errors, "media-errors") && finding(errors, "media-errors")->before == 1u);

    // A read without data that decodes says nothing.
    auto failed = first;
    failed.data.clear();
    failed.problem = "smart-not-passed";
    CHECK(va::smart_findings(failed, nullptr).empty());
}

void reads_of_a_device_are_found()
{
    // Three reads of one device behind enclosure A, the last one failed; one
    // after the device was moved into enclosure B; one of another device.
    auto failed = ata_read("2026-11-15T01:00:00Z", {});
    failed.data.clear();
    failed.thresholds.clear();
    failed.problem = "no-answer";
    const std::vector<SmartRead> reads{
        ata_read("2026-11-01T01:00:00Z", disk(0, 0)),
        ata_read("2026-11-10T01:00:00Z", disk(0, 8)),
        failed,
        ata_read("2026-11-20T01:00:00Z", disk(0, 9), "SAMPLE-OWN-0001", "SAMPLE-ENCLOSURE-B"),
        ata_read("2026-11-21T01:00:00Z", disk(5, 0), "SAMPLE-OWN-0002", "SAMPLE-ENCLOSURE-C"),
    };
    const auto behind_a = va::device_smart(reads, seen("SAMPLE-ENCLOSURE-A"));
    CHECK(behind_a.latest == &reads[1] && behind_a.previous == &reads[0] && behind_a.failed == &reads[2]);
    CHECK(behind_a.alerts() && finding(behind_a.findings, "pending-sectors")->before == 0u);
    // The history follows the device by its own serial, newest first, also
    // behind another enclosure.
    CHECK(behind_a.history.size() == 3 && behind_a.history[0] == &reads[3] && behind_a.history[2] == &reads[0]);
    const auto behind_b = va::device_smart(reads, seen("SAMPLE-ENCLOSURE-B"));
    CHECK(behind_b.latest == &reads[3] && behind_b.previous == &reads[1] && !behind_b.failed);
    CHECK(finding(behind_b.findings, "pending-sectors")->before == 8u);
    // A device described otherwise (another capacity, or no serial): no read.
    auto other = seen("SAMPLE-ENCLOSURE-A");
    other.capacity_bytes = 1;
    CHECK(!va::device_smart(reads, other).read());
    CHECK(!va::device_smart(reads, seen("")).read());
    // A device whose only read failed.
    const std::vector<SmartRead> only_failed{failed};
    const auto never = va::device_smart(only_failed, seen("SAMPLE-ENCLOSURE-A"));
    CHECK(never.read() && !never.latest && never.failed == &only_failed[0] && !never.alerts());

    // An archive is marked by the first of its devices whose latest read
    // alerts.
    const auto alert = va::archive_smart_alert(reads, {seen("SAMPLE-ENCLOSURE-C"), seen("SAMPLE-ENCLOSURE-A")});
    CHECK(alert && alert->read_at == "2026-11-21T01:00:00Z" && alert->finding.code == "reallocated-sectors" &&
          alert->more == 0);
    CHECK(!va::archive_smart_alert(reads, {seen("SAMPLE-ENCLOSURE-Z")}));

    // Behind an enclosure that hides the device's identity (operator,
    // 2026-11-10): what the system tells is the enclosure's, so no read is
    // taken for the device inside until its own identity is read again; then
    // its reads are those of that device, by its own serial, behind any
    // enclosure.
    auto hidden = seen("SAMPLE-ENCLOSURE-A");
    hidden.enclosure_hides_identity = true;
    auto behind_hiding = reads;
    for (auto& read : behind_hiding) {
        read.seen.enclosure_hides_identity = true;
    }
    const auto unknown = va::device_smart(behind_hiding, hidden);
    CHECK(unknown.unknown_device && !unknown.read() && !unknown.alerts() && !unknown.latest);
    const va::DeviceIdentity own{"SAMPLE MODEL", "SAMPLE-OWN-0001", ""};
    const auto known = va::device_smart(behind_hiding, hidden, &own);
    CHECK(!known.unknown_device && known.latest == &behind_hiding[3] && known.previous == &behind_hiding[1] &&
          known.alerts());
    const va::DeviceIdentity another{"SAMPLE MODEL", "SAMPLE-OWN-0009", ""};
    CHECK(!va::device_smart(behind_hiding, hidden, &another).read());
    // A read made behind an enclosure that hid the device's identity is not
    // found by the enclosure's description alone, even where the device is
    // described now without the flag.
    CHECK(!va::device_smart(behind_hiding, seen("SAMPLE-ENCLOSURE-A")).read());
    CHECK(!va::archive_smart_alert(behind_hiding, {hidden}));
    CHECK(!va::archive_smart_alert(behind_hiding, {seen("SAMPLE-ENCLOSURE-C")}));
    const auto by_own = va::archive_smart_alert(behind_hiding, {hidden}, {own});
    CHECK(by_own && by_own->finding.code == "pending-sectors" && by_own->read_at == "2026-11-20T01:00:00Z");
}

void reads_are_worded()
{
    const va::app::Text english(va::app::Language::English);
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const SmartFinding rose{SmartFindingKind::Alert, "pending-sectors", 0xC5, 8, 0u, 0};
    CHECK(va::app::finding_words(english, rose) == "Pending sectors increased (0 → 8)");
    CHECK(va::app::finding_words(chinese, rose) == "待處理的磁區增加（0 → 8）");
    const SmartFinding same{SmartFindingKind::Alert, "reallocated-sectors", 0x05, 8, 8u, 0};
    CHECK(va::app::finding_words(english, same) == "Reallocated sectors: 8 (as in the last read)");
    CHECK(va::app::finding_words(chinese, same) == "已重新配置的磁區：8（和上次相同）");
    const SmartFinding fewer{SmartFindingKind::Alert, "pending-sectors", 0xC5, 3, 8u, 0};
    CHECK(va::app::finding_words(chinese, fewer) == "待處理的磁區：3（上次 8）");
    const SmartFinding cleared{SmartFindingKind::Hint, "pending-sectors-cleared", 0xC5, 0, 8u, 0};
    CHECK(va::app::finding_words(english, cleared) == "Pending sectors cleared (8 → 0)");
    CHECK(va::app::finding_words(chinese, cleared) == "待處理的磁區已清除（8 → 0）");
    const SmartFinding crc{SmartFindingKind::Hint, "crc-errors-increased", 0xC7, 12, 3u, 0};
    CHECK(va::app::finding_words(chinese, crc) ==
          "UltraDMA CRC 錯誤增加（3 → 12）：通常是傳輸線或外接盒的問題，不是裝置本身");
    const SmartFinding failing{SmartFindingKind::Alert, "attribute-failing", 0x01, 10, std::nullopt, 0};
    CHECK(va::app::finding_words(english, failing) == "Raw read error rate (01) is at or below its threshold");
    const SmartFinding warned{SmartFindingKind::Alert, "critical-warning", 0, 0x0c, std::nullopt, 0x0c};
    CHECK(va::app::finding_words(chinese, warned) == "裝置回報嚴重警告：可靠度下降、唯讀");
    CHECK(va::app::attribute_name(english, 0xE7) == "Attribute 231" && va::app::attribute_id(0xE7) == "E7");
    CHECK(va::app::smart_how_words(chinese, "sat") == "經由外接盒（SAT）");
    SmartRead not_passed;
    not_passed.problem = "not-passed";
    CHECK(va::app::smart_problem_words(chinese, not_passed) == "外接盒不會把 ATA 指令轉送給裝置。");

    // Devices: the section of a device whose latest read alerts.
    const std::vector<SmartRead> reads{
        ata_read("2026-11-01T01:00:00Z", disk(0, 0)), ata_read("2026-11-10T01:00:00Z", disk(0, 8))};
    const auto section = va::gui::smart_section(chinese, reads, seen("SAMPLE-ENCLOSURE-A"), true);
    CHECK(section.pill.words == "SMART 警訊" && section.pill.tone == va::app::Tone::Error);
    CHECK(section.alerts.size() == 2 && section.alerts.front() == "⚠ 待處理的磁區增加（0 → 8）");
    CHECK(section.read.find("經由外接盒（SAT）") != std::string::npos);
    CHECK(section.attributes_title == "所有 SMART 屬性（7）" && section.attributes.rows.size() == 7);
    CHECK(section.history_title == "歷次讀取（2）" && section.history.rows.size() == 2 &&
          section.history.rows.front()[2] == "8");
    const auto unread = va::gui::smart_section(english, reads, seen("SAMPLE-ENCLOSURE-Z"), true);
    CHECK(unread.pill.words == "SMART not read" && unread.attributes.rows.empty());
    const auto healthy = va::gui::smart_section(english, {reads.front()}, seen("SAMPLE-ENCLOSURE-A"), true);
    CHECK(healthy.pill.words == "SMART OK" && healthy.alerts.empty());
    // Behind an enclosure that hides the device's identity, without a read of
    // its own identity now: nothing of the reads before.
    auto hidden_device = seen("SAMPLE-ENCLOSURE-A");
    hidden_device.enclosure_hides_identity = true;
    const auto to_read = va::gui::smart_section(chinese, reads, hidden_device, true);
    CHECK(to_read.pill.words == "SMART 需重新讀取" && to_read.pill.tone == va::app::Tone::Neutral &&
          to_read.alerts.empty() && to_read.values.empty() &&
          to_read.problem == "這個外接盒不會告訴系統裡面是哪一個裝置，所以不顯示之前讀到的 SMART：按「讀取所有裝置的 "
                             "SMART…」重新讀取。");
    const va::DeviceIdentity own{"SAMPLE MODEL", "SAMPLE-OWN-0001", ""};
    const auto read_again = va::gui::smart_section(chinese, reads, hidden_device, true, &own);
    CHECK(read_again.pill.words == "SMART 警訊");

    // Archives: the mark beside the result, and the line of the details.
    const va::ArchiveSmartAlert alert{"2026-11-10T01:00:00Z", rose, 1};
    CHECK(va::gui::smart_alert_mark(chinese, alert) == "⚠ SMART：待處理的磁區增加（0 → 8）（+1）");
    CHECK(va::gui::smart_alert_mark(english, alert) == "⚠ SMART: Pending sectors increased (0 → 8) (+1)");
    CHECK(va::gui::smart_alert_line(english, alert).find("1 more alerts. Devices shows all of it.") !=
          std::string::npos);
}

void reads_are_recorded()
{
    test::TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    CHECK(store.has_value());
    if (!store) {
        return;
    }
    auto failed = ata_read("2026-11-15T01:00:00Z", {});
    failed.data.clear();
    failed.thresholds.clear();
    failed.problem = "not-passed";
    failed.detail = "the device's enclosure passes no ATA command on to the device";
    std::vector<SmartRead> reads{ata_read("2026-11-10T01:00:00Z", disk(0, 8)), failed};
    reads.front().identify = std::vector<std::uint8_t>(512, 0x5a);
    reads.front().firmware = "SAMP01";
    CHECK(store->record_smart_reads(reads).has_value());
    CHECK(reads[0].id > 0 && reads[1].id > reads[0].id);
    // Kept by a rebuild of the index, which clears only the index.
    CHECK(store->clear_index().has_value());
    const auto back = store->smart_reads();
    CHECK(back && back->size() == 2);
    if (back && back->size() == 2) {
        const auto& read = back->front();
        CHECK(read.id == reads[0].id && read.read_at == "2026-11-10T01:00:00Z" && read.how == "sat" &&
              read.firmware == "SAMP01" && read.device.serial == "SAMPLE-OWN-0001" &&
              read.seen.serial == "SAMPLE-ENCLOSURE-A" && read.seen.capacity_bytes == 4'000'000'000'000 &&
              read.seen.bus == "usb" && read.identify == reads[0].identify && read.data == reads[0].data &&
              read.thresholds == reads[0].thresholds);
        CHECK((*back)[1].problem == "not-passed" && (*back)[1].data.empty() && (*back)[1].detail == failed.detail);
        // What Devices finds again from the record.
        CHECK(va::device_smart(*back, seen("SAMPLE-ENCLOSURE-A")).alerts());
    }
}


// Decision 0052: a FULL verification given the SMART reads that the window
// made as it was started records those of the devices of the volume it reads
// in its counts, hash-chained; the reads of another device are left out; a
// verification given none records none; the result page lists and alerts
// them.
void verifications_record_smart()
{
    test::EngineFixture fixture;
    auto& device = fixture.media.volume.devices.front();
    device.device_ref = "SAMPLE-DEVICE-REF-SMART";
    fixture.archive.add_file("SAMPLE-docs/report.txt", "sample report");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    auto own = ata_read("2026-11-11T01:00:00Z", disk(0, 8));
    own.seen = device;
    auto other = ata_read("2026-11-11T01:00:00Z", disk(0, 0), "SAMPLE-OWN-0002", "SAMPLE-ENCLOSURE-OTHER");
    other.seen.device_ref = "SAMPLE-DEVICE-REF-OTHER";
    va::VerifyRequest request;
    request.archive_uuid = registered->archive_uuid;
    request.smart_reads = {own, other};
    const auto outcome = fixture.verify(request);
    CHECK(outcome && outcome->result == "PASSED" && outcome->smart.size() == 1 &&
          outcome->smart.front().device.serial == "SAMPLE-OWN-0001");
    const auto runs = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(runs && !runs->empty());
    if (runs && !runs->empty()) {
        const auto recorded = va::recorded_smart_reads(runs->back().counts_json);
        CHECK(recorded.size() == 1 && recorded.front().data == own.data &&
              recorded.front().thresholds == own.thresholds && recorded.front().read_at == own.read_at &&
              recorded.front().seen.serial == device.serial && recorded.front().how == "sat");
        // The result page lists it with the devices, and alerts.
        const va::app::Text english(va::app::Language::English);
        va::app::RecordedRun shown;
        shown.run = runs->back();
        const auto page = va::app::result_page(english, shown);
        CHECK(std::ranges::any_of(page.identity, [](const va::app::Field& field) {
            return field.name == "SMART" && field.value.starts_with("SMART alert: Pending sectors: 8");
        }));
        CHECK(std::ranges::any_of(
            page.alerts, [](const std::string& line) { return line.contains("SMART of SAMPLE MODEL (serial SAMPLE-OWN-0001)"); }));
    }
    const auto checked = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(checked && !checked->first_broken_run);

    va::VerifyRequest without;
    without.archive_uuid = registered->archive_uuid;
    const auto plain = fixture.verify(without);
    CHECK(plain && plain->smart.empty());
    const auto after = fixture.store.verify_runs(registered->archive_uuid);
    CHECK(after && va::recorded_smart_reads(after->back().counts_json).empty());

    // The JSON form round trips, a missing or garbled member reading as empty.
    const auto back = va::smart_reads_from_json(va::smart_reads_json({own}));
    CHECK(back.size() == 1 && back.front().data == own.data && back.front().device.serial == own.device.serial);
    CHECK(va::smart_reads_from_json(nlohmann::json::object()).empty());
    CHECK(va::recorded_smart_reads("not json").empty());
}
} // namespace

void smart_tests()
{
    pages_are_decoded();
    reads_are_judged();
    reads_of_a_device_are_found();
    reads_are_worded();
    reads_are_recorded();
    verifications_record_smart();
}
