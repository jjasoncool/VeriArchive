#pragma once
// SMART (decision 0052): what a device reports of its own condition, read by
// the device identity reader with administrator permission and kept by the
// repository as the pages the device returned (SmartRead, smart_reads). This
// part decodes those pages and judges them, unprivileged and in plain C++:
// ATA SMART READ DATA with SMART READ THRESHOLDS, and the NVMe SMART / Health
// Information log. A SMART alert is a device alert (GLOSSARY): it is told
// beside the archives on the device and never changes a run result.
#include "common/types.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace va {

// The size of each ATA page (IDENTIFY DEVICE, SMART READ DATA, SMART READ
// THRESHOLDS) and of the NVMe health log; of the NVMe Identify Controller
// data.
inline constexpr std::size_t smart_page_size = 512;
inline constexpr std::size_t nvme_identify_size = 4096;

// The ATA attributes that the judgement reads by their id.
inline constexpr std::uint8_t attribute_reallocated_sectors = 0x05;
inline constexpr std::uint8_t attribute_power_on_hours = 0x09;
inline constexpr std::uint8_t attribute_power_cycles = 0x0C;
inline constexpr std::uint8_t attribute_reported_uncorrectable = 0xBB;
inline constexpr std::uint8_t attribute_airflow_temperature = 0xBE;
inline constexpr std::uint8_t attribute_temperature = 0xC2;
inline constexpr std::uint8_t attribute_pending_sectors = 0xC5;
inline constexpr std::uint8_t attribute_offline_uncorrectable = 0xC6;
inline constexpr std::uint8_t attribute_crc_errors = 0xC7;

// One attribute of SMART READ DATA (a 12-byte entry): its id, flags, current
// (normalised) and worst value, its 48-bit raw value (little-endian), and its
// threshold from SMART READ THRESHOLDS, where that page names the id.
struct SmartAttribute {
    std::uint8_t id = 0;
    std::uint16_t flags = 0;
    std::uint8_t value = 0;
    std::uint8_t worst = 0;
    std::uint64_t raw = 0;
    std::optional<std::uint8_t> threshold;
    // Bit 0 of the flags: a pre-failure attribute, whose value at or below its
    // threshold predicts failure; otherwise an old-age (usage) attribute.
    bool prefailure() const { return (flags & 0x0001) != 0; }
    // At or below a threshold that is one (1 to 253; 0 means the attribute
    // never fails, and 254 and 255 are no thresholds).
    bool failing() const { return threshold && *threshold >= 1 && *threshold <= 253 && value <= *threshold; }
    bool operator==(const SmartAttribute&) const = default;
};

// The attributes of `data` (SMART READ DATA: up to 30 entries from byte 2, an
// id of 0 marking an unused entry), in the order the page lists them, each
// with its threshold from `thresholds` (SMART READ THRESHOLDS, the same
// layout: id, threshold), which may be empty. Nothing for data of another
// size, data that is all zeros, or whose checksum (byte 511 makes the sum of
// its 512 bytes zero) does not hold; a thresholds page that is not 512 bytes,
// or whose checksum does not hold, gives no thresholds.
std::optional<std::vector<SmartAttribute>> decode_ata_attributes(
    std::span<const std::uint8_t> data, std::span<const std::uint8_t> thresholds);

// What the identification (IDENTIFY DEVICE) says of SMART: supported (word
// 82, bit 0) and turned on (word 85, bit 0), where those words are valid
// (neither 0x0000 nor 0xFFFF). Nothing for identification of another size.
struct AtaSmartSupport {
    bool supported = false;
    bool enabled = false;
};
std::optional<AtaSmartSupport> ata_smart_support(std::span<const std::uint8_t> identify);

// The SMART / Health Information log of an NVMe device (log page 0x02). Its
// 128-bit counters are kept to 64 bits (a higher value is the largest one).
struct NvmeHealth {
    std::uint8_t critical_warning = 0;   // bits: 0 spare low, 1 temperature, 2 reliability degraded,
                                         // 3 read-only, 4 volatile memory backup failed, 5 persistent memory read-only
    std::uint16_t temperature_kelvin = 0;
    std::uint8_t available_spare = 0;    // percent
    std::uint8_t spare_threshold = 0;    // percent
    std::uint8_t percentage_used = 0;    // of the endurance its maker estimates; may pass 100
    std::uint64_t data_units_read = 0;   // in units of 1000 blocks of 512 bytes
    std::uint64_t data_units_written = 0;
    std::uint64_t power_cycles = 0;
    std::uint64_t power_on_hours = 0;
    std::uint64_t unsafe_shutdowns = 0;
    std::uint64_t media_errors = 0;      // data integrity errors the device could not correct
    std::uint64_t error_log_entries = 0;
    bool operator==(const NvmeHealth&) const = default;
};
// Nothing for a log of another size, or one that is all zeros.
std::optional<NvmeHealth> decode_nvme_health(std::span<const std::uint8_t> log);

// What a device names itself in NVMe Identify Controller data: its serial
// (bytes 4-23), model (24-63) and firmware revision (64-71), ASCII padded
// with spaces, trimmed; a byte that is no printable ASCII becomes '?'.
// Nothing for data of another size, or without a serial.
struct NvmeIdentity {
    std::string model;
    std::string serial;
    std::string firmware;
};
std::optional<NvmeIdentity> decode_nvme_identify(std::span<const std::uint8_t> identify);

// What a read decodes to: ATA attributes, or the NVMe health log.
struct SmartValues {
    std::vector<SmartAttribute> attributes;  // ATA
    std::optional<NvmeHealth> nvme;
    // The attribute with `id`, or nothing.
    const SmartAttribute* attribute(std::uint8_t id) const;
    // The device's temperature in °C: ATA attribute 194 (else 190), the low
    // byte of its raw value; NVMe's composite temperature. Nothing where the
    // device reports none.
    std::optional<int> temperature_celsius() const;
    // The hours the device has been powered: ATA attribute 9 (the low 32 bits
    // of its raw value, as most devices count hours), NVMe's.
    std::optional<std::uint64_t> power_on_hours() const;
};
// The values of `read`, where its data decodes ("nvme": the health log;
// otherwise the ATA attributes with their thresholds).
std::optional<SmartValues> smart_values(const SmartRead& read);

// One thing a read says, as the window words it (app/smart_words.h).
enum class SmartFindingKind {
    Alert,  // a SMART alert: the device may be failing
    Hint,   // worth knowing, no alert: a cable or enclosure that loses data on the way, a count cleared
};
struct SmartFinding {
    SmartFindingKind kind = SmartFindingKind::Alert;
    // Alerts: "attribute-failing" (`attribute` at or below its threshold),
    // "reallocated-sectors", "pending-sectors", "offline-uncorrectable",
    // "reported-uncorrectable" (ATA, a raw count above zero), "critical-warning"
    // (NVMe, `warning_bits` without bit 0), "spare-low" (NVMe: available spare
    // below its threshold, or bit 0), "media-errors" (NVMe, above zero).
    // Hints: "crc-errors-increased" (ATA 199 counted more than in the
    // previous read), "pending-sectors-cleared", "offline-uncorrectable-cleared"
    // (above zero in the previous read, zero now).
    std::string code;
    std::uint8_t attribute = 0;          // the ATA attribute's id, where the finding is of one
    std::uint64_t value = 0;             // the count or value now
    std::optional<std::uint64_t> before; // in the previous read of the same device, where it gave one
    std::uint8_t warning_bits = 0;       // "critical-warning": the NVMe critical warning
    bool operator==(const SmartFinding&) const = default;
};

// What `read` says, compared with `previous`, the read of the same device
// before it that gave data (nothing where there is none): its alerts first,
// then its hints. An alert stays while its count stays above zero, whether it
// rose, stayed or fell (`before` tells which); a pending or offline
// uncorrectable count back at zero alerts no more, and is a hint once. A count
// that fell to zero because its sectors were reallocated raises the count of
// reallocated sectors, which alerts. Temperature alerts nothing. Nothing for
// a read without data that decodes.
std::vector<SmartFinding> smart_findings(const SmartRead& read, const SmartRead* previous);

bool any_smart_alert(const std::vector<SmartFinding>& findings);

// Whether two descriptions of a device are of the device a SMART read saw:
// the same model, serial and WWN as the system reports them (an enclosure's
// where it hides the device's own), with a serial or a WWN, and the same
// capacity where both know it. Not the device's own identity: Devices finds
// the reads of a device attached now by what the system tells of it without
// elevation.
bool same_seen_device(const PhysicalDeviceInfo& left, const PhysicalDeviceInfo& right);

// The reads of one device that Devices and Archives show (decision 0052).
// `reads` as the repository lists them, oldest first. `device` as the system
// describes it now; `own`, where known now, its own identity (a read of it
// that the window keeps, decision 0047): the reads are then those of that
// device, by its own serial or WWN. Without it, the reads are those of the
// device as the system describes it, made where nothing hid its identity: a
// device behind an enclosure that hides its identity, with no read of its
// own identity now, shows no read at all (`unknown_device`), as another
// device may be inside the enclosure since (operator, 2026-11-10).
struct DeviceSmart {
    bool unknown_device = false;
    // The latest read of the device seen as it is described that gave data:
    // what its SMART says.
    const SmartRead* latest = nullptr;
    // The read before `latest` of the same device (its own identity,
    // same_device_identity) that gave data, which `latest` is compared with.
    const SmartRead* previous = nullptr;
    // The latest read of the device seen so, where it is later than `latest`
    // (or there is none) and gave no data: why not.
    const SmartRead* failed = nullptr;
    // Every read of the same device that gave data, newest first.
    std::vector<const SmartRead*> history;
    std::vector<SmartFinding> findings;  // smart_findings(latest, previous)
    bool read() const { return latest != nullptr || failed != nullptr; }
    bool alerts() const { return any_smart_alert(findings); }
};
DeviceSmart device_smart(
    const std::vector<SmartRead>& reads, const PhysicalDeviceInfo& device, const DeviceIdentity* own = nullptr);

// The SMART alert that marks an archive (decision 0052): of the first of
// `devices` (those of its carrier, each with its own identity in `own` where
// it is known now, as device_smart takes it) whose latest read alerts, that
// read's time and first alert, and how many more it gives.
struct ArchiveSmartAlert {
    Timestamp read_at;
    SmartFinding finding;
    std::size_t more = 0;
    bool operator==(const ArchiveSmartAlert&) const = default;
};
std::optional<ArchiveSmartAlert> archive_smart_alert(
    const std::vector<SmartRead>& reads, const std::vector<PhysicalDeviceInfo>& devices,
    const std::vector<std::optional<DeviceIdentity>>& own = {});

} // namespace va

namespace va {

// Decision 0052: the reads of `reads` made of the devices of `devices` (the
// volume a verification reads), each found by the system's name for the
// device and its description at the moment both were read.
std::vector<SmartRead> reads_of_volume(const std::vector<SmartRead>& reads, const std::vector<PhysicalDeviceInfo>& devices);

// The SMART reads that a verification records with its run (counts_json
// member `smart`, hash-chained, and its record for a resume): [{"read_at",
// "seen": {"model", "serial", "wwn"}, "device": {"model", "serial", "wwn"},
// "firmware", "how", "data", "thresholds", "problem"}], the pages in
// lowercase hex as the device returned them; and back. A member missing or
// of another type reads as empty.
nlohmann::json smart_reads_json(const std::vector<SmartRead>& reads);
std::vector<SmartRead> smart_reads_from_json(const nlohmann::json& json);
// Those that a run's counts record; none for a run that recorded none.
std::vector<SmartRead> recorded_smart_reads(std::string_view counts_json);

} // namespace va
