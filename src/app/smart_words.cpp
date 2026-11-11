#include "app/smart_words.h"

#include "app/device_alerts.h"
#include "app/local_time.h"

#include <format>
#include <string>

namespace va::app {
namespace {

// The name of what a finding counts.
std::string counted_name(const Text& text, const SmartFinding& finding)
{
    if (finding.code == "media-errors") {
        return text("Media and data integrity errors");
    }
    if (finding.code == "spare-low") {
        return text("Available spare");
    }
    return attribute_name(text, finding.attribute);
}

// "Name increased (0 → 8)", "Name: 8 (as in the last read)", "Name: 3 (8 in
// the last read)", "Name: 8".
std::string counted_words(const Text& text, const std::string& name, const SmartFinding& finding)
{
    const auto value = std::to_string(finding.value);
    if (!finding.before) {
        return text.format("{}: {}", name, value);
    }
    const auto before = std::to_string(*finding.before);
    if (finding.value > *finding.before) {
        return text.format("{} increased ({} → {})", name, before, value);
    }
    if (finding.value == *finding.before) {
        return text.format("{}: {} (as in the last read)", name, value);
    }
    return text.format("{}: {} ({} in the last read)", name, value, before);
}

// The bits of an NVMe critical warning other than the spare's, in words.
std::string warning_bits_words(const Text& text, std::uint8_t bits)
{
    std::string words;
    const auto add = [&](const std::string& word) {
        if (!words.empty()) {
            words += text.language() == Language::TraditionalChinese ? "、" : ", ";
        }
        words += word;
    };
    if ((bits & 0x02) != 0) {
        add(text("temperature beyond its limit"));
    }
    if ((bits & 0x04) != 0) {
        add(text("reliability degraded"));
    }
    if ((bits & 0x08) != 0) {
        add(text("read-only"));
    }
    if ((bits & 0x10) != 0) {
        add(text("volatile memory backup failed"));
    }
    if ((bits & 0x20) != 0) {
        add(text("persistent memory read-only"));
    }
    return words;
}

} // namespace

std::string attribute_name(const Text& text, std::uint8_t id)
{
    switch (id) {
    case 0x01: return text("Raw read error rate");
    case 0x02: return text("Throughput performance");
    case 0x03: return text("Spin-up time");
    case 0x04: return text("Start/stop count");
    case 0x05: return text("Reallocated sectors");
    case 0x07: return text("Seek error rate");
    case 0x08: return text("Seek time performance");
    case 0x09: return text("Power-on hours");
    case 0x0A: return text("Spin retry count");
    case 0x0B: return text("Calibration retry count");
    case 0x0C: return text("Power cycle count");
    case 0xB7: return text("SATA downshift errors");
    case 0xB8: return text("End-to-end errors");
    case 0xBB: return text("Reported uncorrectable errors");
    case 0xBC: return text("Command timeouts");
    case 0xBD: return text("High fly writes");
    case 0xBE: return text("Airflow temperature");
    case 0xBF: return text("G-sense errors");
    case 0xC0: return text("Power-off retracts");
    case 0xC1: return text("Load/unload cycles");
    case 0xC2: return text("Temperature");
    case 0xC3: return text("Hardware ECC recovered");
    case 0xC4: return text("Reallocation events");
    case 0xC5: return text("Pending sectors");
    case 0xC6: return text("Offline uncorrectable sectors");
    case 0xC7: return text("UltraDMA CRC errors");
    case 0xC8: return text("Write error rate");
    case 0xF0: return text("Head flying hours");
    case 0xF1: return text("Total LBAs written");
    case 0xF2: return text("Total LBAs read");
    default: return text.format("Attribute {}", std::to_string(id));
    }
}

std::string attribute_id(std::uint8_t id)
{
    return std::format("{:02X}", id);
}

std::string finding_words(const Text& text, const SmartFinding& finding)
{
    if (finding.code == "attribute-failing") {
        return text.format("{} ({}) is at or below its threshold", attribute_name(text, finding.attribute),
            attribute_id(finding.attribute));
    }
    if (finding.code == "spare-low") {
        return text.format("Available spare {}% is below its threshold", std::to_string(finding.value));
    }
    if (finding.code == "critical-warning") {
        return text.format("The device reports a critical warning: {}", warning_bits_words(text, finding.warning_bits));
    }
    if (finding.code == "crc-errors-increased") {
        return text.format("{}: usually a cable or an enclosure, not the device itself",
            counted_words(text, attribute_name(text, finding.attribute), finding));
    }
    if (finding.code == "pending-sectors-cleared" || finding.code == "offline-uncorrectable-cleared") {
        return text.format("{} cleared ({} → 0)", attribute_name(text, finding.attribute),
            std::to_string(finding.before.value_or(0)));
    }
    return counted_words(text, counted_name(text, finding), finding);
}

std::string smart_problem_words(const Text& text, const SmartRead& read)
{
    if (read.problem == "not-passed") {
        return text("The enclosure passes no ATA command on to the device.");
    }
    if (read.problem == "smart-not-passed") {
        return text("The device's identification was read, but not its SMART: the enclosure may not pass SMART "
                    "commands on.");
    }
    if (read.problem == "smart-unsupported") {
        return text("The device does not support SMART.");
    }
    if (read.problem == "smart-off") {
        return text("The device has SMART turned off.");
    }
    if (read.problem == "no-answer") {
        return text("The device did not answer in time.");
    }
    if (read.problem.empty()) {
        return text("The SMART data that the device returned does not check.");
    }
    if (read.detail.empty()) {
        return text("The device could not be read.");
    }
    return text.format("The device could not be read: {}", text(read.detail));
}

std::string smart_how_words(const Text& text, std::string_view how)
{
    if (how == "ata") {
        return text("directly (ATA)");
    }
    if (how == "sat") {
        return text("through its enclosure (SAT)");
    }
    if (how == "nvme") {
        return text("NVMe health log");
    }
    if (how == "sample") {
        return text("sample");
    }
    return std::string(how);
}

std::string smart_advice(const Text& text)
{
    return text("This device may be failing. Copy the archives on it to another device soon, and verify the copies.");
}

} // namespace va::app

namespace va::app {
namespace {

// The alerts of `read` alone (a run records no read before it), joined.
std::string alert_words(const Text& text, const std::vector<SmartFinding>& findings)
{
    std::string words;
    for (const auto& finding : findings) {
        if (finding.kind != SmartFindingKind::Alert) {
            continue;
        }
        if (!words.empty()) {
            words += text.language() == Language::TraditionalChinese ? "；" : "; ";
        }
        words += finding_words(text, finding);
    }
    return words;
}

} // namespace

std::string smart_read_words(const Text& text, const SmartRead& read)
{
    const auto when = text.format("read {}, {}", local_time(read.read_at, text), smart_how_words(text, read.how));
    if (!read.problem.empty() || !smart_values(read)) {
        return text.format("{} · {}", smart_problem_words(text, read), when);
    }
    const auto findings = smart_findings(read, nullptr);
    if (!any_smart_alert(findings)) {
        return text.format("{} · {}", text("SMART OK"), when);
    }
    return text.format("{} · {}", text.format("SMART alert: {}", alert_words(text, findings)), when);
}

std::optional<std::string> smart_read_alert_line(const Text& text, const SmartRead& read)
{
    const auto findings = smart_findings(read, nullptr);
    if (!any_smart_alert(findings)) {
        return std::nullopt;
    }
    // The device by its own identity, which a read that gave data has.
    const auto name = read.device.serial.empty()
                          ? text.format("{} (WWN {})", read.device.model, read.device.wwn)
                          : text.format("{} (serial {})", read.device.model, read.device.serial);
    return std::string(alert_mark) + text.format("SMART of {}, read {}: {}. Copy the archives on it to another device "
                                                 "soon, and verify the copies.",
                                         name, local_time(read.read_at, text), alert_words(text, findings));
}

} // namespace va::app
