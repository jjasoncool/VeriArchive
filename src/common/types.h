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