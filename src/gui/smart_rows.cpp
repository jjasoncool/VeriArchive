#include "gui/smart_rows.h"

#include "app/device_alerts.h"
#include "app/local_time.h"
#include "app/smart_words.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace va::gui {
namespace {

using app::Tone;

// The reads that the history table lists at most.
constexpr std::size_t history_rows = 20;

std::string number(std::uint64_t value)
{
    return std::to_string(value);
}

std::string celsius(const app::Text& text, int degrees)
{
    return text.format("{} °C", std::to_string(degrees));
}

std::string hours(const app::Text& text, std::uint64_t value)
{
    return text.format("{} h", number(value));
}

std::string percent(std::uint64_t value)
{
    return number(value) + "%";
}

// A count of an ATA attribute, or "—" where the read has none.
std::string raw_of(const SmartValues& values, std::uint8_t id)
{
    const auto* attribute = values.attribute(id);
    return attribute ? number(attribute->raw) : std::string("—");
}

std::string temperature_of(const app::Text& text, const SmartValues& values)
{
    const auto degrees = values.temperature_celsius();
    return degrees ? celsius(text, *degrees) : std::string("—");
}

// The values most looked at, as fields of the section.
std::vector<Field> main_values(const app::Text& text, const SmartValues& values)
{
    std::vector<Field> fields;
    if (values.nvme) {
        const auto& health = *values.nvme;
        fields.push_back({text("Available spare"),
            text.format("{} (threshold {})", percent(health.available_spare), percent(health.spare_threshold))});
        fields.push_back({text("Percentage used"), percent(health.percentage_used)});
        fields.push_back({text("Media and data integrity errors"), number(health.media_errors)});
        fields.push_back({text("Unsafe shutdowns"), number(health.unsafe_shutdowns)});
        fields.push_back({text("Data written"), app::format_bytes(health.data_units_written * 512'000)});
    } else {
        for (const auto id : {attribute_reallocated_sectors, attribute_pending_sectors,
                 attribute_offline_uncorrectable, attribute_crc_errors}) {
            if (const auto* attribute = values.attribute(id)) {
                fields.push_back({app::attribute_name(text, id), number(attribute->raw)});
            }
        }
        if (const auto* cycles = values.attribute(attribute_power_cycles)) {
            fields.push_back({app::attribute_name(text, attribute_power_cycles), number(cycles->raw)});
        }
    }
    if (const auto on = values.power_on_hours()) {
        fields.push_back({text("Power-on hours"), hours(text, *on)});
    }
    if (const auto degrees = values.temperature_celsius()) {
        fields.push_back({text("Temperature"), celsius(text, *degrees)});
    }
    return fields;
}

// Every value of the read: the ATA attributes, or the NVMe log's values.
SmartTable all_values(const app::Text& text, const SmartValues& values)
{
    SmartTable table;
    if (values.nvme) {
        const auto& health = *values.nvme;
        table.headings = {text("Item"), text("Value")};
        table.rows = {
            {text("Critical warning"), health.critical_warning == 0 ? text("none") : number(health.critical_warning)},
            {text("Temperature"), temperature_of(text, values)},
            {text("Available spare"), percent(health.available_spare)},
            {text("Available spare threshold"), percent(health.spare_threshold)},
            {text("Percentage used"), percent(health.percentage_used)},
            {text("Data read"), app::format_bytes(health.data_units_read * 512'000)},
            {text("Data written"), app::format_bytes(health.data_units_written * 512'000)},
            {text("Power cycle count"), number(health.power_cycles)},
            {text("Power-on hours"), hours(text, health.power_on_hours)},
            {text("Unsafe shutdowns"), number(health.unsafe_shutdowns)},
            {text("Media and data integrity errors"), number(health.media_errors)},
            {text("Error log entries"), number(health.error_log_entries)},
        };
        return table;
    }
    table.headings = {text("ID"), text("Attribute"), text("Current"), text("Worst"), text("Threshold"), text("Raw")};
    for (const auto& attribute : values.attributes) {
        auto name = app::attribute_name(text, attribute.id);
        if (attribute.failing()) {
            name = std::string(app::alert_mark) + name;
        }
        table.rows.push_back({app::attribute_id(attribute.id), name, number(attribute.value), number(attribute.worst),
            attribute.threshold ? number(*attribute.threshold) : std::string("—"), number(attribute.raw)});
    }
    return table;
}

// The reads of the device, newest first, a few values each.
SmartTable history_table(const app::Text& text, const std::vector<const SmartRead*>& reads, bool nvme)
{
    SmartTable table;
    if (nvme) {
        table.headings = {text("Read"), text("Media and data integrity errors"), text("Available spare"),
            text("Percentage used"), text("Temperature")};
    } else {
        table.headings = {text("Read"), app::attribute_id(attribute_reallocated_sectors),
            app::attribute_id(attribute_pending_sectors), app::attribute_id(attribute_offline_uncorrectable),
            app::attribute_id(attribute_crc_errors), text("Temperature")};
    }
    for (std::size_t index = 0; index < reads.size() && index < history_rows; ++index) {
        const auto values = smart_values(*reads[index]);
        if (!values) {
            continue;
        }
        const auto when = app::local_time(reads[index]->read_at, text);
        if (values->nvme) {
            table.rows.push_back({when, number(values->nvme->media_errors), percent(values->nvme->available_spare),
                percent(values->nvme->percentage_used), temperature_of(text, *values)});
        } else {
            table.rows.push_back({when, raw_of(*values, attribute_reallocated_sectors),
                raw_of(*values, attribute_pending_sectors), raw_of(*values, attribute_offline_uncorrectable),
                raw_of(*values, attribute_crc_errors), temperature_of(text, *values)});
        }
    }
    return table;
}

std::string read_line(const app::Text& text, const SmartRead& read)
{
    return text.format("Read {} · {}", app::local_time(read.read_at, text), app::smart_how_words(text, read.how));
}

} // namespace

SmartSection smart_section(
    const app::Text& text, const std::vector<SmartRead>& reads, const PhysicalDeviceInfo& device, bool reader,
    const DeviceIdentity* own)
{
    SmartSection section;
    const auto smart = device_smart(reads, device, own);
    if (smart.unknown_device) {
        section.pill = Pill{text("SMART to read again"), Tone::Neutral};
        section.problem = reader ? text("This enclosure does not tell which device is inside, so no SMART read before "
                                        "is shown: Read the SMART of all devices... reads it again.")
                                 : text("This enclosure does not tell which device is inside, so no SMART read before "
                                        "is shown.");
        return section;
    }
    if (!smart.read()) {
        section.pill = Pill{text("SMART not read"), Tone::Neutral};
        section.problem = reader ? text("Its SMART has not been read yet: Read the SMART of all devices... reads it.")
                                 : text("Its SMART has not been read yet.");
        return section;
    }
    if (!smart.latest) {
        section.pill = Pill{text("SMART cannot be read"), Tone::Neutral};
        section.read = read_line(text, *smart.failed);
        section.problem = app::smart_problem_words(text, *smart.failed);
        return section;
    }
    const auto& latest = *smart.latest;
    const auto values = smart_values(latest);
    section.pill = smart.alerts() ? Pill{text("SMART alert"), Tone::Error} : Pill{text("SMART OK"), Tone::Success};
    section.read = read_line(text, latest);
    for (const auto& finding : smart.findings) {
        auto words = app::finding_words(text, finding);
        if (finding.kind == SmartFindingKind::Alert) {
            section.alerts.push_back(std::string(app::alert_mark) + words);
        } else {
            section.hints.push_back(std::move(words));
        }
    }
    if (!section.alerts.empty()) {
        section.alerts.push_back(app::smart_advice(text));
    }
    if (smart.failed) {
        section.problem = text.format("Latest read, {}: {}", app::local_time(smart.failed->read_at, text),
            app::smart_problem_words(text, *smart.failed));
    }
    if (values) {
        section.values = main_values(text, *values);
        section.attributes = all_values(text, *values);
        section.attributes_title =
            values->nvme ? text.format("All NVMe health values ({})", number(section.attributes.rows.size()))
                         : text.format("All SMART attributes ({})", number(section.attributes.rows.size()));
        section.history = history_table(text, smart.history, values->nvme.has_value());
        section.history_title = text.format("Reads ({})", number(smart.history.size()));
    }
    return section;
}

std::string smart_alert_mark(const app::Text& text, const ArchiveSmartAlert& alert)
{
    auto mark = std::string(app::alert_mark) + text.format("SMART: {}", app::finding_words(text, alert.finding));
    if (alert.more > 0) {
        mark += text.format(" (+{})", number(alert.more));
    }
    return mark;
}

std::string smart_alert_line(const app::Text& text, const ArchiveSmartAlert& alert)
{
    const auto when = app::local_time(alert.read_at, text);
    const auto finding = app::finding_words(text, alert.finding);
    if (alert.more == 0) {
        return std::string(app::alert_mark) +
               text.format("SMART of its device, read {}: {}. Devices shows all of it.", when, finding);
    }
    return std::string(app::alert_mark) +
           text.format("SMART of its device, read {}: {}; {} more alerts. Devices shows all of it.", when, finding,
               number(alert.more));
}

std::string smart_summary(
    const app::Text& text, const std::vector<SmartRead>& reads, const std::vector<PhysicalDeviceInfo>& devices,
    const std::vector<std::optional<DeviceIdentity>>& own)
{
    if (reads.empty()) {
        return text("SMART has not been read yet.");
    }
    const auto last = std::ranges::max_element(reads, {}, &SmartRead::read_at);
    std::size_t alerting = 0;
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        if (device_smart(reads, device, index < own.size() && own[index] ? &*own[index] : nullptr).alerts()) {
            ++alerting;
        }
    }
    const auto when = app::local_time(last->read_at, text);
    if (alerting == 0) {
        return text.format("SMART last read {} · no attached device alerts", when);
    }
    if (alerting == 1) {
        return text.format("SMART last read {} · 1 attached device alerts", when);
    }
    return text.format("SMART last read {} · {} attached devices alert", when, number(alerting));
}

} // namespace va::gui

namespace va::gui {

std::optional<std::string> smart_run_words(const app::Text& text,
    const std::expected<std::vector<DeviceRead>, IoError>& reads, const std::vector<SmartRead>& all)
{
    if (!reads) {
        switch (device_identity_failure(reads.error())) {
        case DeviceIdentityFailure::Cancelled:
            return std::nullopt;
        case DeviceIdentityFailure::Declined:
            return text("Administrator permission was not given, so no SMART was read.");
        case DeviceIdentityFailure::NotAvailable:
            return text("This computer has no device identity reader, so SMART cannot be read here.");
        default:
            return text.format("SMART could not be read: {}", text(reads.error().message));
        }
    }
    std::size_t read = 0;
    std::size_t alerting = 0;
    for (const auto& device : *reads) {
        if (!device.smart || !device.smart->problem.empty() || !smart_values(*device.smart)) {
            continue;
        }
        ++read;
        if (device_smart(all, device.smart->seen, device.identity ? &device.identity->device : nullptr).alerts()) {
            ++alerting;
        }
    }
    return text.format("SMART read of {} of {} devices. Devices with a SMART alert: {}.", number(read),
        number(reads->size()), number(alerting));
}

} // namespace va::gui
