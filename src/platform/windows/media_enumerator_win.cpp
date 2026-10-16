// Windows media enumeration with unprivileged calls only (R-09): device and
// volume handles are opened with desired access 0, which is enough for the
// identification IOCTLs used here.
#include "platform/media_enumerator.h"
#include "platform/windows/volume_list.h"
#include "platform/windows/win_util.h"

#include "common/utf8.h"
#include "core/identity/filesystem_id.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>
#include <winioctl.h>

namespace va::platform {
namespace {

using windows::UniqueHandle;
using windows::error_from_win32;

std::string wide_to_utf8(std::wstring_view text)
{
    return utf16_to_utf8(std::u16string(text.begin(), text.end()));
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Opens a device or volume for identification only (no read access needed).
UniqueHandle open_for_query(const std::wstring& name)
{
    return UniqueHandle(CreateFileW(name.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr));
}

// Reads a string stored at `offset` inside an IOCTL output buffer.
std::string string_at(const std::vector<BYTE>& buffer, DWORD offset)
{
    if (offset == 0 || offset >= buffer.size()) {
        return {};
    }
    const auto* begin = reinterpret_cast<const char*>(buffer.data() + offset);
    const auto* end = reinterpret_cast<const char*>(buffer.data() + buffer.size());
    return trim(std::string(begin, std::find(begin, end, '\0')));
}

std::string bus_name(STORAGE_BUS_TYPE bus)
{
    switch (bus) {
    case BusTypeUsb:
        return "usb";
    case BusTypeSata:
    case BusTypeAta:
        return "sata";
    case BusTypeNvme:
        return "nvme";
    case BusTypeScsi:
    case BusTypeSas:
    case BusTypeiScsi:
        return "scsi";
    default:
        return "";
    }
}

PhysicalDeviceInfo describe_disk(DWORD disk_number)
{
    PhysicalDeviceInfo device;
    device.device_ref = "\\\\.\\PhysicalDrive" + std::to_string(disk_number);
    const auto disk = open_for_query(L"\\\\.\\PhysicalDrive" + std::to_wstring(disk_number));
    if (!disk.valid()) {
        return device;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<BYTE> buffer(4096);
    DWORD returned = 0;
    if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
            buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        buffer.resize(returned);
        const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        const auto vendor = string_at(buffer, descriptor->VendorIdOffset);
        const auto product = string_at(buffer, descriptor->ProductIdOffset);
        device.model = product.empty() ? vendor : product;
        device.serial = string_at(buffer, descriptor->SerialNumberOffset);
        device.bus = bus_name(descriptor->BusType);
        if (const auto wwn = nvme_wwn_from_windows_serial(device.serial)) {
            device.wwn = *wwn;
            device.serial.clear();
        }
    }

    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek_penalty{};
    if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
            &seek_penalty, sizeof(seek_penalty), &returned, nullptr) &&
        returned >= sizeof(seek_penalty)) {
        device.rotational = seek_penalty.IncursSeekPenalty != FALSE;
    }

    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
            sizeof(geometry), &returned, nullptr)) {
        device.capacity_bytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
    }
    return device;
}

// "\\?\Volume{GUID}\" -> the disks holding its extents (several for spanned volumes).
std::vector<PhysicalDeviceInfo> describe_devices(const std::wstring& volume_name)
{
    std::wstring device_name = volume_name;
    if (!device_name.empty() && device_name.back() == L'\\') {
        device_name.pop_back();
    }
    const auto volume = open_for_query(device_name);
    if (!volume.valid()) {
        return {};
    }
    std::vector<BYTE> buffer(sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT));
    DWORD returned = 0;
    if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
            buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        return {};
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    std::vector<PhysicalDeviceInfo> devices;
    std::vector<DWORD> seen;
    for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
        const DWORD disk = extents->Extents[index].DiskNumber;
        if (std::find(seen.begin(), seen.end(), disk) == seen.end()) {
            seen.push_back(disk);
            devices.push_back(describe_disk(disk));
        }
    }
    return devices;
}

// The 64-bit NTFS serial (§B1.3). FILE_ID_INFO on the root directory is
// available to any user; FSCTL_GET_NTFS_VOLUME_DATA is the fallback.
std::optional<std::uint64_t> ntfs_serial_64(const std::wstring& mount_point,
    const std::wstring& volume_name, DWORD serial_32)
{
    UniqueHandle root(CreateFileW(mount_point.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (root.valid()) {
        FILE_ID_INFO information{};
        if (GetFileInformationByHandleEx(root.get(), FileIdInfo, &information, sizeof(information)) &&
            static_cast<DWORD>(information.VolumeSerialNumber) == serial_32 &&
            (information.VolumeSerialNumber >> 32) != 0) {
            return information.VolumeSerialNumber;
        }
    }

    std::wstring device_name = volume_name;
    if (!device_name.empty() && device_name.back() == L'\\') {
        device_name.pop_back();
    }
    const auto volume = open_for_query(device_name);
    NTFS_VOLUME_DATA_BUFFER data{};
    DWORD returned = 0;
    if (volume.valid() && DeviceIoControl(volume.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                              &data, sizeof(data), &returned, nullptr)) {
        return static_cast<std::uint64_t>(data.VolumeSerialNumber.QuadPart);
    }
    return std::nullopt;
}

std::vector<std::wstring> mount_points_of(const std::wstring& volume_name)
{
    std::vector<wchar_t> buffer(1024);
    DWORD length = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume_name.c_str(), buffer.data(),
            static_cast<DWORD>(buffer.size()), &length)) {
        if (GetLastError() != ERROR_MORE_DATA) {
            return {};
        }
        buffer.resize(length);
        if (!GetVolumePathNamesForVolumeNameW(volume_name.c_str(), buffer.data(),
                static_cast<DWORD>(buffer.size()), &length)) {
            return {};
        }
    }
    std::vector<std::wstring> result;
    for (const wchar_t* entry = buffer.data(); *entry; entry += wcslen(entry) + 1) {
        result.emplace_back(entry);
    }
    return result;
}

std::expected<VolumeInfo, IoError> describe_volume(const std::wstring& volume_name,
    const std::wstring& mount_point)
{
    wchar_t label[MAX_PATH + 1]{};
    wchar_t filesystem[MAX_PATH + 1]{};
    DWORD serial = 0;
    DWORD flags = 0;
    if (!GetVolumeInformationW(volume_name.c_str(), label, MAX_PATH + 1, &serial, nullptr, &flags,
            filesystem, MAX_PATH + 1)) {
        return std::unexpected(error_from_win32(GetLastError(), "GetVolumeInformationW"));
    }

    VolumeInfo volume;
    volume.mount_point = std::filesystem::path(mount_point);
    volume.volume_label = wide_to_utf8(label);
    volume.filesystem_type = normalise_filesystem_type(wide_to_utf8(filesystem));
    volume.read_only = (flags & FILE_READ_ONLY_VOLUME) != 0;

    std::string raw_id = std::format("{:08x}", serial);
    if (volume.filesystem_type == "ntfs") {
        if (const auto serial_64 = ntfs_serial_64(mount_point, volume_name, serial)) {
            raw_id = std::format("{:016x}", *serial_64);
        }
    }
    const auto id = normalise_filesystem_id(volume.filesystem_type, raw_id);
    volume.filesystem_uuid = id.uuid;
    volume.filesystem_uuid_short = id.uuid_short;

    const auto guid_start = volume_name.find(L"Volume{");
    if (guid_start != std::wstring::npos) {
        auto guid = volume_name.substr(guid_start + 6);
        if (!guid.empty() && guid.back() == L'\\') {
            guid.pop_back();
        }
        volume.volume_guid = wide_to_utf8(guid);
    }

    ULARGE_INTEGER total{};
    if (GetDiskFreeSpaceExW(mount_point.c_str(), nullptr, &total, nullptr)) {
        volume.capacity_bytes = total.QuadPart;
    }
    volume.devices = describe_devices(volume_name);
    return volume;
}

class WindowsMediaEnumerator final : public IMediaEnumerator {
public:
    std::expected<std::vector<VolumeInfo>, IoError> volumes() override
    {
        std::vector<VolumeInfo> result;
        wchar_t name[MAX_PATH + 1]{};
        const HANDLE search = FindFirstVolumeW(name, MAX_PATH + 1);
        if (search == INVALID_HANDLE_VALUE) {
            return std::unexpected(error_from_win32(GetLastError(), "FindFirstVolumeW"));
        }
        std::vector<windows::VolumeNames> found;
        do {
            const std::wstring volume_name(name);
            found.push_back({volume_name, mount_points_of(volume_name)});
        } while (FindNextVolumeW(search, name, MAX_PATH + 1));
        FindVolumeClose(search);
        // Also a volume that has no drive letter or folder path, under its
        // GUID path (decision 0029, point 3).
        for (const auto& [volume_name, mount_point] : windows::volumes_to_list(found)) {
            if (auto volume = describe_volume(volume_name, mount_point)) {
                result.push_back(std::move(*volume));
            }
        }
        return result;
    }

    std::expected<VolumeInfo, IoError> volume_for_path(const std::filesystem::path& path) override
    {
        std::vector<wchar_t> mount(32768);
        if (!GetVolumePathNameW(path.c_str(), mount.data(), static_cast<DWORD>(mount.size()))) {
            return std::unexpected(error_from_win32(GetLastError(), "GetVolumePathNameW"));
        }
        const std::wstring mount_point(mount.data());
        wchar_t volume_name[MAX_PATH + 1]{};
        if (!GetVolumeNameForVolumeMountPointW(mount_point.c_str(), volume_name, MAX_PATH + 1)) {
            return std::unexpected(
                error_from_win32(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
        }
        return describe_volume(volume_name, mount_point);
    }
};

} // namespace

std::unique_ptr<IMediaEnumerator> make_media_enumerator()
{
    return std::make_unique<WindowsMediaEnumerator>();
}

} // namespace va::platform
