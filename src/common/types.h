#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace va {

using Uuid = std::string;
using Timestamp = std::string;

struct PhysicalDeviceInfo {
    std::string device_ref;
    std::string model;
    std::string serial;
    std::string wwn;
    std::uint64_t capacity_bytes = 0;
    std::optional<bool> rotational;
    std::string bus;
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