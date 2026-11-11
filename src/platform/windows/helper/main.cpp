// The device identity reader, veriarchive-device-identity.exe (decisions
// 0047 and 0052): the one program of VeriArchive that runs with
// administrator permission. Its manifest asks Windows for it
// (requireAdministrator), so Windows starts it only once the operator allows
// it; the window starts it on the operator's request
// (platform::make_device_identity_helper), never itself elevated (R-12).
// It reads, of each device it is given, the device's own identity and its
// SMART, where the device's enclosure reports its own serial in place of the
// device's and the system shows no SMART without that permission:
//   - an ATA device: IDENTIFY DEVICE, SMART READ DATA and SMART READ
//     THRESHOLDS, which return what the device says of itself and change
//     nothing, through ATA pass-through, and where that fails inside the
//     SCSI command ATA PASS-THROUGH (16), as USB bridges take it;
//   - an NVMe device: Identify Controller and the SMART / Health Information
//     log, through the storage driver (IOCTL_STORAGE_QUERY_PROPERTY).
// The devices are read side by side, so that sleeping hard disks spin up
// together. No other command, nothing read of the devices' content, nothing
// written to them, no repository, no window. It writes the pages as the
// devices returned them; the window decodes them, unprivileged.
//
//   veriarchive-device-identity.exe <result file> <device number>...
//
// <result file>: a full path of a .json file that is not there yet; the
// reader makes it new and writes its result there (device_identity_reader.h).
// <device number>: 0 to 63, the N of \\.\PhysicalDriveN, nothing else; one to
// 64 of them, each once.
// Exit status: 0 the result is written, with at least one device read; 1 the
// result is written, and no device could be read; 2 the arguments are
// refused, nothing is written and no device is opened; 3 the result file
// cannot be made (it is there, or it is not where its path says; no device is
// then opened), or cannot be written.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include "platform/windows/device_identity_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace va::platform::windows;

constexpr int some_read = 0;
constexpr int none_read = 1;
constexpr int arguments_refused = 2;
constexpr int result_refused = 3;

// The time each command may take, in seconds: a sleeping hard disk behind a
// bridge may first spin up. A device takes four commands at most, which fit
// in the time that the program which started the reader waits for it
// (device_identity_helper_win.cpp, 120 s).
constexpr ULONG command_time_limit = 15;

class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_;
};

// The path by which the file system names the file `handle` is open on, in
// the \\?\ form with its drive letter.
std::optional<std::wstring> final_path_of(HANDLE handle)
{
    std::vector<wchar_t> buffer(512);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(length + 1);
    }
    return std::nullopt;
}

// Makes the result file new, and keeps it only where it is the file its path
// names: a link or junction on the way that led elsewhere has it removed
// again at once. Nothing where it cannot be made so.
std::optional<HANDLE> make_result_file(std::wstring_view path)
{
    const std::wstring name(path);
    const HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const std::wstring wanted = name.starts_with(L"\\\\?\\") ? name : L"\\\\?\\" + name;
    const auto final_path = final_path_of(file);
    if (!final_path ||
        CompareStringOrdinal(final_path->c_str(), static_cast<int>(final_path->size()), wanted.c_str(),
            static_cast<int>(wanted.size()), TRUE) != CSTR_EQUAL) {
        FILE_DISPOSITION_INFO remove{};
        remove.DeleteFile = TRUE;
        SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
        CloseHandle(file);
        return std::nullopt;
    }
    return file;
}

int write_result(HANDLE file, const std::string& result, int status)
{
    DWORD written = 0;
    const bool whole = WriteFile(file, result.data(), static_cast<DWORD>(result.size()), &written, nullptr) &&
                       written == result.size();
    CloseHandle(file);
    return whole ? status : result_refused;
}

// One ATA command that reads one 512-byte block from the device, through ATA
// pass-through, as a SATA controller takes it: IDENTIFY DEVICE (`feature`
// 0), or SMART with `feature`. `error`: the Win32 error where the request
// failed, 0 where the device refused the command. Nothing where it failed.
std::optional<std::vector<std::uint8_t>> by_ata_pass_through(
    HANDLE device, std::uint8_t command, std::uint8_t feature, DWORD& error)
{
    struct Request {
        ATA_PASS_THROUGH_EX header;
        alignas(16) unsigned char data[identify_data_size];
    };
    Request request;
    std::memset(&request, 0, sizeof(request));
    request.header.Length = sizeof(ATA_PASS_THROUGH_EX);
    request.header.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    request.header.DataTransferLength = identify_data_size;
    request.header.TimeOutValue = command_time_limit;
    request.header.DataBufferOffset = offsetof(Request, data);
    if (command == smart_command) {
        request.header.CurrentTaskFile[0] = feature;          // the features register
        request.header.CurrentTaskFile[1] = 1;                // the sector count
        request.header.CurrentTaskFile[3] = smart_lba_mid;    // the cylinder low register
        request.header.CurrentTaskFile[4] = smart_lba_high;   // the cylinder high register
    }
    request.header.CurrentTaskFile[6] = command;              // the command register
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_ATA_PASS_THROUGH, &request, sizeof(request), &request, sizeof(request),
            &returned, nullptr)) {
        error = GetLastError();
        return std::nullopt;
    }
    error = 0;
    // Back from the device, the command register holds its status: ERR
    // (bit 0) says it refused the command.
    if ((request.header.CurrentTaskFile[6] & 0x01) != 0) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::begin(request.data), std::end(request.data));
}

// The command `cdb` inside ATA PASS-THROUGH (16), as a USB bridge that
// follows SAT takes it. `error` as above; 0 also where the bridge answered
// with a SCSI status other than GOOD, as one that does not pass the command
// on does.
std::optional<std::vector<std::uint8_t>> by_sat(HANDLE device, const std::array<std::uint8_t, 16>& cdb, DWORD& error)
{
    struct Request {
        SCSI_PASS_THROUGH header;
        alignas(16) unsigned char sense[32];
        alignas(16) unsigned char data[identify_data_size];
    };
    Request request;
    std::memset(&request, 0, sizeof(request));
    request.header.Length = sizeof(SCSI_PASS_THROUGH);
    request.header.CdbLength = 16;
    request.header.SenseInfoLength = sizeof(request.sense);
    request.header.DataIn = SCSI_IOCTL_DATA_IN;
    request.header.DataTransferLength = identify_data_size;
    request.header.TimeOutValue = command_time_limit;
    request.header.SenseInfoOffset = offsetof(Request, sense);
    request.header.DataBufferOffset = offsetof(Request, data);
    std::memcpy(request.header.Cdb, cdb.data(), cdb.size());
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_SCSI_PASS_THROUGH, &request, sizeof(request), &request, sizeof(request),
            &returned, nullptr)) {
        error = GetLastError();
        return std::nullopt;
    }
    error = 0;
    if (request.header.ScsiStatus != 0) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::begin(request.data), std::end(request.data));
}

// The bus the storage driver names the device's: what tells an NVMe device.
std::optional<STORAGE_BUS_TYPE> bus_of(HANDLE device)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
            sizeof(descriptor), &returned, nullptr) ||
        returned < offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(descriptor.BusType)) {
        return std::nullopt;
    }
    return descriptor.BusType;
}

// What the NVMe protocol gives of `data_type` (Identify, a log page) for
// `request` (its CNS, its log identifier), `length` bytes, through the
// storage driver. `error` as above; 0 where the driver answered with less.
std::optional<std::vector<std::uint8_t>> by_nvme(
    HANDLE device, STORAGE_PROPERTY_ID property, DWORD data_type, DWORD request, std::size_t length, DWORD& error)
{
    const std::size_t head = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    const std::size_t size = head + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + length;
    // 8-byte aligned, as the structures inside ask.
    std::vector<std::uint64_t> storage((size + 7) / 8, 0);
    auto* bytes = reinterpret_cast<unsigned char*>(storage.data());
    auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(bytes);
    query->PropertyId = property;
    query->QueryType = PropertyStandardQuery;
    auto* asked = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(bytes + head);
    asked->ProtocolType = ProtocolTypeNvme;
    asked->DataType = data_type;
    asked->ProtocolDataRequestValue = request;
    asked->ProtocolDataRequestSubValue = 0;
    asked->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    asked->ProtocolDataLength = static_cast<DWORD>(length);
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, bytes, static_cast<DWORD>(size), bytes,
            static_cast<DWORD>(size), &returned, nullptr)) {
        error = GetLastError();
        return std::nullopt;
    }
    error = 0;
    const auto* descriptor = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(bytes);
    if (descriptor->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
        descriptor->Size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) {
        return std::nullopt;
    }
    const auto& answered = descriptor->ProtocolSpecificData;
    const std::size_t from = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) +
                             answered.ProtocolDataOffset;
    if (answered.ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || answered.ProtocolDataLength < length ||
        from + length > size) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(bytes + from, bytes + from + length);
}

bool all_zero(const std::vector<std::uint8_t>& data)
{
    return std::ranges::all_of(data, [](std::uint8_t byte) { return byte == 0; });
}

bool identifies(const std::optional<std::vector<std::uint8_t>>& data)
{
    return data && decode_identify_data(std::as_bytes(std::span(*data))).has_value();
}

// An NVMe device: its Identify Controller data and its health log.
void read_nvme(HANDLE device, ReaderDevice& result)
{
    DWORD error = 0;
    auto identify = by_nvme(device, StorageAdapterProtocolSpecificProperty, NVMeDataTypeIdentify,
        nvme_identify_controller, nvme_identify_data_size, error);
    if (!identify || all_zero(*identify)) {
        result.error = "not-passed";
        result.code = error;
        return;
    }
    result.how = "nvme";
    result.identify = std::move(*identify);
    auto health = by_nvme(device, StorageDeviceProtocolSpecificProperty, NVMeDataTypeLogPage, nvme_health_log,
        nvme_health_log_size, error);
    if (!health || all_zero(*health)) {
        result.smart_error = "not-passed";
        result.smart_code = error;
        return;
    }
    result.smart = std::move(*health);
}

// An ATA device, directly or behind a bridge: the way IDENTIFY DEVICE came
// back by carries the SMART reads too.
void read_ata(HANDLE device, ReaderDevice& result)
{
    DWORD error = 0;
    auto identify = by_ata_pass_through(device, identify_device_command, 0, error);
    bool sat = false;
    if (!identifies(identify)) {
        identify = by_sat(device, identify_device_cdb(), error);
        sat = true;
    }
    if (!identifies(identify)) {
        result.error = "not-passed";
        result.code = error;
        return;
    }
    result.how = sat ? "sat" : "ata";
    result.identify = std::move(*identify);
    const auto smart = [&](std::uint8_t feature) {
        return sat ? by_sat(device, smart_cdb(feature), error)
                   : by_ata_pass_through(device, smart_command, feature, error);
    };
    auto data = smart(smart_read_data);
    if (!data || all_zero(*data)) {
        result.smart_error = "not-passed";
        result.smart_code = error;
        return;
    }
    result.smart = std::move(*data);
    if (auto thresholds = smart(smart_read_thresholds); thresholds && !all_zero(*thresholds)) {
        result.thresholds = std::move(*thresholds);
    }
}

ReaderDevice read_device(unsigned number)
{
    ReaderDevice result;
    result.number = number;
    // The pass-through requests take a handle with read and write access
    // (their control codes say so). Nothing of the device's content is read
    // or written through it: the commands sent only return what the device
    // says of itself.
    const std::wstring name = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    const Handle device(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!device.valid()) {
        result.error = "open-failed";
        result.code = GetLastError();
        return result;
    }
    if (bus_of(device.get()) == BusTypeNvme) {
        read_nvme(device.get(), result);
    } else {
        read_ata(device.get(), result);
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3 || !output_path_argument(argv[1])) {
        return arguments_refused;
    }
    std::vector<std::wstring_view> number_arguments(argv + 2, argv + argc);
    const auto numbers = device_numbers_argument(number_arguments);
    if (!numbers) {
        return arguments_refused;
    }
    // The result file first: where it cannot be made, no device is opened.
    const auto file = make_result_file(argv[1]);
    if (!file) {
        return result_refused;
    }

    std::vector<ReaderDevice> results(numbers->size());
    {
        std::vector<std::jthread> readers;
        for (std::size_t index = 0; index < numbers->size(); ++index) {
            readers.emplace_back([&results, index, number = (*numbers)[index]] { results[index] = read_device(number); });
        }
    }
    const bool any = std::ranges::any_of(results, [](const ReaderDevice& device) { return device.error.empty(); });
    return write_result(*file, reader_result_json(results), any ? some_read : none_read);
}
