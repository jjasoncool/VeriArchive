#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace va {

using Uuid = std::string;
using Timestamp = std::string;

// What a device reports of its own health (decision 0044), as the system
// shows it to a program without elevation: Windows from the storage stack's
// health of the disk, Linux from udisks2's reading of its SMART data. Unknown
// where it cannot be read, as through a USB enclosure that passes nothing on.
enum class HealthStatus { Unknown, Healthy, Warning, Unhealthy };

struct DeviceHealth {
    HealthStatus status = HealthStatus::Unknown;
    // What the device reports beside its status, as codes that the interfaces
    // word: "predicting-failure", "hardware-error", "not-usable",
    // "transient-error", "in-service", "bad-sectors", "attributes-failing",
    // "spare-low", "temperature", "degraded", "read-only",
    // "volatile-memory-failed". Empty for a device that reports none.
    std::vector<std::string> conditions;
};

struct PhysicalDeviceInfo {
    std::string device_ref;
    std::string model;
    std::string serial;
    std::string wwn;
    std::uint64_t capacity_bytes = 0;
    std::optional<bool> rotational;
    std::string bus;
    // Runtime only, never identity (decision 0044): the device's health as
    // IMediaEnumerator::device_health read it at the start of the operation
    // that reads the volume, kept in its media evidence; nothing where it
    // was not asked.
    std::optional<DeviceHealth> health;
    // Runtime only, never identity (decision 0047): the device is behind an
    // enclosure (a USB bridge or dock) that hides the device's own identity,
    // so `serial` and `wwn` are the enclosure's, as the platform tells it
    // without elevation. False where nothing hides it, for removable media
    // (a USB flash memory or card reader has no enclosure), and where it
    // cannot tell; false also where the device's identification page names
    // the device by a worldwide identifier that the enclosure passes on, while
    // `serial` may then still be the enclosure's. So false does not prove that
    // `serial` is the device's own.
    bool enclosure_hides_identity = false;
};

// A device's own identity as the device identity reader reads it behind its
// enclosure (ATA IDENTIFY DEVICE, decision 0047): model, serial and WWN.
struct DeviceIdentity {
    std::string model;
    std::string serial;
    std::string wwn;
};

// One read of a device's own identity: what was read, how the reader read it
// ("ata": ATA pass-through; "sat": an ATA command inside a SCSI command, as
// USB bridges take it), when, and which device of the volume it read (its
// device_ref; empty where not known).
struct DeviceIdentityRead {
    DeviceIdentity device;
    std::string how;
    Timestamp read_at;
    std::string device_ref;
};

// One read of a device's SMART by the device identity reader (decision
// 0052): the pages as the device returned them, which the repository keeps
// (smart_reads) and the program decodes unprivileged (core/smart), so that a
// better decoding reads old reads anew. A read that gave no SMART data is
// kept too, with why (`problem`).
struct SmartRead {
    std::int64_t id = 0;            // its row in smart_reads; 0 before it is recorded
    Timestamp read_at;
    // The device as the system described it without elevation when it was
    // read (device_ref, model, serial, WWN, bus, capacity, whether its
    // enclosure hides its identity): how Devices finds the reads of a device
    // attached now.
    PhysicalDeviceInfo seen;
    // Its own identity, from its identification (ATA IDENTIFY DEVICE or NVMe
    // Identify Controller): how the reads of one device are compared over
    // time. Empty where its identification was not read.
    DeviceIdentity device;
    std::string firmware;
    // How it was read: "ata" (ATA pass-through), "sat" (ATA commands inside
    // SCSI commands, as USB bridges take them), "nvme" (the NVMe protocol
    // through the storage driver); empty where nothing was read.
    std::string how;
    std::vector<std::uint8_t> identify;    // IDENTIFY DEVICE (512 bytes) or Identify Controller (4096)
    std::vector<std::uint8_t> data;        // SMART READ DATA or the SMART / Health Information log (512)
    std::vector<std::uint8_t> thresholds;  // SMART READ THRESHOLDS (512); empty for NVMe
    // Empty where `data` was read; else why not: "not-passed" (the
    // enclosure passes no command on to the device), "smart-not-passed"
    // (the identification was read, SMART was not), "smart-unsupported",
    // "smart-off" (the device has SMART turned off), "no-answer",
    // "unreadable".
    std::string problem;
    std::string detail;  // the error's own message (English), where there is one
};

struct VolumeInfo {
    std::filesystem::path mount_point;
    std::string filesystem_type;
    std::string filesystem_uuid;
    std::string filesystem_uuid_short;
    std::string volume_label;
    std::string volume_guid;
    std::uint64_t capacity_bytes = 0;
    bool read_only = false;
    std::vector<PhysicalDeviceInfo> devices;
    std::string container;
    // Runtime only, never identity or part of a result (decision 0006): the
    // program that serves the mount where it is no kernel driver of the
    // format, such as "ntfs-3g" for NTFS through FUSE on Linux; else empty.
    std::string driver;
};

struct ArchiveIdentity {
    Uuid archive_uuid;
    std::string label;
    std::uint32_t revision = 0;
    std::string root_subpath;
    Timestamp created;
};

struct MediaEvidence {
    Timestamp seen_at;
    VolumeInfo volume;
    std::optional<Uuid> claimed_archive_uuid;
};

} // namespace va