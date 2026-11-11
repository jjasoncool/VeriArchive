#include "core/smart/smart_data.h"

#include "common/hex.h"
#include "core/engine/carrier_confirmation.h"

#include <algorithm>
#include <limits>

namespace va {
namespace {

bool all_zero(std::span<const std::uint8_t> data)
{
    return std::ranges::all_of(data, [](std::uint8_t byte) { return byte == 0; });
}

// The ATA data structure checksum: the 512 bytes sum to zero.
bool sums_to_zero(std::span<const std::uint8_t> data)
{
    unsigned sum = 0;
    for (const auto byte : data) {
        sum += byte;
    }
    return (sum & 0xff) == 0;
}

std::uint16_t word_at(std::span<const std::uint8_t> data, std::size_t index)
{
    return static_cast<std::uint16_t>(data[2 * index] | (data[2 * index + 1] << 8));
}

// `count` bytes from `offset`, little-endian; a value that needs more than 64
// bits is the largest one.
std::uint64_t little_endian(std::span<const std::uint8_t> data, std::size_t offset, std::size_t count)
{
    for (std::size_t index = 8; index < count; ++index) {
        if (data[offset + index] != 0) {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    std::uint64_t value = 0;
    for (std::size_t index = std::min<std::size_t>(count, 8); index-- > 0;) {
        value = (value << 8) | data[offset + index];
    }
    return value;
}

std::string ascii_field(std::span<const std::uint8_t> data, std::size_t offset, std::size_t count)
{
    std::string text;
    for (std::size_t index = offset; index < offset + count; ++index) {
        const auto byte = data[index];
        text += byte == 0 ? ' ' : (byte < 0x20 || byte > 0x7e) ? '?' : static_cast<char>(byte);
    }
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

// The raw count of an ATA attribute, where the read gives that attribute.
std::optional<std::uint64_t> raw_count(const SmartValues& values, std::uint8_t id)
{
    if (const auto* attribute = values.attribute(id)) {
        return attribute->raw;
    }
    return std::nullopt;
}

// Whether two reads are of one device: by its own identity where both read
// one, else as the system saw it.
bool same_device(const SmartRead& left, const SmartRead& right)
{
    const bool own = !(left.device.serial.empty() && left.device.wwn.empty()) &&
                     !(right.device.serial.empty() && right.device.wwn.empty());
    if (own) {
        return same_device_identity(left.device, right.device);
    }
    return same_seen_device(left.seen, right.seen);
}

} // namespace

std::optional<std::vector<SmartAttribute>> decode_ata_attributes(
    std::span<const std::uint8_t> data, std::span<const std::uint8_t> thresholds)
{
    if (data.size() != smart_page_size || all_zero(data) || !sums_to_zero(data)) {
        return std::nullopt;
    }
    const bool with_thresholds = thresholds.size() == smart_page_size && sums_to_zero(thresholds);
    std::vector<SmartAttribute> attributes;
    for (std::size_t entry = 0; entry < 30; ++entry) {
        const std::size_t at = 2 + 12 * entry;
        if (data[at] == 0) {
            continue;
        }
        SmartAttribute attribute;
        attribute.id = data[at];
        attribute.flags = static_cast<std::uint16_t>(data[at + 1] | (data[at + 2] << 8));
        attribute.value = data[at + 3];
        attribute.worst = data[at + 4];
        attribute.raw = little_endian(data, at + 5, 6);
        if (with_thresholds) {
            for (std::size_t limit = 0; limit < 30; ++limit) {
                const std::size_t place = 2 + 12 * limit;
                if (thresholds[place] == attribute.id) {
                    attribute.threshold = thresholds[place + 1];
                    break;
                }
            }
        }
        attributes.push_back(attribute);
    }
    return attributes;
}

std::optional<AtaSmartSupport> ata_smart_support(std::span<const std::uint8_t> identify)
{
    if (identify.size() != smart_page_size) {
        return std::nullopt;
    }
    const auto valid = [](std::uint16_t word) { return word != 0x0000 && word != 0xffff; };
    const auto supported_word = word_at(identify, 82);
    const auto enabled_word = word_at(identify, 85);
    AtaSmartSupport support;
    support.supported = valid(supported_word) && (supported_word & 0x0001) != 0;
    support.enabled = valid(enabled_word) && (enabled_word & 0x0001) != 0;
    return support;
}

std::optional<NvmeHealth> decode_nvme_health(std::span<const std::uint8_t> log)
{
    if (log.size() != smart_page_size || all_zero(log)) {
        return std::nullopt;
    }
    NvmeHealth health;
    health.critical_warning = log[0];
    health.temperature_kelvin = static_cast<std::uint16_t>(log[1] | (log[2] << 8));
    health.available_spare = log[3];
    health.spare_threshold = log[4];
    health.percentage_used = log[5];
    health.data_units_read = little_endian(log, 32, 16);
    health.data_units_written = little_endian(log, 48, 16);
    health.power_cycles = little_endian(log, 112, 16);
    health.power_on_hours = little_endian(log, 128, 16);
    health.unsafe_shutdowns = little_endian(log, 144, 16);
    health.media_errors = little_endian(log, 160, 16);
    health.error_log_entries = little_endian(log, 176, 16);
    return health;
}

std::optional<NvmeIdentity> decode_nvme_identify(std::span<const std::uint8_t> identify)
{
    if (identify.size() != nvme_identify_size) {
        return std::nullopt;
    }
    NvmeIdentity identity{ascii_field(identify, 24, 40), ascii_field(identify, 4, 20), ascii_field(identify, 64, 8)};
    if (identity.serial.empty()) {
        return std::nullopt;
    }
    return identity;
}

const SmartAttribute* SmartValues::attribute(std::uint8_t id) const
{
    const auto found = std::ranges::find(attributes, id, &SmartAttribute::id);
    return found == attributes.end() ? nullptr : &*found;
}

std::optional<int> SmartValues::temperature_celsius() const
{
    if (nvme) {
        if (nvme->temperature_kelvin == 0) {
            return std::nullopt;
        }
        return static_cast<int>(nvme->temperature_kelvin) - 273;
    }
    for (const auto id : {attribute_temperature, attribute_airflow_temperature}) {
        if (const auto* found = attribute(id)) {
            const auto celsius = static_cast<int>(found->raw & 0xff);
            if (celsius > 0 && celsius < 128) {
                return celsius;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> SmartValues::power_on_hours() const
{
    if (nvme) {
        return nvme->power_on_hours;
    }
    if (const auto* found = attribute(attribute_power_on_hours)) {
        return found->raw & 0xffff'ffffu;
    }
    return std::nullopt;
}

std::optional<SmartValues> smart_values(const SmartRead& read)
{
    SmartValues values;
    if (read.how == "nvme") {
        values.nvme = decode_nvme_health(read.data);
        if (!values.nvme) {
            return std::nullopt;
        }
        return values;
    }
    auto attributes = decode_ata_attributes(read.data, read.thresholds);
    if (!attributes) {
        return std::nullopt;
    }
    values.attributes = std::move(*attributes);
    return values;
}

std::vector<SmartFinding> smart_findings(const SmartRead& read, const SmartRead* previous)
{
    const auto now = smart_values(read);
    if (!now) {
        return {};
    }
    const auto before = previous ? smart_values(*previous) : std::nullopt;
    std::vector<SmartFinding> alerts;
    std::vector<SmartFinding> hints;

    if (now->nvme) {
        const auto& health = *now->nvme;
        const auto earlier = before && before->nvme ? std::optional(*before->nvme) : std::nullopt;
        if ((health.critical_warning & 0x01) != 0 || health.available_spare < health.spare_threshold) {
            alerts.push_back({SmartFindingKind::Alert, "spare-low", 0, health.available_spare,
                earlier ? std::optional<std::uint64_t>(earlier->available_spare) : std::nullopt, 0});
        }
        if (const std::uint8_t others = health.critical_warning & 0x3e; others != 0) {
            alerts.push_back({SmartFindingKind::Alert, "critical-warning", 0, others, std::nullopt, others});
        }
        if (health.media_errors > 0) {
            alerts.push_back({SmartFindingKind::Alert, "media-errors", 0, health.media_errors,
                earlier ? std::optional(earlier->media_errors) : std::nullopt, 0});
        }
        return alerts;
    }

    for (const auto& attribute : now->attributes) {
        if (attribute.failing()) {
            alerts.push_back({SmartFindingKind::Alert, "attribute-failing", attribute.id, attribute.value,
                std::nullopt, 0});
        }
    }
    struct Counted {
        std::uint8_t id;
        const char* alert;
        const char* cleared;  // a hint once the count is back at zero; nullptr: it never goes back
    };
    for (const Counted counted : {Counted{attribute_reallocated_sectors, "reallocated-sectors", nullptr},
             Counted{attribute_pending_sectors, "pending-sectors", "pending-sectors-cleared"},
             Counted{attribute_offline_uncorrectable, "offline-uncorrectable", "offline-uncorrectable-cleared"},
             Counted{attribute_reported_uncorrectable, "reported-uncorrectable", nullptr}}) {
        const auto count = raw_count(*now, counted.id);
        if (!count) {
            continue;
        }
        const auto earlier = before ? raw_count(*before, counted.id) : std::nullopt;
        if (*count > 0) {
            alerts.push_back({SmartFindingKind::Alert, counted.alert, counted.id, *count, earlier, 0});
        } else if (counted.cleared && earlier && *earlier > 0) {
            hints.push_back({SmartFindingKind::Hint, counted.cleared, counted.id, 0, earlier, 0});
        }
    }
    if (const auto crc = raw_count(*now, attribute_crc_errors)) {
        const auto earlier = before ? raw_count(*before, attribute_crc_errors) : std::nullopt;
        if (earlier && *crc > *earlier) {
            hints.push_back({SmartFindingKind::Hint, "crc-errors-increased", attribute_crc_errors, *crc, earlier, 0});
        }
    }
    alerts.insert(alerts.end(), hints.begin(), hints.end());
    return alerts;
}

bool any_smart_alert(const std::vector<SmartFinding>& findings)
{
    return std::ranges::any_of(
        findings, [](const SmartFinding& finding) { return finding.kind == SmartFindingKind::Alert; });
}

bool same_seen_device(const PhysicalDeviceInfo& left, const PhysicalDeviceInfo& right)
{
    if (left.serial.empty() && left.wwn.empty()) {
        return false;
    }
    if (left.model != right.model || left.serial != right.serial || left.wwn != right.wwn) {
        return false;
    }
    return left.capacity_bytes == 0 || right.capacity_bytes == 0 || left.capacity_bytes == right.capacity_bytes;
}

DeviceSmart device_smart(const std::vector<SmartRead>& reads, const PhysicalDeviceInfo& device, const DeviceIdentity* own)
{
    DeviceSmart smart;
    // Behind an enclosure that hides the device's identity, what the system
    // tells is the enclosure's: only a read of the device's own identity now
    // tells which device is inside (decision 0052, operator 2026-11-10).
    if (!own && device.enclosure_hides_identity) {
        smart.unknown_device = true;
        return smart;
    }
    // The reads of the device: by its own identity where it is known now;
    // else by what the system tells of it, never by a read made behind an
    // enclosure that hid the identity of the device inside.
    const auto of_device = [&](const SmartRead& read) {
        if (own) {
            return !(own->serial.empty() && own->wwn.empty()) && same_device_identity(read.device, *own);
        }
        return !read.seen.enclosure_hides_identity && same_seen_device(read.seen, device);
    };
    // The latest read of the device; the latest of those that gave data.
    const SmartRead* last_seen = nullptr;
    for (const auto& read : reads) {
        if (!of_device(read)) {
            continue;
        }
        last_seen = &read;
        if (read.problem.empty() && smart_values(read)) {
            smart.latest = &read;
        }
    }
    if (last_seen && last_seen != smart.latest) {
        smart.failed = last_seen;
    }
    if (!smart.latest) {
        return smart;
    }
    for (auto read = reads.rbegin(); read != reads.rend(); ++read) {
        if (!read->problem.empty() || !same_device(*read, *smart.latest) || !smart_values(*read)) {
            continue;
        }
        smart.history.push_back(&*read);
    }
    // The history is newest first and holds `latest`: the read after it in
    // the list is the one before it in time.
    const auto at = std::ranges::find(smart.history, smart.latest);
    if (at != smart.history.end() && std::next(at) != smart.history.end()) {
        smart.previous = *std::next(at);
    }
    smart.findings = smart_findings(*smart.latest, smart.previous);
    return smart;
}

std::optional<ArchiveSmartAlert> archive_smart_alert(
    const std::vector<SmartRead>& reads, const std::vector<PhysicalDeviceInfo>& devices,
    const std::vector<std::optional<DeviceIdentity>>& own)
{
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        const DeviceIdentity* known = index < own.size() && own[index] ? &*own[index] : nullptr;
        const auto smart = device_smart(reads, device, known);
        if (!smart.alerts()) {
            continue;
        }
        ArchiveSmartAlert alert;
        alert.read_at = smart.latest->read_at;
        alert.finding = smart.findings.front();
        alert.more = static_cast<std::size_t>(std::ranges::count_if(smart.findings,
                         [](const SmartFinding& finding) { return finding.kind == SmartFindingKind::Alert; })) -
                     1;
        return alert;
    }
    return std::nullopt;
}

} // namespace va

namespace va {
namespace {

std::string page_hex(const std::vector<std::uint8_t>& bytes)
{
    return to_hex(std::as_bytes(std::span(bytes)));
}

std::vector<std::uint8_t> page_bytes(const nlohmann::json& json, const char* name)
{
    const auto found = json.find(name);
    if (found == json.end() || !found->is_string()) {
        return {};
    }
    const auto bytes = from_hex(found->get<std::string>());
    if (!bytes) {
        return {};
    }
    std::vector<std::uint8_t> page;
    for (const auto byte : *bytes) {
        page.push_back(std::to_integer<std::uint8_t>(byte));
    }
    return page;
}

std::string text_of(const nlohmann::json& json, const char* name)
{
    const auto found = json.find(name);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

nlohmann::json identity_json(const std::string& model, const std::string& serial, const std::string& wwn)
{
    return nlohmann::json{{"model", model}, {"serial", serial}, {"wwn", wwn}};
}

} // namespace

std::vector<SmartRead> reads_of_volume(const std::vector<SmartRead>& reads, const std::vector<PhysicalDeviceInfo>& devices)
{
    std::vector<SmartRead> kept;
    for (const auto& read : reads) {
        if (std::ranges::any_of(devices, [&](const PhysicalDeviceInfo& device) {
                return !device.device_ref.empty() && read.seen.device_ref == device.device_ref &&
                       same_seen_device(read.seen, device);
            })) {
            kept.push_back(read);
        }
    }
    return kept;
}

nlohmann::json smart_reads_json(const std::vector<SmartRead>& reads)
{
    auto list = nlohmann::json::array();
    for (const auto& read : reads) {
        list.push_back(nlohmann::json{{"read_at", read.read_at},
            {"seen", identity_json(read.seen.model, read.seen.serial, read.seen.wwn)},
            {"device", identity_json(read.device.model, read.device.serial, read.device.wwn)},
            {"firmware", read.firmware}, {"how", read.how}, {"data", page_hex(read.data)},
            {"thresholds", page_hex(read.thresholds)}, {"problem", read.problem}});
    }
    return list;
}

std::vector<SmartRead> smart_reads_from_json(const nlohmann::json& json)
{
    std::vector<SmartRead> reads;
    if (!json.is_array()) {
        return reads;
    }
    for (const auto& entry : json) {
        if (!entry.is_object()) {
            continue;
        }
        SmartRead read;
        read.read_at = text_of(entry, "read_at");
        if (const auto seen = entry.find("seen"); seen != entry.end() && seen->is_object()) {
            read.seen.model = text_of(*seen, "model");
            read.seen.serial = text_of(*seen, "serial");
            read.seen.wwn = text_of(*seen, "wwn");
        }
        if (const auto device = entry.find("device"); device != entry.end() && device->is_object()) {
            read.device = DeviceIdentity{text_of(*device, "model"), text_of(*device, "serial"), text_of(*device, "wwn")};
        }
        read.firmware = text_of(entry, "firmware");
        read.how = text_of(entry, "how");
        read.data = page_bytes(entry, "data");
        read.thresholds = page_bytes(entry, "thresholds");
        read.problem = text_of(entry, "problem");
        reads.push_back(std::move(read));
    }
    return reads;
}

std::vector<SmartRead> recorded_smart_reads(std::string_view counts_json)
{
    const auto counts = nlohmann::json::parse(counts_json, nullptr, false);
    if (!counts.is_object() || !counts.contains("smart")) {
        return {};
    }
    return smart_reads_from_json(counts["smart"]);
}

} // namespace va
